#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>
#include <openssl/sha.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "mongoose.h"
#include "jansson.h"
#include "common.h"

#define SERVER_DATA_DIR "server-data"
#define COOP_FILE SERVER_DATA_DIR "/coop.json"
#define PUB_FILE SERVER_DATA_DIR "/pub.json"
#define USERS_FILE SERVER_DATA_DIR "/users.txt"
#define VERSIONS_DIR SERVER_DATA_DIR "/versions"
#define TEMP_DIR SERVER_DATA_DIR "/temp"

// Hardcoded server and admin credentials
const char SERVER_PASS[] = "vodka";
const char ADMIN_USER[]  = "admin";
const char ADMIN_PASS[]  = "admin123";

// Connection state per client
typedef struct {
    bool is_authed;
    char username[256];
    char personal_pass[256]; // Store temporarily for new user registration
} ConnState;

static Document g_doc;
static int g_done = 0;
static struct mg_mgr g_mgr;

// --- Function Prototypes ---
void sha256_hex(const char *input, char out_hex[65]);
int lookup_user(const char *username_plain, char stored_pass_hex[65], char role_out[16]);
int append_user(const char *username_plain, const char *password_plain, const char *role);
static void broadcast(struct mg_connection *sender, const char *msg, size_t len);
static int load_document_from_file(const char *filename, Document *doc);
static void save_document_to_file(const char *filename, const Document *doc);
static void ensure_directory_exists(const char *path);
static void event_handler(struct mg_connection *c, int ev, void *ev_data, void *fn_data);
void print_local_ip(void);


void sha256_hex(const char *input, char out_hex[65]) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)input, strlen(input), hash);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        sprintf(out_hex + (i * 2), "%02x", hash[i]);
    }
    out_hex[64] = '\0';
}

int lookup_user(const char *username_plain, char stored_pass_hex[65], char role_out[16]) {
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) return 0;
    char want_hash[65];
    sha256_hex(username_plain, want_hash);

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char file_user[65], file_pass[65], file_role[32];
        if (sscanf(line, "%64s %64s %31s", file_user, file_pass, file_role) == 3) {
            if (strcmp(file_user, want_hash) == 0) {
                if (stored_pass_hex) strcpy(stored_pass_hex, file_pass);
                if (role_out) { strncpy(role_out, file_role, 15); role_out[15] = '\0'; }
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

int append_user(const char *username_plain, const char *password_plain, const char *role) {
    if (lookup_user(username_plain, NULL, NULL)) {
        return 1; 
    }
    FILE *f = fopen(USERS_FILE, "a");
    if (!f) {
        perror("open users file for append");
        return 0;
    }
    char user_hex[65], pass_hex[65];
    sha256_hex(username_plain, user_hex);
    sha256_hex(password_plain, pass_hex);
    fprintf(f, "%s %s %s\n", user_hex, pass_hex, role);
    fclose(f);
    return 1;
}

static void broadcast(struct mg_connection *sender, const char *msg, size_t len) {
    struct mg_connection *c;
    for (c = g_mgr.conns; c != NULL; c = c->next) {
        if (c != sender && c->user_data != NULL) {
            ConnState *state = (ConnState *) c->user_data;
            if (state->is_authed) {
                mg_ws_send(c, msg, len, WEBSOCKET_OP_TEXT);
            }
        }
    }
}

static int load_document_from_file(const char *filename, Document *doc) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        doc->num_paragraphs = 1;
        Paragraph *p = &doc->paragraphs[0];
        p->num_segments = 1;
        strcpy(p->segments[0].text, "Welcome to the Collaborative Editor!");
        p->segments[0].num_styles = 0;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *json_str = malloc(fsize + 1);
    fread(json_str, 1, fsize, f);
    fclose(f);
    json_str[fsize] = 0;
    int result = json_to_document(json_str, doc);
    free(json_str);
    return result;
}

static void save_document_to_file(const char *filename, const Document *doc) {
    char *json_str = document_to_json(doc);
    if (json_str) {
        FILE *f = fopen(filename, "w");
        if (f) {
            fprintf(f, "%s", json_str);
            fclose(f);
        }
        free(json_str);
    }
}

static void ensure_directory_exists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0700);
    }
}

