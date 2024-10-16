#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <util.h>
#include <utmp.h>


#define PORT 3820
#define MAX_CONNECTIONS 10
#define BUFFER_SIZE 1024
#define MAX_ARGS 10

pthread_t client_threads[MAX_CONNECTIONS];  // Array to hold thread IDs
int thread_count = 0;  // To track the current number of active threads

pthread_mutex_t thread_count_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int client_socket;
    char client_ip[INET_ADDRSTRLEN];
    int client_port;
} client_t;

// Daemonize the process
void create_daemon() {
    pid_t pid;
    static FILE *log; /* for the log */

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    //child code

    /* Close all file descriptors that are open */
    int k;
    int fd;
    for (k = getdtablesize()-1; k>0; k--)
        close(k);

    /* Redirecting stdin and stdout to /dev/null */
    if ( (fd = open("/dev/null", O_RDWR)) < 0) {
        perror("Open");
        exit(0);
    }
    dup2(fd, STDIN_FILENO);      /* detach stdin */
    dup2(fd, STDOUT_FILENO);     /* detach stdout */
    close (fd);

    // Set the file permissions mask to 0
    umask(0);

    /* Detach controlling terminal by becoming sesion leader */
    setsid();

    /* Put self in a new process group */
    pid = getpid();
    setpgrp(); /* GPI: modified for linux */

    // Change the working directory to root
    if (chdir("/") < 0) {
        exit(EXIT_FAILURE);
    }

    // Open a log file for output
    open("/tmp/yashd.log", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    syslog(LOG_NOTICE, "Daemon started successfully");
}


void *handle_client(void *arg) {
    client_t *client_info = (client_t *)arg;  // Cast the argument to client_t struct
    int client_socket = client_info->client_socket;
    char client_ip[INET_ADDRSTRLEN];
    strcpy(client_ip, client_info->client_ip);
    int client_port = client_info->client_port;

    syslog(LOG_INFO, "Handling client: %s:%d", client_ip, client_port);
    free(client_info);  // Free the dynamically allocated client struct

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    int master_fd, slave_fd;
    pid_t pid;

    // Time buffer for logging
    char time_str[64];
    time_t now;
    struct tm *timeinfo;

    // Open log file in append mode
    FILE *log_file = fopen("/tmp/yashd.log", "a");
    if (!log_file) {
        syslog(LOG_ERR, "Failed to open log file");
        close(client_socket);
        pthread_exit(NULL);
    }

    // Create a pseudo-terminal (pty)
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == -1) {
        syslog(LOG_ERR, "Failed to open pty");
        close(client_socket);
        pthread_exit(NULL);
    }

    // Fork the process to create a child
    pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Fork failed");
        close(client_socket);
        close(master_fd);
        close(slave_fd);
        pthread_exit(NULL);
    }

    if (pid == 0) {  // Child process: run the command or shell
        close(master_fd);  // Close master FD in the child
        dup2(slave_fd, STDIN_FILENO);  // Redirect stdin to slave FD
        dup2(slave_fd, STDOUT_FILENO); // Redirect stdout to slave FD
        dup2(slave_fd, STDERR_FILENO); // Redirect stderr to slave FD
        close(slave_fd);  // No longer needed after dup2()
        execl("/ysh", "ysh",  (char *)NULL);  // Run a shell
        syslog(LOG_ERR, "Exec failed");
        exit(EXIT_FAILURE);
    }

    close(slave_fd);  // Close slave FD in the parent

    // Use select() to monitor both client_socket and master_fd
    fd_set read_fds;
    int max_fd = (client_socket > master_fd) ? client_socket : master_fd;  // Max of both FDs

    while (1) {
        FD_ZERO(&read_fds);  // Clear the set of file descriptors
        FD_SET(client_socket, &read_fds);  // Add client socket to the set
        FD_SET(master_fd, &read_fds);  // Add master FD to the set

        // Wait for input on either the client socket or the master FD
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity < 0 && errno != EINTR) {
            syslog(LOG_ERR, "Select error");
            break;
        }

        // Check if there's data to read from the client socket
        if (FD_ISSET(client_socket, &read_fds)) {
            bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) {
                // Client disconnected
                syslog(LOG_INFO, "Client disconnected: %s:%d", client_ip, client_port);
                break;
            }
            buffer[bytes_read] = '\0';

            // Log the command to /tmp/yashd.log
            time(&now);
            timeinfo = localtime(&now);
            strftime(time_str, sizeof(time_str), "%b %d %H:%M:%S", timeinfo);  // Format time like syslog

            // Write log entry
            fprintf(log_file, "%s yashd[%s:%d]: %s\n", time_str, client_ip, client_port, buffer);
            fflush(log_file);  // Ensure the log is written immediately

            if (strcmp(buffer, "EOF\n") == 0) {
                // If the client sends EOF, stop reading input
                syslog(LOG_INFO, "EOF received from client: %s:%d", client_ip, client_port);
                break;
            }

            // Send the command to the child process (running the shell)
            write(master_fd, buffer, bytes_read);
        }

        // Check if there's data to read from the master FD (child process output)
        if (FD_ISSET(master_fd, &read_fds)) {
            bytes_read = read(master_fd, buffer, sizeof(buffer) - 1);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                send(client_socket, buffer, bytes_read, 0);
            }
        }
    }

    // Clean up after communication ends
    close(client_socket);
    close(master_fd);
    pthread_mutex_lock(&thread_count_lock);
    thread_count--;
    pthread_mutex_unlock(&thread_count_lock);
    pthread_exit(NULL);
}



void run_server() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Create the server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        syslog(LOG_ERR, "Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Enable port reuse
    int reuse = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Bind the socket to the specified port
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(3820);
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to any address

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_socket, MAX_CONNECTIONS) < 0) {
        syslog(LOG_ERR, "Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Server listening on port %d", PORT);

    // Accept and handle incoming connections
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            syslog(LOG_ERR, "Accept failed");
            continue;
        }

        syslog(LOG_INFO, "Client connected");

        // Allocate memory for the client info
        client_t *client_info = malloc(sizeof(client_t));
        if (client_info == NULL) {
            syslog(LOG_ERR, "Memory allocation failed");
            close(client_socket);
            continue;
        }

        client_info->client_socket = client_socket;
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_info->client_ip, INET_ADDRSTRLEN);
        client_info->client_port = ntohs(client_addr.sin_port);

        // Limit the number of threads (clients)
        pthread_mutex_lock(&thread_count_lock);
        if (thread_count >= MAX_CONNECTIONS) {
            syslog(LOG_WARNING, "Maximum client connections reached, rejecting client.");
            close(client_socket);
            free(client_info);
            pthread_mutex_unlock(&thread_count_lock);
            continue;
        }
        int thread_index = thread_count++;
        pthread_mutex_unlock(&thread_count_lock);

        // Create a thread to handle the client
        int ret = pthread_create(&client_threads[thread_index], NULL, handle_client, (void *)client_info);
        if (ret != 0) {
            syslog(LOG_ERR, "Failed to create thread: %s", strerror(ret));
            close(client_socket);
            free(client_info);
            pthread_mutex_lock(&thread_count_lock);
            thread_count--;  // Decrement count in case of failure
            pthread_mutex_unlock(&thread_count_lock);
            continue;
        }

        // Optionally detach the thread to clean up resources after it exits
        pthread_detach(client_threads[thread_index]);
    }

    // Close the server socket when shutting down
    close(server_socket);
}


int main(){
    create_daemon();
    run_server();  // Start the server
    return 0;
}