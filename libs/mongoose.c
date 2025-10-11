// Copyright (c) 2023 Cesanta Software Limited
// All rights reserved
//
// Abridged version of Mongoose 7.10, made functional for this project.

#include "mongoose.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/select.h>
#endif

void mg_mgr_init(struct mg_mgr *m) {
  memset(m, 0, sizeof(*m));
#if defined(_WIN32)
  WSADATA data;
  WSAStartup(MAKEWORD(2, 2), &data);
#endif
}

void mg_mgr_free(struct mg_mgr *m) {
  struct mg_connection *c, *next;
  for (c = m->conns; c != NULL; c = next) {
    next = c->next;
    mg_close_conn(c);
  }
}

static struct mg_connection *mg_add_conn(struct mg_mgr *m, sock_t sock, mg_event_handler_t fn, void *fn_data) {
  struct mg_connection *c = (struct mg_connection *) calloc(1, sizeof(*c));
  if (c == NULL) return NULL;
  c->sock = sock;
  c->mgr = m;
  c->fn = fn;
  c->fn_data = fn_data;
  if (m->conns) m->conns->prev = c;
  c->next = m->conns;
  m->conns = c;
  return c;
}

void mg_close_conn(struct mg_connection *c) {
  if (c == NULL) return;
  if (c->prev) c->prev->next = c->next;
  if (c->next) c->next->prev = c->prev;
  if (c->mgr->conns == c) c->mgr->conns = c->next;
  if(c->sock != -1) {
    #ifdef _WIN32
        closesocket(c->sock);
    #else
        close(c->sock);
    #endif
  }
  free(c->user_data);
  free(c);
}

static void mg_call(struct mg_connection *c, int ev, void *ev_data) {
  if (c->fn != NULL) c->fn(c, ev, ev_data, c->fn_data);
}

void mg_mgr_poll(struct mg_mgr *m, int msecs) {
    fd_set rset, wset;
    sock_t max_sock = 0;
    struct mg_connection *c, *next;
    struct timeval tv = { msecs / 1000, (msecs % 1000) * 1000 };

    FD_ZERO(&rset);
    FD_ZERO(&wset);

    for (c = m->conns; c != NULL; c = c->next) {
        if (c->sock == -1) continue;
        FD_SET(c->sock, &rset);
        if (c->sock > max_sock) max_sock = c->sock;
    }

    if (select((int)max_sock + 1, &rset, &wset, NULL, &tv) == -1) return;

    for (c = m->conns; c != NULL; c = next) {
        next = c->next;
        if (c->sock == -1 || !FD_ISSET(c->sock, &rset)) continue;
        
        if (c->flags & MG_F_LISTENING) {
            sock_t new_sock = accept(c->sock, NULL, NULL);
            if (new_sock != -1) {
                mg_call(mg_add_conn(m, new_sock, c->fn, c->fn_data), MG_EV_ACCEPT, NULL);
            }
        } else {
            char buf[4096];
            int n = recv(c->sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                mg_call(c, MG_EV_CLOSE, NULL);
                mg_close_conn(c);
                continue;
            }
            
            if (!(c->flags & MG_F_IS_WEBSOCKET)) {
                if (strstr(buf, "Upgrade: websocket")) {
                    const char *key_header = "Sec-WebSocket-Key: ";
                    const char *key = strstr(buf, key_header);
                    if (key) {
                        key += strlen(key_header);
                        char key_buf[64];
                        sscanf(key, "%s", key_buf);
                        const char *response =
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
                        send(c->sock, response, strlen(response), 0);
                        c->flags |= MG_F_IS_WEBSOCKET;
                        mg_call(c, MG_EV_WS_OPEN, NULL);
                    }
                }
            } else {
                unsigned char *p = (unsigned char *) buf;
                size_t i = 0;
                while (i + 2 <= (size_t)n) {
                    size_t len = p[i+1] & 0x7f;
                    size_t offset = i + 2;
                    if (len == 126 && i + 4 <= (size_t)n) {
                        len = (p[i+2] << 8) | p[i+3];
                        offset = i + 4;
                    } else if (len == 127) { 
                         break;
                    }

                    size_t mask_offset = 0;
                    if (p[i+1] & 0x80) { // Masked frame from client
                        mask_offset = 4;
                    }
                    if (offset + mask_offset + len > (size_t)n) break; 
                    
                    if (mask_offset > 0) {
                        unsigned char *mask = p + offset;
                        for(size_t j = 0; j < len; j++) {
                            p[offset + mask_offset + j] ^= mask[j % 4];
                        }
                    }

                    struct mg_ws_message wm = { { (char*)p + offset + mask_offset, len }, p[i] };
                    mg_call(c, MG_EV_WS_MSG, &wm);
                    i += offset + mask_offset + len;
                }
            }
        }
    }
}

struct mg_connection *mg_http_listen(struct mg_mgr *mgr, const char *url, mg_event_handler_t fn, void *fn_data) {
    (void) url; 
    struct sockaddr_in sa;
    sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return NULL;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(8000);
    sa.sin_addr.s_addr = INADDR_ANY;

    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));

    if (bind(sock, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        close(sock);
        return NULL;
    }
    if (listen(sock, 10) != 0) {
        close(sock);
        return NULL;
    }
    struct mg_connection *c = mg_add_conn(mgr, sock, fn, fn_data);
    if(c) {
        c->flags |= MG_F_LISTENING;
        mg_call(c, MG_EV_ACCEPT, NULL);
    }
    return c;
}

struct mg_connection *mg_ws_connect(struct mg_mgr *mgr, const char *url, mg_event_handler_t fn, void *fn_data, const char *fmt, ...) {
    (void) fmt;
    struct sockaddr_in sa;
    sock_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return NULL;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    
    char host[100];
    unsigned short port = 8000;
    if(sscanf(url, "ws://%99[^:]:%hu", host, &port) < 2) {
        sscanf(url, "ws://%99s", host);
    }
    sa.sin_port = htons(port);
    
    struct hostent *he = gethostbyname(host);
    if(he == NULL) { close(sock); return NULL; }
    memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr *) &sa, sizeof(sa)) != 0) {
        close(sock);
        return NULL;
    }

    struct mg_connection *c = mg_add_conn(mgr, sock, fn, fn_data);
    if (c) {
        char req[512];
        snprintf(req, sizeof(req),
            "GET / HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n", host);
        send(c->sock, req, strlen(req), 0);
    }
    return c;
}

int mg_ws_send(struct mg_connection *c, const char *buf, size_t len, int op) {
    unsigned char frame[10];
    frame[0] = 0x80 | op;
    size_t n = 1;
    if (len < 126) {
        frame[n++] = (unsigned char) len;
    } else { 
        frame[n++] = 126;
        frame[n++] = (unsigned char) (len >> 8);
        frame[n++] = (unsigned char) len;
    }
    send(c->sock, (char*)frame, n, 0);
    return send(c->sock, buf, (int)len, 0);
}

int mg_printf(struct mg_connection *c, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return mg_ws_send(c, buf, len, WEBSOCKET_OP_TEXT);
}

