#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

#define MSG_TELEMETRIA 1
#define MSG_ACK 2
#define MSG_EQUIPE_DRONE 3
#define MSG_CONCLUSAO 4

int usa_ipv4 = 0; // 1 = usa IPv4, 0 = usa IPv6
int n_cidades;

/* estruturas disponibilizadas no enunciado */
typedef struct {
    uint16_t tipo;
    uint16_t tamanho;
} header_t;

typedef struct {
    int id_cidade;
    int status;
} telemetria_t;

typedef struct {
    int total;
    telemetria_t dados[50];
} payload_telemetria_t;

typedef struct {
    int status;
} payload_ack_t;

typedef struct {
    int id_cidade;
    int id_equipe;
} payload_equipe_drone_t;

typedef struct {
    int id_cidade;
    time_t timestamp;
    int equipe_atuando;
} alerta_t;

typedef struct Cidade {
    int _idx;
    char _nome[100];
    int _tipo;
} Cidade;

/* leitura do arquivo */
Cidade *ler_arquivo(FILE *f) {
    int N, M;
    fscanf(f, "%d %d", &N, &M);
    fgetc(f);
    n_cidades = N;

    Cidade *c = malloc(N * sizeof(Cidade));
    char linha[256];
    int idx, tipo;
    char nome[200];
    for (int k = 0; k < N; k++) {
        fgets(linha, sizeof(linha), f);
        sscanf(linha, "%d %[^0-9] %d", &idx, nome, &tipo);
        int len = strlen(nome);
        if (len > 0 && nome[len - 1] == ' ') nome[len - 1] = '\0';
        c[idx]._idx = idx;
        strcpy(c[idx]._nome, nome);
        c[idx]._tipo = tipo;
    }
    return c;
}

/* sockets / endereços */
struct sockaddr_in addr4;
struct sockaddr_in6 addr6;
int sockfd;

/* sincronização */
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
payload_telemetria_t ultima_telemetria;

alerta_t alerta_global = { .id_cidade = -1, .timestamp = 0, .equipe_atuando = 0 };
int alerta_ativo = 0;

pthread_cond_t cond_ack = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_ack = PTHREAD_MUTEX_INITIALIZER;
int last_ack_status = -1;

/* missão */
typedef struct {
    int id_cidade;
    int id_equipe;
    int ativa;
    int ocupada;
} mission_t;
mission_t current_mission;
pthread_cond_t cond_mission = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_mission = PTHREAD_MUTEX_INITIALIZER;

/* enviar pacote */
ssize_t send_packet(const void *buf, size_t len) {
    if (usa_ipv4) {
        return sendto(sockfd, buf, len, 0, (struct sockaddr *)&addr4, sizeof(addr4));
    } else {
        return sendto(sockfd, buf, len, 0, (struct sockaddr *)&addr6, sizeof(addr6));
    }
}

/* Thread monitoramento */
void *thread_monitoramento(void *arg) {
    Cidade *cidades = (Cidade *)arg;
    srand((unsigned int)time(NULL) ^ (unsigned int)pthread_self());
    while (1) {
        sleep(5);
        pthread_mutex_lock(&lock);
        ultima_telemetria.total = n_cidades;
        for (int i = 0; i < n_cidades && i < 50; i++) {
            ultima_telemetria.dados[i].id_cidade = i;
            int r = rand() % 100;
            if (r < 3) {
                ultima_telemetria.dados[i].status = 1;
                alerta_ativo = 1;
                alerta_global.id_cidade = i;
                alerta_global.timestamp = time(NULL);
                alerta_global.equipe_atuando = 0;
            } else {
                ultima_telemetria.dados[i].status = 0;
            }
        }
        pthread_mutex_unlock(&lock);
    }
}

