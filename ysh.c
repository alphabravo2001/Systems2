#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

// Global variables
pid_t stopped_stack[STACK_SIZE];
int stack_top = -1;
pid_t foreground_pid = -1;
Job *head = NULL;
Job *tail = NULL;
int job_count = 0;
char *current_command_line = NULL;

// Stack functions for managing stopped processes
void push(pid_t pid) {
    if (stack_top < STACK_SIZE - 1) {
        stopped_stack[++stack_top] = pid;
    }
}

pid_t pop() {
    if (stack_top >= 0) {
        return stopped_stack[stack_top--];
    }
    return -1;
}

pid_t peek() {
    if (stack_top >= 0) {
        return stopped_stack[stack_top];
    }
    return -1;
}

// Job control functions
void add_job(pid_t pgid, char *command, JobStatus status, int print) {
    Job *new_job = (Job *)malloc(sizeof(Job));
    if (!new_job) {
        perror("malloc failed");
        return;
    }

    new_job->job_id = ++job_count;
    new_job->pgid = pgid;
    new_job->status = status;
    strncpy(new_job->command, command, 255);
    new_job->command[255] = '\0';
    new_job->next = NULL;

    // Add the job to the linked list
    if (head == NULL) {
        head = new_job;
        tail = new_job;
    } else {
        tail->next = new_job;
        tail = new_job;
    }

    if (print != 0) {
        printf("[%d] %d %s &\n", new_job->job_id, new_job->pgid, new_job->command);
    }
}

void remove_job(pid_t pid) {
    Job *current = head;
    Job *previous = NULL;

    while (current != NULL && current->pgid != pid) {
        previous = current;
        current = current->next;
    }

    if (current != NULL) {
        if (previous == NULL) {
            head = current->next;
        } else {
            previous->next = current->next;
        }

        if (current == tail) {
            tail = previous;
        }

        free(current);
        job_count--;
    }
}

Job* find_job(pid_t pgid) {
    Job *current = head;

    while (current != NULL) {
        if (current->pgid == pgid) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// List jobs
void list_jobs() {
    Job *current = head;

    if (current == NULL) {
        return;
    }

    pid_t most_recent_stopped_pid = peek();

    while (current != NULL) {
        printf("[%d] ", current->job_id);
        if (current->status == RUNNING) {
            printf("Running   ");
        } else if (current->status == SUSPENDED && current->pgid == most_recent_stopped_pid) {
            printf("+ Suspended   ");
        } else if (current->status == SUSPENDED && current->pgid != most_recent_stopped_pid) {
            printf("- Suspended   ");
        } else if (current->status == DONE) {
            printf("Done      ");
        }
        printf("PGID: %d   %s\n", current->pgid, current->command);
        current = current->next;
    }
}

// Foreground and background commands
void fg_command() {
    pid_t most_recent_stopped_pid = peek();

    if (most_recent_stopped_pid == -1) {
        printf("fg: no current job\n");
        return;
    }

    Job *current = find_job(most_recent_stopped_pid);

    if (current != NULL) {
        if (current->status == SUSPENDED) {
            kill(-current->pgid, SIGCONT);
        }
        pop();
        printf("[%d] continued %s\n", current->job_id, current->command);
        current->status = RUNNING;
        foreground_pid = current->pgid;
        int status;
        waitpid(-current->pgid, &status, WUNTRACED);
        if (WIFSTOPPED(status)) {
            current->status = SUSPENDED;
            foreground_pid = -1;
        } else {
            current->status = DONE;
            remove_job(current->pgid);
            foreground_pid = -1;
        }
    }
}

void bg_command() {
    pid_t most_recent_stopped_pid = peek();

    if (most_recent_stopped_pid == -1) {
        printf("bg: no current job\n");
        return;
    }

    Job *current = find_job(most_recent_stopped_pid);

    if (current != NULL) {
        if (current->status == SUSPENDED) {
            kill(-current->pgid, SIGCONT);
        }
        pop();
        current->status = RUNNING;
        printf("[%d] %s &\n", current->job_id, current->command);
    } else {
        printf("bg: no current job\n");
    }
}

// Redirection
int redirection(char **args) {
    int in_fd = -1, out_fd = -1;

    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            in_fd = open(args[i + 1], O_RDONLY);
            if (in_fd == -1) {
                perror("Failed to open input file");
                return -1;
            }
            dup2(in_fd, STDIN_FILENO);
            close(in_fd);
            args[i] = NULL;
            i++;
        } else if (strcmp(args[i], ">") == 0) {
            out_fd = open(args[i + 1], O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
            if (out_fd == -1) {
                perror("Failed to open output file");
                return -1;
            }
            dup2(out_fd, STDOUT_FILENO);
            close(out_fd);
            args[i] = NULL;
            i++;
        }
    }
    return 0;
}

// Handle pipe commands
int do_pipe(char **left_cmd, char **right_cmd) {
    int pfd[2];

    if (pipe(pfd) == -1) {
        perror("pipe failed");
        return -1;
    }

    int lpid = fork();
    if (lpid == -1) {
        perror("fork failed");
        return -1;
    }

    if (lpid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        redirection(left_cmd);
        execvp(left_cmd[0], left_cmd);
        perror("execvp failed");
        exit(EXIT_FAILURE);
    }

    int rpid = fork();
    if (rpid == -1) {
        perror("fork failed");
        return -1;
    }

    if (rpid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[1]);
        close(pfd[0]);
        redirection(right_cmd);
        execvp(right_cmd[0], right_cmd);
        perror("execvp failed");
        exit(EXIT_FAILURE);
    }

    close(pfd[0]);
    close(pfd[1]);

    waitpid(lpid, NULL, 0);
    waitpid(rpid, NULL, 0);

    return 0;
}

