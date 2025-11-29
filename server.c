#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>
#include <limits.h>

#define MSG_TELEMETRIA 1
#define MSG_ACK 2
#define MSG_EQUIPE_DRONE 3
#define MSG_CONCLUSAO 4

/*
 estruturas disponibilizadas no enunciado
 */
typedef struct {
    uint16_t tipo;
    uint16_t tamanho;
} header_t;

typedef struct {
    int id_cidade;
    int status; // 0 = OK, 1 = ALERTA
} telemetria_t;

typedef struct {
    int total;
    telemetria_t dados[50];
} payload_telemetria_t;

typedef struct {
    int status; // 0=ACK TELEMETRIA, 1=ACK EQUIPE, 2=ACK CONCLUSÃO
} payload_ack_t;

typedef struct {
    int id_cidade;
    int id_equipe;
} payload_equipe_drone_t;

typedef struct {
    int id_cidade;
    time_t timestamp;
    int equipe_atuando; // -1 = nenhuma, >=0 = id equipe alocada
} alerta_t;

alerta_t alertas[100];
int total_alertas = 0;

/* Grafo */
typedef struct Edge {
    int to;
    int peso;
    struct Edge *next;
} Edge;

typedef struct Node {
    int _idx;
    char _nome[100];
    int _tipo;
    int _status;
    int _ocupada;
    Edge *_adj;
    int _grau;
} Node;

typedef struct Grafo {
    int n;
    int m;
    Node *nodes;
} Grafo;

void adiciona_aresta(Grafo *g, int u, int v, int p) {
    Edge *e1 = malloc(sizeof(Edge));
    e1->to = v;
    e1->peso = p;
    e1->next = g->nodes[u]._adj;
    g->nodes[u]._adj = e1;

    Edge *e2 = malloc(sizeof(Edge));
    e2->to = u;
    e2->peso = p;
    e2->next = g->nodes[v]._adj;
    g->nodes[v]._adj = e2;

    g->nodes[u]._grau++;
    g->nodes[v]._grau++;
}

Grafo *cria_grafo(FILE *f) {
    int N, M;
    fscanf(f, "%d %d", &N, &M);
    fgetc(f);
    Grafo *g = malloc(sizeof(Grafo));
    g->n = N;
    g->m = M;
    g->nodes = malloc(N * sizeof(Node));
    for (int i = 0; i < N; i++) {
        g->nodes[i]._idx = i;
        g->nodes[i]._nome[0] = '\0';
        g->nodes[i]._tipo = -1;
        g->nodes[i]._status = 0;
        g->nodes[i]._ocupada = 0;
        g->nodes[i]._adj = NULL;
        g->nodes[i]._grau = 0;
    }

    char linha[256];
    int idx, tipo;
    char nome[200];
    for (int k = 0; k < N; k++) {
        fgets(linha, sizeof(linha), f);
        sscanf(linha, "%d %[^0-9] %d", &idx, nome, &tipo);
        int len = strlen(nome);
        if (len > 0 && nome[len - 1] == ' ') nome[len - 1] = '\0';
        g->nodes[idx]._idx = idx;
        strcpy(g->nodes[idx]._nome, nome);
        g->nodes[idx]._tipo = tipo;
    }

    for (int i = 0; i < M; i++) {
        int u, v, p;
        fscanf(f, "%d %d %d", &u, &v, &p);
        adiciona_aresta(g, u, v, p);
    }

    return g;
}

/* registrar alerta */
void registrar_alerta(int id_cidade) {
    if (total_alertas >= 100) return;
    alerta_t *a = &alertas[total_alertas++];
    a->id_cidade = id_cidade;
    a->timestamp = time(NULL);
    a->equipe_atuando = -1;
}

/* Dijkstra: além de retornar índice da melhor equipe, retorna distância via out_dist */
int dijkstra_escolhe_equipe(Grafo *g, int origem, int *out_dist) {
    int n = g->n;
    int dist[n];
    int usado[n];

    for (int i = 0; i < n; i++) {
        dist[i] = INT_MAX;
        usado[i] = 0;
    }
    dist[origem] = 0;

    for (int iter = 0; iter < n; iter++) {
        int v = -1;
        for (int i = 0; i < n; i++) {
            if (!usado[i] && (v == -1 || dist[i] < dist[v])) v = i;
        }
        if (v == -1 || dist[v] == INT_MAX) break;
        usado[v] = 1;
        for (Edge *e = g->nodes[v]._adj; e != NULL; e = e->next) {
            int u = e->to;
            int peso = e->peso;
            if (dist[v] != INT_MAX && dist[v] + peso < dist[u]) {
                dist[u] = dist[v] + peso;
            }
        }
    }

    int melhor_idx = -1;
    int melhor_dist = INT_MAX;
    for (int i = 0; i < n; i++) {
        Node *node = &g->nodes[i];
        if (node->_tipo == 1 && node->_ocupada == 0) {
            if (dist[i] < melhor_dist) {
                melhor_dist = dist[i];
                melhor_idx = i;
            }
        }
    }

    if (melhor_idx == -1 || melhor_dist == INT_MAX) {
        if (out_dist) *out_dist = -1;
        return -1;
    }

    g->nodes[melhor_idx]._ocupada = 1;
    if (out_dist) *out_dist = melhor_dist;
    return g->nodes[melhor_idx]._idx;
}