static void event_handler(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    if (ev == MG_EV_ACCEPT) {
        c->user_data = calloc(1, sizeof(ConnState));
    } else if (ev == MG_EV_CLOSE) {
        if (c->user_data) free(c->user_data);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        ConnState *state = (ConnState *) c->user_data;
        if (!state) return;

        json_error_t error;
        json_t *root = json_loads(wm->data.p, 0, &error);
        if (!root) return;

        const char *type = json_string_value(json_object_get(root, "type"));

        if (type && strcmp(type, "LOGIN") == 0) {
            const char *user = json_string_value(json_object_get(root, "user"));
            const char *pass = json_string_value(json_object_get(root, "pass"));
            
            if (!user || !pass) { json_decref(root); return; }

            char stored_pass_hex[65], role[16];
            if (lookup_user(user, stored_pass_hex, role)) {
                char personal_hex[65];
                sha256_hex(pass, personal_hex);
                if (strcmp(personal_hex, stored_pass_hex) == 0) {
                    state->is_authed = true;
                    strncpy(state->username, user, sizeof(state->username)-1);
                    
                    char *doc_json = document_to_json(&g_doc);
                    mg_printf(c, "{\"type\":\"AUTH_SUCCESS\",\"payload\":%s}", doc_json);
                    free(doc_json);
                } else {
                    mg_printf(c, "{\"type\":\"AUTH_FAIL\",\"reason\":\"wrong_password\"}");
                }
            } else { // New user
                strncpy(state->username, user, sizeof(state->username)-1);
                strncpy(state->personal_pass, pass, sizeof(state->personal_pass)-1);
                mg_printf(c, "{\"type\":\"NEED_SERVER_PASS\"}");
            }
        } else if (type && strcmp(type, "SERVERPASS") == 0) {
            const char *server_pass = json_string_value(json_object_get(root, "pass"));
            if (server_pass && strcmp(server_pass, SERVER_PASS) == 0) {
                append_user(state->username, state->personal_pass, "user");
                state->is_authed = true;
                char *doc_json = document_to_json(&g_doc);
                mg_printf(c, "{\"type\":\"AUTH_SUCCESS\",\"payload\":%s}", doc_json);
                free(doc_json);
            } else {
                mg_printf(c, "{\"type\":\"AUTH_FAIL\",\"reason\":\"wrong_server_password\"}");
            }
        } else if (type && strcmp(type, "DOC_UPDATE") == 0 && state->is_authed) {
            json_t *payload = json_object_get(root, "payload");
            char *payload_str = json_dumps(payload, 0);
            if (json_to_document(payload_str, &g_doc) == 0) {
                save_document_to_file(COOP_FILE, &g_doc);
                broadcast(c, wm->data.p, wm->data.len);
            }
            free(payload_str);
        }
        json_decref(root);
    }
    (void) fn_data;
}

void print_local_ip() {
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return;
    }

    printf("Server IP addresses:\n");
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET) { // Only care about IPv4 for this simple case
            s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                            host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                continue;
            }
            // Ignore loopback address
            if (strcmp(host, "127.0.0.1") != 0) {
                 printf("  -> %s\n", host);
            }
        }
    }
    freeifaddrs(ifaddr);
}


int main(void) {
    printf("Starting server...\n");

    ensure_directory_exists(SERVER_DATA_DIR);
    ensure_directory_exists(VERSIONS_DIR);
    ensure_directory_exists(TEMP_DIR);
    
    FILE *f = fopen(USERS_FILE, "r");
    if (!f) {
        append_user(ADMIN_USER, ADMIN_PASS, "admin");
    } else {
        fclose(f);
    }
    
    load_document_from_file(COOP_FILE, &g_doc);

    mg_mgr_init(&g_mgr);
    printf("Server listening on port 8000. Clients should connect to one of the IPs below:\n");
    print_local_ip();

    mg_http_listen(&g_mgr, "ws://0.0.0.0:8000", event_handler, NULL);

    while (!g_done) {
        mg_mgr_poll(&g_mgr, 100);
    }

    mg_mgr_free(&g_mgr);
    printf("Server stopped.\n");
    return 0;
}

