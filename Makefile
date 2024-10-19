# Define the compiler
CC = gcc

# Define the compiler flags
CFLAGS = -Wall -g

# Define the libraries to link
LIBS = -lreadline

# Define the source files
SERVER_SRC = server.c ysh.c
CLIENT_SRC = client.c

# Define the target executables
SERVER_TARGET = yashd
CLIENT_TARGET = yash

# Rules to build the server executable
$(SERVER_TARGET): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_TARGET) $(SERVER_SRC) $(LIBS)

# Rules to build the client executable
$(CLIENT_TARGET): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_TARGET) $(CLIENT_SRC)

# Clean up the build files
clean:
	rm -f $(SERVER_TARGET) $(CLIENT_TARGET)
