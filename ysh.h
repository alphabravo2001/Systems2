// ysh.h: Header file for ysh.c

#include <sys/types.h>
#include <unistd.h>

#ifndef YSH_H
#define YSH_H

#define MAX_ARGS 10
#define MAX_JOBS 20
#define MAX_PIDS 10
#define STACK_SIZE 100

// Job status enum for tracking running, suspended, or done jobs
typedef enum { RUNNING, SUSPENDED, DONE } JobStatus;

// Job structure for managing background/foreground jobs
typedef struct _Job {
    int job_id;
    pid_t pgid;        // Process group ID
    JobStatus status;  // Status of the job
    char command[256]; // Command associated with the job
    struct _Job* next; // Pointer to the next job in the list
} Job;

// Declare global variables
extern pid_t stopped_stack[STACK_SIZE];
extern int stack_top;
extern pid_t foreground_pid;
extern Job *head;
extern Job *tail;
extern int job_count;
extern char *current_command_line;

// Declare signal handler functions so server.c can use them
void sigint_handler(int sig);    // Handle Ctrl+C (SIGINT)
void sigtstp_handler(int sig);   // Handle Ctrl+Z (SIGTSTP)
void sigchld_handler(int sig);   // Handle child process cleanup

// Function declarations for job control
void push(pid_t pid);
pid_t pop();
pid_t peek();
void add_job(pid_t pgid, char *command, JobStatus status, int print);
void remove_job(pid_t pid);
Job* find_job(pid_t pgid);
void list_jobs();
void fg_command();
void bg_command();

// Command and execution handling
int redirection(char **args);
int do_pipe(char **left_cmd, char **right_cmd);
char **parse_command(char *command);
int split_pipe(char *command, char **left_cmd, char **right_cmd);

// Main loop for the ysh shell
void ysh_loop();  // Declaration of the main ysh loop

#endif