/* Thread envia telemetria */
void *thread_envia_telemetria(void *arg) {
    Cidade *cidades = (Cidade *)arg;
    (void)arg;
    while (1) {
        sleep(30);

        pthread_mutex_lock(&lock);
        payload_telemetria_t pl = ultima_telemetria;
        pthread_mutex_unlock(&lock);

        // monta payload com conversão para network order
        payload_telemetria_t net_pl;
        net_pl.total = htonl(pl.total);
        for (int i = 0; i < pl.total && i < 50; i++) {
            net_pl.dados[i].id_cidade = htonl(pl.dados[i].id_cidade);
            net_pl.dados[i].status = htonl(pl.dados[i].status);
        }

        header_t h;
        h.tipo = htons(MSG_TELEMETRIA);
        h.tamanho = htons((uint16_t)sizeof(net_pl));

        uint8_t buffer[sizeof(header_t) + sizeof(net_pl)];
        memcpy(buffer, &h, sizeof(h));
        memcpy(buffer + sizeof(h), &net_pl, sizeof(net_pl));

        // prints conforme enunciado
        printf("\n[ENVIANDO TELEMETRIA]\n");
        printf("Total de cidades: %d\n", pl.total);
        for (int i = 0; i < pl.total && i < 50; i++) {
            if (pl.dados[i].status == 1) {
                printf("ALERTA: %s (ID=%d)\n", cidades[pl.dados[i].id_cidade]._nome, pl.dados[i].id_cidade);
            }
        }

        int tries = 0;
        int ack_received = 0;
        while (tries < 3 && !ack_received) {
            printf("-> Telemetria enviada (tentativa %d/3)\n", tries + 1);
            ssize_t sent = send_packet(buffer, sizeof(buffer));
            if (sent < 0) {
                perror("sendto telemetria");
                break;
            }

            // espera ACK status==0
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;

            pthread_mutex_lock(&lock_ack);
            last_ack_status = -1;
            int rc = 0;
            while (last_ack_status == -1 && rc == 0) {
                rc = pthread_cond_timedwait(&cond_ack, &lock_ack, &ts);
            }
            if (rc == 0 && last_ack_status == 0) {
                ack_received = 1;
            }
            pthread_mutex_unlock(&lock_ack);

            if (!ack_received) tries++;
        }

        if (ack_received) {
            printf(". ACK recebido do servidor\n");
        } else {
            fprintf(stderr, "Telemetria: sem ACK após 3 tentativas\n");
        }
    }
}

/* Thread de recepção */
void *thread_recebe(void *arg) {
    Cidade *cidades = (Cidade *)arg;
    (void)arg;
    uint8_t buffer[2048];
    while (1) {
        ssize_t len = recv(sockfd, buffer, sizeof(buffer), 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            continue;
        }
        if (len < (ssize_t)sizeof(header_t)) continue;

        header_t h;
        memcpy(&h, buffer, sizeof(h));
        uint16_t tipo = ntohs(h.tipo);
        uint16_t tamanho = ntohs(h.tamanho);
        uint8_t *payload = buffer + sizeof(header_t);

        if (tipo == MSG_EQUIPE_DRONE) {
            if (tamanho >= sizeof(payload_equipe_drone_t)) {
                payload_equipe_drone_t p;
                memcpy(&p, payload, sizeof(p));
                int id_cidade = ntohl(p.id_cidade);
                int id_equipe = ntohl(p.id_equipe);

                printf("\n[ORDEM DE DRONE RECEBIDA]\n");
                printf("Cidade : %s (ID=%d)\n", cidades[id_cidade]._nome, id_cidade);
                printf("Equipe : %s (ID=%d)\n", cidades[id_equipe]._nome, id_equipe);

                // envia ACK (status=1) ao servidor (em network order)
                header_t ack_h;
                payload_ack_t ack_p;
                ack_h.tipo = htons(MSG_ACK);
                ack_h.tamanho = htons(sizeof(ack_p));
                ack_p.status = htonl(1);

                uint8_t ack_buf[sizeof(header_t) + sizeof(ack_p)];
                memcpy(ack_buf, &ack_h, sizeof(ack_h));
                memcpy(ack_buf + sizeof(ack_h), &ack_p, sizeof(ack_p));
                send_packet(ack_buf, sizeof(ack_buf));
                printf("-> ACK enviado ao servidor\n");

                // registra missão se possível
                pthread_mutex_lock(&lock_mission);
                if (current_mission.ocupada) {
                    printf("Já existe missão ativa, ordem ignorada\n");
                } else {
                    current_mission.id_cidade = id_cidade;
                    current_mission.id_equipe = id_equipe;
                    current_mission.ativa = 1;
                    current_mission.ocupada = 1;
                    pthread_cond_signal(&cond_mission);
                    printf("-> Missão registrada para execução\n");
                }
                pthread_mutex_unlock(&lock_mission);
            }
        } else if (tipo == MSG_ACK) {
            if (tamanho >= sizeof(payload_ack_t)) {
                payload_ack_t ap;
                memcpy(&ap, payload, sizeof(ap));
                int status = ntohl(ap.status);
                pthread_mutex_lock(&lock_ack);
                last_ack_status = status;
                pthread_cond_signal(&cond_ack);
                pthread_mutex_unlock(&lock_ack);
                // log opcional:
                //printf("[DEBUG] MSG_ACK status=%d\n", status);
            }
        } else {
            // outros
        }
    }
}

