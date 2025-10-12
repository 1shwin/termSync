CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -g
LDFLAGS_SERVER=-pthread
LDFLAGS_CLIENT=-pthread -lncurses

all: server client

server: server.c common.h
	$(CC) $(CFLAGS) server.c -o server $(LDFLAGS_SERVER)

client: client.c common.h
	$(CC) $(CFLAGS) client.c -o client $(LDFLAGS_CLIENT)

clean:
	rm -f server client
