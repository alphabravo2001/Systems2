#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 3820
#define BUFFER_SIZE 1024

int sockfd;

// Function to connect to the server
int server_connect(const char *ip_address) {
    struct sockaddr_in server_addr;

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    // Convert IP address
    if (inet_pton(AF_INET, ip_address, &server_addr.sin_addr) <= 0) {
        perror("Invalid address or Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to server failed");
        exit(EXIT_FAILURE);
    }

    printf("Connected to server at %s:%d\n", ip_address, PORT);
    return sockfd;
}

// Function to handle quitting the client (Ctrl-D or "quit" command)
void handle_quit(int sig) {
    printf("Quitting the client...\n");
    close(sockfd);
    exit(0);
}

// Function to handle sending Ctrl-C (SIGINT) to the server
void handle_sigint(int sig) {
    char ctl_message[] = "CTL c\n";  // Send control message for Ctrl-C
    send(sockfd, ctl_message, strlen(ctl_message), 0);
}

// Function to handle sending Ctrl-Z (SIGTSTP) to the server
void handle_sigtstp(int sig) {
    char ctl_message[] = "CTL z\n";  // Send control message for Ctrl-Z
    send(sockfd, ctl_message, strlen(ctl_message), 0);
}

// Function to send command to server using the CMD protocol
void send_command(char *command) {
    char buffer[BUFFER_SIZE];

    // Format the command to match the CMD protocol
    snprintf(buffer, sizeof(buffer), "CMD %s\n", command);
    send(sockfd, buffer, strlen(buffer), 0);
}


// Function to handle plain text input for commands like 'cat > file.txt'
void send_plaintext_loop() {
    char input[BUFFER_SIZE];

    // Continue reading from stdin until EOF (Ctrl-D)
    while (fgets(input, sizeof(input), stdin)) {
        // Send the plain text to the server
        send(sockfd, input, strlen(input), 0);
    }

    // When the user sends EOF (Ctrl-D), send an EOF message to the server
    send(sockfd, "EOF\n", 4, 0);  // Notify the server that the input is finished
}

// Main client loop for reading commands and receiving responses
void client_loop() {
    char buffer[BUFFER_SIZE];  // Buffer to store server responses
    char command[BUFFER_SIZE]; // Buffer to store client commands
    int bytes_read;

    while (1) {
        // Read server output (including prompt)
        bytes_read = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            printf("Server disconnected or error occurred.\n");
            break;
        }

        // Display the server's output, including the prompt
        buffer[bytes_read] = '\0';  // Null-terminate the string
        printf("%s", buffer);  // Print the server's response

        // Read user input (command or text) and send it to the server
        if (fgets(command, sizeof(command), stdin) == NULL) {
            // End of input (Ctrl-D)
            handle_quit(0);  // Call a function to handle quitting
        }

        // Remove the newline at the end of the input
        command[strcspn(command, "\n")] = '\0';

        // Special case for quitting
        if (strcmp(command, "quit") == 0) {
            handle_quit(0);  // Call the quit handler
        }

        // If the user enters a shell command (CMD), send it to the server
        if (strncmp(command, "CMD ", 4) == 0) {
            send_command(command + 4);  // Skip "CMD " and send only the command
        } else if (strncmp(command, "CTL ", 4) == 0) {
            // Handle control commands (CTL), for example, "CTL c", "CTL z"
            send(sockfd, command, strlen(command), 0);  // Send control message to the server
        }
    }
}


int main(int argc, char *argv[]) {
    // Check if the IP address is provided
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <IP_Address_of_Server>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Set up signal handling for Ctrl-C (SIGINT) and Ctrl-Z (SIGTSTP)
    signal(SIGINT, handle_sigint);    // Handle Ctrl-C (SIGINT)
    signal(SIGTSTP, handle_sigtstp);  // Handle Ctrl-Z (SIGTSTP)

    // Connect to the server
    server_connect(argv[1]);

    // Start the client loop
    client_loop();

    // Close the socket when done
    close(sockfd);
    return 0;
}