/* Thread atuação (drones) */
void *thread_atuacao(void *arg) {
    Cidade *cidades = (Cidade *)arg;
    (void)arg;
    while (1) {
        pthread_mutex_lock(&lock_mission);
        while (!current_mission.ativa) {
            pthread_cond_wait(&cond_mission, &lock_mission);
        }
        int id_cidade = current_mission.id_cidade;
        int id_equipe = current_mission.id_equipe;
        current_mission.ativa = 0;
        pthread_mutex_unlock(&lock_mission);

        printf("\n[MISSÃO EM ANDAMENTO]\n");
        printf("Equipe %s atuando em %s\n", cidades[id_equipe]._nome, cidades[id_cidade]._nome);
        srand((unsigned int)time(NULL) ^ (unsigned int)pthread_self());
        int dur = rand() % 31;
        printf(". Tempo estimado : %d segundos\n", dur > 0 ? dur : 1);
        sleep(dur > 0 ? dur : 1);
        printf(". Missão concluída!\n");

        // envia MSG_CONCLUSAO
        header_t h;
        payload_equipe_drone_t concl;
        h.tipo = htons(MSG_CONCLUSAO);
        h.tamanho = htons(sizeof(concl));
        concl.id_cidade = htonl(id_cidade);
        concl.id_equipe = htonl(id_equipe);

        uint8_t buf[sizeof(h) + sizeof(concl)];
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), &concl, sizeof(concl));

        int tries = 0;
        int ack_recebido = 0;
        while (tries < 3 && !ack_recebido) {
            ssize_t s = send_packet(buf, sizeof(buf));
            if (s < 0) {
                perror("sendto conclusão");
                break;
            }
            printf("-> Conclusão enviada ao servidor (tentativa %d/3)\n", tries + 1);

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 5;

            pthread_mutex_lock(&lock_ack);
            last_ack_status = -1;
            int rc = 0;
            while (last_ack_status == -1 && rc == 0) {
                rc = pthread_cond_timedwait(&cond_ack, &lock_ack, &ts);
            }
            if (rc == 0 && last_ack_status == 2) ack_recebido = 1;
            pthread_mutex_unlock(&lock_ack);

            if (!ack_recebido) tries++;
        }

        if (ack_recebido) {
            printf("-> ACK de encerramento recebido do servidor\n");
        } else {
            fprintf(stderr, "Conclusão: sem ACK do servidor após 3 tentativas. Liberando equipe localmente.\n");
        }

        pthread_mutex_lock(&lock_mission);
        current_mission.ocupada = 0;
        pthread_mutex_unlock(&lock_mission);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s v4|v6\n", argv[0]);
        return 1;
    }

    FILE *f = fopen("grafo_amazonia_legal.txt", "r");
    if (!f) {
        perror("Erro ao abrir arquivo");
        return 1;
    }

    Cidade *cidades = ler_arquivo(f);
    fclose(f);

    char *protocolo = argv[1];
    int porta = 8080;

    if (strcmp(protocolo, "v4") == 0) {
        usa_ipv4 = 1;
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) { perror("socket"); return 1; }
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(porta);
        inet_pton(AF_INET, "127.0.0.1", &addr4.sin_addr);
        printf("Conectado ao servidor 127.0.0.1:%d\n\n", porta);
    } else {
        usa_ipv4 = 0;
        sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
        if (sockfd < 0) { perror("socket"); return 1; }
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(porta);
        inet_pton(AF_INET6, "::1", &addr6.sin6_addr);
        printf("Conectado ao servidor ::1:%d\n\n", porta);
    }

    pthread_mutex_lock(&lock);
    ultima_telemetria.total = 0;
    pthread_mutex_unlock(&lock);

    current_mission.ativa = 0;
    current_mission.ocupada = 0;

    printf("Iniciando threads...\n\n");
    printf("[ Thread Monitoramento ] Iniciada\n");
    printf("[ Thread Simulação Drones ] Iniciada\n");
    printf("[ Thread Telemetria ] Iniciada\n");
    printf("[ Thread Recepção Drones ] Iniciada\n");
    printf(". Todas as threads iniciadas com sucesso\n");
    printf("Pressione Ctrl+C para encerrar...\n");

    pthread_t t1, t2, trecv, t4;
    pthread_create(&t1, NULL, thread_monitoramento, (void *)cidades);
    pthread_create(&t2, NULL, thread_envia_telemetria, (void *)cidades);
    pthread_create(&trecv, NULL, thread_recebe, (void *)cidades);
    pthread_create(&t4, NULL, thread_atuacao, (void *)cidades);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(trecv, NULL);
    pthread_join(t4, NULL);

    free(cidades);
    close(sockfd);
    return 0;
}