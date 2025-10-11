CC = gcc
# Add -I./libs to tell the compiler to look for header files in the libs directory
# Add -pthread for Mongoose's threading capabilities
CFLAGS = -Wall -g -I./libs -pthread

# --- Source File Locations ---
LIBS_DIR = ./libs
MONGOOSE_SRC = $(LIBS_DIR)/mongoose.c
COMMON_SRC = $(LIBS_DIR)/common.c
COMMON_HEADER = $(LIBS_DIR)/common.h

# --- Server ---
SERVER_TARGET = server
SERVER_SRC = server.c
# Link against crypto for SHA256
SERVER_LIBS = -ljansson -ldl -lcrypto

# --- Client ---
CLIENT_TARGET = client
CLIENT_SRC = client.c
CLIENT_LIBS = -ljansson -lncurses -ldl

# Build both by default
all: $(SERVER_TARGET) $(CLIENT_TARGET)

# Rule to build the server
$(SERVER_TARGET): $(SERVER_SRC) $(MONGOOSE_SRC) $(COMMON_SRC) $(COMMON_HEADER)
	$(CC) $(CFLAGS) -o $(SERVER_TARGET) $(SERVER_SRC) $(MONGOOSE_SRC) $(COMMON_SRC) $(SERVER_LIBS)

# Rule to build the client
$(CLIENT_TARGET): $(CLIENT_SRC) $(MONGOOSE_SRC) $(COMMON_SRC) $(COMMON_HEADER)
	$(CC) $(CFLAGS) -o $(CLIENT_TARGET) $(CLIENT_SRC) $(MONGOOSE_SRC) $(COMMON_SRC) $(CLIENT_LIBS)

clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET)

