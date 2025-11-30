CC = gcc
CFLAGS = -Wall -Wextra

all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o server -lpthread
	./server v6

client: client.c
	$(CC) $(CFLAGS) client.c -o client -lpthread
	./client v6

clean:
	rm -f server client