// Parsing commands
char **parse_command(char *command) {
    char **args = malloc(MAX_ARGS * sizeof(char *));
    char *cmd_copy = strdup(command);
    char *token;
    int index = 0;

    token = strtok(cmd_copy, " ");
    while (token != NULL && index < MAX_ARGS - 1) {
        args[index++] = strdup(token);
        token = strtok(NULL, " ");
    }
    args[index] = NULL;

    free(cmd_copy);
    return args;
}

// Handle pipes
int split_pipe(char *command, char **left_cmd, char **right_cmd) {
    char *token = strtok(command, "|");
    if (token) {
        *left_cmd = token;
        token = strtok(NULL, "|");

        if (token) {
            *right_cmd = token;
            return 1;
        }
        *right_cmd = NULL;
        return 0;
    }
    *left_cmd = command;
    *right_cmd = NULL;
    return 0;
}

// Signal handlers
void sigint_handler(int sig) {
    // Handle Ctrl+C (SIGINT)
    if (foreground_pid > 0) {
        kill(foreground_pid, SIGINT);  // Send SIGINT to the foreground job
    }
}

void sigtstp_handler(int sig) {
    // Handle Ctrl+Z (SIGTSTP)
    if (foreground_pid > 0) {
        kill(foreground_pid, SIGTSTP);  // Suspend the foreground job
    }
}

void sigchld_handler(int sig) {
    // Handle child process cleanup
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            remove_job(pid);  // Clean up finished jobs
        }
    }
}

void ysh_loop() {
    int cpid;
    char *inString;
    char **parsedcmd;
    int if_bg; //background

    pid_t pgid = 0;
    pid_t pipe_pids[MAX_PIDS]; //used in pipe for multiple PIDs

    // Setup signal handlers
    signal(SIGINT, sigint_handler);    // Handle Ctrl+C
    signal(SIGTSTP, sigtstp_handler);  // Handle Ctrl+Z
    signal(SIGCHLD, sigchld_handler);  // Handle child process cleanup

    char *left_cmd = NULL, *right_cmd = NULL;

    while ((inString = readline("# "))) {

        if (current_command_line != NULL) {
            free(current_command_line);
        }
        current_command_line = strdup(inString);

        if_bg = 0;

        if (strcmp(inString, "jobs") == 0) {
            list_jobs();
            free(inString);
            continue;
        }

        if (strncmp(inString, "fg", 2) == 0) {
            fg_command();
            free(inString);
            continue;
        }

        if (strncmp(inString, "bg", 2) == 0) {
            bg_command();
            free(inString);
            continue;
        }

        if (strstr(inString, "&") != NULL) {
            if_bg = 1;
            inString[strcspn(inString, "&")] = '\0';  // Remove `&`
        }

        if (split_pipe(inString, &left_cmd, &right_cmd)) {
            // If a pipe is found, split and handle the pipe
            char **parsed_left_cmd = parse_command(left_cmd);
            char **parsed_right_cmd = parse_command(right_cmd);

            do_pipe(parsed_left_cmd, parsed_right_cmd);

            if (if_bg) {
                // Add piped command as a background job using the first process's PGID
                add_job(pipe_pids[0], inString, RUNNING, 0);
            } else {
                foreground_pid = cpid;
                //tcsetpgrp(STDIN_FILENO, getpid());  // Return control to the shell
                foreground_pid = -1;
            }

            free(parsed_left_cmd);
            free(parsed_right_cmd);
        }
        else if ((strstr(inString, "<") != NULL) || (strstr(inString, ">") != NULL)){
            parsedcmd = parse_command(inString);
            cpid = fork();

            if (cpid == 0) {
                setpgid(0, 0);  // Set PGID to the child's PID
                redirection(parsedcmd);
                execvp(parsedcmd[0], parsedcmd);
                perror("execvp failed");
                exit(EXIT_FAILURE);
            } else {
                setpgid(cpid, cpid);  // Set the PGID of the child to its PID
                if (if_bg) {
                    // If it's a background task, add to jobs list
                    add_job(cpid, inString, RUNNING, 0);
                } else {
                    // Foreground task
                    foreground_pid = cpid;

                    waitpid(cpid, NULL, 0);  // Wait for the foreground process to finish
                    //tcsetpgrp(STDIN_FILENO, getpid());  // Return control to the shell
                    foreground_pid = -1;
                }
            }
        }
        else {
            parsedcmd = parse_command(inString);  // Parse the command into arguments
            cpid = fork();

            if (cpid == 0) {
                setpgid(0, 0);  //set pgid to child's pid
                execvp(parsedcmd[0], parsedcmd);
                perror("execvp failed");
                exit(EXIT_FAILURE);
            }
            else if (cpid > 0) {  // Parent process
                setpgid(cpid, cpid);

                if (if_bg) {
                    add_job(cpid, inString, RUNNING,0);  // Add the job to the jobs list
                } else {

                    pid_t shell_pgrp = tcgetpgrp(STDIN_FILENO);
                    if (shell_pgrp != getpid()) {
                        printf("Shell is not in control of the terminal\n");
                    }

                    foreground_pid = cpid;
                    //tcsetpgrp(STDIN_FILENO, cpid);

                    int status;
                    waitpid(cpid, &status, WUNTRACED); // Wait for the foreground job to finish
                    //tcsetpgrp(STDIN_FILENO, getpid());  // Return control to the shell
                    foreground_pid = -1;
                }
            } else {
                perror("fork failed");
            }

            free(parsedcmd);
        }

        free(inString);
    }
}