/* Envia ACK (payload.status em network order) */
void send_ack(int sockfd, struct sockaddr_storage *client_addr, socklen_t client_len, int status) {
    header_t h;
    payload_ack_t ack;
    h.tipo = htons(MSG_ACK);
    h.tamanho = htons((uint16_t)sizeof(payload_ack_t));
    ack.status = htonl(status);

    uint8_t buffer[sizeof(header_t) + sizeof(payload_ack_t)];
    memcpy(buffer, &h, sizeof(h));
    memcpy(buffer + sizeof(h), &ack, sizeof(ack));

    sendto(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)client_addr, client_len);
}

/* Envia mensagem de equipe (os campos já devem estar em network order no payload) */
ssize_t enviar_msg_equipe(int sockfd, struct sockaddr_storage *client_addr, socklen_t client_len, int id_cidade, int id_equipe) {
    header_t h;
    payload_equipe_drone_t p;
    h.tipo = htons(MSG_EQUIPE_DRONE);
    h.tamanho = htons((uint16_t)sizeof(payload_equipe_drone_t));
    p.id_cidade = htonl(id_cidade);
    p.id_equipe = htonl(id_equipe);

    uint8_t buffer[sizeof(header_t) + sizeof(payload_equipe_drone_t)];
    memcpy(buffer, &h, sizeof(h));
    memcpy(buffer + sizeof(h), &p, sizeof(p));
    return sendto(sockfd, buffer, sizeof(buffer), 0, (struct sockaddr *)client_addr, client_len);
}

