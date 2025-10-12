#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>

#define PORT 8080
#define MAX_CLIENTS 10

// --- Formatting Attributes ---
#define ATTR_NONE       0b00000000
#define ATTR_BOLD       0b00000001
#define ATTR_UNDERLINE  0b00000010

// --- Data Structures ---

typedef struct {
    char ch;
    uint8_t format;
} StyledChar;

typedef struct {
    int x;
    int y;
    int user_id;
    bool active;
} Cursor;

// --- Operation Payloads ---
// These structs define the data for specific actions.

typedef struct {
    int y;
    int x;
    StyledChar s_char;
} InsertOp;

typedef struct {
    int y;
    int x; // Position of character *before* which deletion happens
} DeleteOp;

typedef struct {
    int y;
    int x; // Position of character to format
    uint8_t format;
} FormatOp;

typedef struct {
    int y;
    int x; // Position where the line break occurs
} NewlineOp;

// --- Communication Protocol ---

// Client-to-Server (C2S) Operation Types
typedef enum {
    C2S_INSERT,
    C2S_DELETE,         // Backspace
    C2S_DELETE_FORWARD, // Delete key
    C2S_MERGE_LINE,     // Backspace at start of a line
    C2S_CURSOR_MOVE,
    C2S_FORMAT_CHANGE,
    C2S_NEWLINE,
    C2S_RUN_CODE,
} C2S_OpType;

// Server-to-Client (S2C) Message Types
typedef enum {
    S2C_INITIAL_STATE, // Provides full document state to a new client
    S2C_INSERT,
    S2C_DELETE,
    S2C_DELETE_FORWARD,
    S2C_MERGE_LINE,
    S2C_CURSOR_UPDATE, // Provides cursor positions of other users
    S2C_FORMAT_UPDATE,
    S2C_NEWLINE,
    S2C_CODE_OUTPUT,
    S2C_USER_ID_ASSIGN, // Tells a new client their unique ID
} S2C_MsgType;

// The main message structure sent over the network.
// A union is used so the message size is consistent.
typedef struct {
    int user_id;
    int type; // Will be cast to C2S_OpType on server, S2C_MsgType on client
    union {
        // C2S Payloads
        InsertOp insert_op;
        DeleteOp delete_op;
        Cursor cursor_op;
        FormatOp format_op;
        NewlineOp newline_op;

        // S2C Payloads
        char code_output[1024];
    } payload;
} NetMessage;


#endif // COMMON_H

