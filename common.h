// common.h
#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>

// --- Networking & capacity ---
#define PORT 8080
#define MAX_CLIENTS 10

// --- Document capacity ---
#define MAX_LINES     1024
#define MAX_LINE_LEN  1024

// --- Formatting Attributes ---
#define ATTR_NONE       0b00000000
#define ATTR_BOLD       0b00000001
#define ATTR_UNDERLINE  0b00000010

// --- Data Structures ---
typedef struct {
    char    ch;
    uint8_t format;   // bitmask of ATTR_BOLD | ATTR_UNDERLINE
} StyledChar;

typedef struct {
    int  x;
    int  y;
    int  user_id;
    bool active;
} Cursor;

// Range is [y1,x1] .. [y2,x2), i.e., exclusive end like typical editors.
typedef struct {
    int y1, x1;
    int y2, x2;
} Range;

typedef struct {
    Range range;
    bool  active;
} SelectionState;

// --- Operation Payloads ---
typedef struct {
    int y;
    int x;
    StyledChar s_char;
} InsertOp;

typedef struct {
    int y;
    int x;  // Position of character *before* which deletion happens
} DeleteOp;

typedef struct {
    int      y;
    int      x;      // Position of character to format
    uint8_t  format; // absolute value to set
} FormatOp;

typedef struct {
    int y;
    int x; // Position where the line break occurs
} NewlineOp;

typedef struct {
    Range    range;
    uint8_t  format_toggle_bits; // bits to XOR across range
} FormatRangeOp;

typedef struct {
    Range range;
} DeleteRangeOp;

// --- Protocol enums ---
// Client-to-Server (C2S) Operation Types
typedef enum {
    C2S_INSERT,
    C2S_DELETE,           // Backspace
    C2S_DELETE_FORWARD,   // Delete key
    C2S_MERGE_LINE,       // Backspace at column 0
    C2S_CURSOR_MOVE,
    C2S_FORMAT_CHANGE,    // single-char format
    C2S_NEWLINE,
    C2S_RUN_CODE,
    C2S_FORMAT_RANGE,     // selection format
    C2S_DELETE_RANGE,     // selection delete
    C2S_SELECTION_UPDATE, // selection broadcast
} C2S_OpType;

// Server-to-Client (S2C) Message Types
typedef enum {
    S2C_INITIAL_STATE,
    S2C_INSERT,
    S2C_DELETE,
    S2C_DELETE_FORWARD,
    S2C_MERGE_LINE,
    S2C_CURSOR_UPDATE,
    S2C_FORMAT_UPDATE,
    S2C_NEWLINE,
    S2C_CODE_OUTPUT,
    S2C_USER_ID_ASSIGN,
    S2C_FORMAT_RANGE,
    S2C_DELETE_RANGE,
    S2C_SELECTION_UPDATE, // selection broadcast
} S2C_MsgType;

// --- Network Message ---
// Fixed-size struct for simple send/recv (same architecture recommended).
typedef struct {
    int user_id;
    int type; // cast to C2S_OpType on server, S2C_MsgType on client
    union {
        // C2S payloads
        InsertOp       insert_op;
        DeleteOp       delete_op;
        Cursor         cursor_op;
        FormatOp       format_op;
        NewlineOp      newline_op;
        FormatRangeOp  format_range_op;
        DeleteRangeOp  delete_range_op;
        SelectionState selection_op;

        // S2C payloads
        char           code_output[1024];
    } payload;
} NetMessage;

#endif // COMMON_H