/* Variável para mostrar qual alerta foi o último enviado (heurística simples) */
int last_sent_alert = -1;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s v4|v6\n", argv[0]);
        return 1;
    }

    FILE *f = fopen("grafo_amazonia_legal.txt", "r");
    int porta = 8080;
    if (!f) {
        perror("Erro abrindo arquivo");
        return 1;
    }

    Grafo *g = cria_grafo(f);
    fclose(f);

    printf("Servidor escutando na porta %d...\n\n", porta);

    char *protocolo = argv[1];
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    int sockfd;

    if (strcmp(protocolo, "v4") == 0) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        memset(&addr4, 0, sizeof(addr4));
        addr4.sin_family = AF_INET;
        addr4.sin_addr.s_addr = INADDR_ANY;
        addr4.sin_port = htons(porta);
        bind(sockfd, (struct sockaddr *)&addr4, sizeof(addr4));
    } else {
        sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr = in6addr_any;
        addr6.sin6_port = htons(porta);
        bind(sockfd, (struct sockaddr *)&addr6, sizeof(addr6));
    }

    while (1) {
        uint8_t buf[4096];
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&client_addr, &client_len);
        if (n < (ssize_t)sizeof(header_t)) continue;

        header_t h;
        memcpy(&h, buf, sizeof(h));
        uint16_t tipo = ntohs(h.tipo);
        uint16_t tamanho = ntohs(h.tamanho);
        uint8_t *payload = buf + sizeof(header_t);

        if (tipo == MSG_TELEMETRIA) {
            payload_telemetria_t tele;
            memcpy(&tele, payload, sizeof(payload_telemetria_t));
            // conversão de endianness
            tele.total = ntohl(tele.total);
            for (int i = 0; i < tele.total && i < 50; i++) {
                tele.dados[i].id_cidade = ntohl(tele.dados[i].id_cidade);
                tele.dados[i].status = ntohl(tele.dados[i].status);
            }

            printf("[TELEMETRIA RECEBIDA]\n");
            printf("Total de cidades monitoradas: %d\n", tele.total);

            // imprime alertas
            int any_alert = 0;
            for (int i = 0; i < tele.total && i < 50; i++) {
                if (tele.dados[i].status == 1) {
                    any_alert = 1;
                    int id = tele.dados[i].id_cidade;
                    printf("ALERTA: %s (ID=%d)\n", g->nodes[id]._nome, id);
                }
            }
            if (!any_alert) {
                printf("Nenhum alerta na telemetria.\n");
            }

            // envia ACK telemetria (status 0)
            send_ack(sockfd, &client_addr, client_len, 0);
            printf("-> ACK enviado (tipo=0)\n\n");

            // Para cada nova cidade que passou de 0->1, registrar e despachar
            for (int i = 0; i < tele.total && i < 50; i++) {
                int id = tele.dados[i].id_cidade;
                int st = tele.dados[i].status;
                if (g->nodes[id]._status == 0 && st == 1) {
                    registrar_alerta(id);
                    int distancia = -1;
                    int id_equipe = dijkstra_escolhe_equipe(g, id, &distancia);
                    printf("[DESPACHANDO DRONES]\n");
                    printf("Cidade em alerta: %s (ID=%d)\n", g->nodes[id]._nome, id);

                    if (id_equipe == -1) {
                        printf("-> Nenhuma equipe disponível alcançável para cidade %s (ID=%d)\n\n", g->nodes[id]._nome, id);
                    } else {
                        // log dijkstra
                        printf("-> Dijkstra: capital %s (ID=%d) selecionada, distância=%d km\n",
                               g->nodes[id_equipe]._nome, id_equipe, distancia >= 0 ? distancia : 0);

                        // envia ordem ao cliente (usa client_addr do recv)
                        ssize_t sent = enviar_msg_equipe(sockfd, &client_addr, client_len, id, id_equipe);
                        if (sent < 0) {
                            perror("sendto MSG_EQUIPE_DRONE failed");
                        } else {
                            // registra qual equipe está atuando nesse alerta (heurística: último alerta)
                            int idx_alert = total_alertas - 1;
                            if (idx_alert >= 0) alertas[idx_alert].equipe_atuando = id_equipe;
                            last_sent_alert = idx_alert;

                            printf("-> Ordem enviada : Equipe %s (ID=%d) -> Cidade %s (ID=%d)\n\n",
                                   g->nodes[id_equipe]._nome, id_equipe, g->nodes[id]._nome, id);
                        }
                    }
                    // marca status interno
                    g->nodes[id]._status = st;
                } else {
                    // só atualiza status
                    g->nodes[id]._status = st;
                }
            }

        } else if (tipo == MSG_ACK) {
            if (tamanho >= sizeof(payload_ack_t)) {
                payload_ack_t ap;
                memcpy(&ap, payload, sizeof(ap));
                int status = ntohl(ap.status);
                if (status == 1) {
                    // ACK de ordem de drone
                    printf("[ACK RECEBIDO]\n");
                    // heurística: assume ACK corresponde ao último enviado
                    if (last_sent_alert >= 0 && last_sent_alert < total_alertas) {
                        int id_c = alertas[last_sent_alert].id_cidade;
                        printf("Cliente confirmou recebimento de ordem de drone para %s (ID=%d)\n\n",
                               g->nodes[id_c]._nome, id_c);
                    } else {
                        printf("Cliente confirmou recebimento de ordem de drone (sem mapeamento)\n\n");
                    }
                } else if (status == 0) {
                    // ACK telemetria (geralmente já tratado no cliente)
                    // podemos logar se quiser
                } else if (status == 2) {
                    // ACK de conclusao (servidor normalmente envia ACK, mas cliente pode enviar)
                    printf("[ACK RECEBIDO] status=2 (conclusão)\n\n");
                }
            }
        } else if (tipo == MSG_CONCLUSAO) {
            if (tamanho >= sizeof(payload_equipe_drone_t)) {
                payload_equipe_drone_t p;
                memcpy(&p, payload, sizeof(p));
                int id_cidade = ntohl(p.id_cidade);
                int id_equipe = ntohl(p.id_equipe);

                // localizar alerta correspondente e liberar equipe
                int found = -1;
                for (int i = 0; i < total_alertas; i++) {
                    if (alertas[i].id_cidade == id_cidade) {
                        found = i;
                        break;
                    }
                }

                printf("[MISSAO CONCLUÍDA]\n");
                printf("Cidade atendida: %s (ID=%d)\n", g->nodes[id_cidade]._nome, id_cidade);
                printf("Equipe : %s (ID=%d)\n", g->nodes[id_equipe]._nome, id_equipe);

                // libera equipe no grafo (marcar capital livre)
                g->nodes[id_equipe]._ocupada = 0;
                if (found != -1) {
                    alertas[found].equipe_atuando = -1;
                }

                printf("-> Equipe %s liberada para novas missões\n", g->nodes[id_equipe]._nome);
                // envia ACK tipo=2
                send_ack(sockfd, &client_addr, client_len, 2);
                printf("-> ACK enviado (tipo=2)\n\n");
            }
        } else {
            // outros tipos
        }
    }

    close(sockfd);
    return 0;
}