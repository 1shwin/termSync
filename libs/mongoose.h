// Abridged, functional Mongoose header for the project.
#ifndef MONGOOSE_H
#define MONGOOSE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;
#else
typedef int sock_t;
#endif

// Main Mongoose manager struct. Holds all connections.
struct mg_mgr;

// Connection struct. One per client.
struct mg_connection;

// Event handler function signature.
typedef void (*mg_event_handler_t)(struct mg_connection *, int ev, void *ev_data, void *fn_data);

// Events that can be passed to the event handler.
enum {
  MG_EV_ERROR,
  MG_EV_ACCEPT,
  MG_EV_CONNECT,
  MG_EV_CLOSE,
  MG_EV_POLL,
  MG_EV_WS_OPEN,
  MG_EV_WS_MSG,
};

#define MG_F_LISTENING (1 << 0)
#define MG_F_IS_WEBSOCKET (1 << 7)
#define WEBSOCKET_OP_TEXT 1

// A string representation used by Mongoose.
struct mg_str {
  const char *p;
  size_t len;
};

// WebSocket message struct, passed to the event handler on MG_EV_WS_MSG
struct mg_ws_message {
  struct mg_str data;
  unsigned char flags;
};

// The connection struct with all necessary members.
struct mg_connection {
  struct mg_connection *next, *prev;
  struct mg_mgr *mgr;
  sock_t sock;
  int flags;
  void *user_data; // <--- ADDED: Critical for storing user state
  mg_event_handler_t fn;
  void *fn_data;
};

// The manager struct with all necessary members.
struct mg_mgr {
  struct mg_connection *conns;
};

// --- Mongoose API Function Prototypes ---

// Initialize a Mongoose manager.
void mg_mgr_init(struct mg_mgr *mgr);

// Free a Mongoose manager and all its connections.
void mg_mgr_free(struct mg_mgr *mgr);

// Poll for events. This is the main event loop function.
void mg_mgr_poll(struct mg_mgr *mgr, int msecs);

// Create a listening connection (server).
struct mg_connection *mg_http_listen(struct mg_mgr *mgr, const char *url, mg_event_handler_t fn, void *fn_data);

// Create an outbound WebSocket connection (client).
struct mg_connection *mg_ws_connect(struct mg_mgr *mgr, const char *url, mg_event_handler_t fn, void *fn_data, const char *fmt, ...);

// Send data over a WebSocket connection.
int mg_ws_send(struct mg_connection *c, const char *buf, size_t len, int op);

// Send formatted data over a connection (like printf).
int mg_printf(struct mg_connection *c, const char *fmt, ...);

// Close a connection.
void mg_close_conn(struct mg_connection *c);

#endif // MONGOOSE_H

