#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>

#define MAXLINE 80
#define MAXARGC 80
#define MAXJOBS 5

struct JOB {
	pid_t PID;
	char *status;
	char command_line[MAXLINE];
	int fg;
};
struct JOB jobs[5];


// Function prototypes
void kill_and_reap_all_children();
void SIGINT_handler(int);
void SIGCHLD_handler(int);
void SIGTSTP_handler(int);
int parseline(char*, char*[]);
void IO_redirection_handler(char*[], int*, int*, int*, int*);
int builtin_command(char*[]);
void eval(char*);


int main() {
	// Register signal handlers
	signal(SIGINT, SIGINT_handler);
	signal(SIGTSTP, SIGTSTP_handler);
	signal(SIGCHLD, SIGCHLD_handler);

	char cmdline[MAXLINE];
	int i = 0;

	// Initialize instances of JOB struct
	for (; i < MAXJOBS; i++) jobs[i].PID = -1;

	while (1) {
		int saved_stdin = dup(0);
		int saved_stdout = dup(1);

		printf("prompt> ");
		fgets(cmdline, MAXLINE, stdin);
		if (feof(stdin))
			exit(0);

		// evaluate
		eval(cmdline);

		// restore standart I/O
		dup2(saved_stdin, 0);
		dup2(saved_stdout, 1);
	}
}


void kill_and_reap_all_children() {
	int i = 0;
	for (; i < MAXJOBS; i++)
		if (jobs[i].PID != -1) {
			kill(jobs[i].PID, SIGINT);
			waitpid(jobs[i].PID, NULL, 0);
			jobs[i].PID = -1;
		}
}


// Signal Handlers
void SIGINT_handler(int signal) {
	int i = 0;
	pid_t fg_pid = 0;

	// Locate the fg process pid and remove it from record
	for (; i < MAXJOBS; i++) {
		if (jobs[i].PID != -1 && jobs[i].fg == 1) {
			fg_pid = jobs[i].PID;
			jobs[i].PID = -1;
			break;
		}
	}

	if (fg_pid != 0) kill(fg_pid, signal);
	else {
		kill_and_reap_all_children();
		exit(0);
	}
}


void SIGCHLD_handler(int signal) {
	int i = 0;
	// Remove reaped processes from record
	for (; i < MAXJOBS; i++) {
		pid_t wpid = waitpid(jobs[i].PID, NULL, WNOHANG);
		if (wpid != 0) jobs[i].PID = -1;
	}
}


void SIGTSTP_handler(int signal) {
	int i = 0;
	pid_t fg_pid = 0;

	// Locate the fg process pid and change its status as well as the fg flag
	for (; i < MAXJOBS; i++) {
		if (jobs[i].PID != -1 && jobs[i].fg == 1) {
			fg_pid = jobs[i].PID;
			jobs[i].status = "Stopped";
			jobs[i].fg = 0;
			break;
		}
	}

	if (fg_pid != 0) kill(fg_pid, signal);
}


int parseline(char* buf, char *argv[]) {
	char *delim; // Points to first space delimiter
	int argc; // Number of args
	int bg; //Background job?

	buf[strlen(buf)-1] = ' '; // Replace trailing '\n' with space
	while (*buf && (*buf == ' ')) // Ignore leading spaces
		buf++;

	// Build the argv list
	argc = 0;
	while ((delim = strchr(buf, ' '))) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) // Ignore spaces
			buf++;
	}
	argv[argc] = NULL;

	if (argc == 0) //Ignore blank line
		return 1;

	// bg job?
	if ((bg = (*argv[argc-1] == '&')) != 0)
		argv[--argc] = NULL;

	return bg;
}


void IO_redirection_handler(char* argv[], int* inFileID, int* outFileID, int* redirect_stdin, int* redirect_stdout) {
	// I/O redirection Authorization
	int i, j;
	mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
	char *argv_temp[MAXARGC];

	char **rover = argv;
	for (i = 0; *rover; i++) {
		if (!strcmp(argv[i], "<")) {
			*redirect_stdin = 1;
			break;
		}
		rover++;
	}

	rover = argv;
	for (j = 0; *rover; j++) {
		if (!strcmp(argv[j], ">")) {
			*redirect_stdout = 1;
			break;
		}
		rover++;
	}

	if (*redirect_stdin == 1) {
		*inFileID = open(argv[i+1], O_RDONLY, mode);
		dup2(*inFileID, STDIN_FILENO);
		close(*inFileID);
		argv[i] = NULL;
		argv[i+1] = NULL;
	}

	if (*redirect_stdout == 1) {
		*outFileID = open(argv[j+1], O_CREAT|O_WRONLY|O_TRUNC, mode);
		dup2(*outFileID, STDOUT_FILENO);
		close(*outFileID);
		argv[j] = NULL;
		argv[j+1] = NULL;
	}
}


int builtin_command(char* argv[]) {
	int i = 0;

	if (!strcmp(argv[0], "quit")) {
		kill_and_reap_all_children();
		exit(0);
	}

	else if (!strcmp(argv[0], "fg")) {
		pid_t pid;
		int fg;

		if (argv[1][0] == '%') pid = jobs[atoi(&argv[1][1])-1].PID;
		else pid = atoi(argv[1]);

		// stop the process if it is a bg job
		for (i = 0; i < MAXJOBS; i++)
			if (jobs[i].PID == pid) {
				fg = jobs[i].fg;
				break;
			}

		if (fg == 0) kill(pid, SIGTSTP);

		// Continue running
		kill(pid, SIGCONT);
		jobs[i].status = "Running";
		jobs[i].fg = 1;
		waitpid(pid, NULL, WUNTRACED);

		return 1;
	}

	else if (!strcmp(argv[0], "bg")) {
		pid_t pid;
		int fg;

		if (argv[1][0] == '%') pid = jobs[atoi(&argv[1][1])-1].PID;
		else pid = atoi(argv[1]);

		// Locate the pid in jobs array
		for (i = 0; i < MAXJOBS; i++) if (jobs[i].PID == pid) break;

		if (jobs[i].fg == 0 && strcmp(jobs[i].status, "Running") == 0) return 1; // Already in bg and running

		// Continue running
		kill(pid, SIGCONT);
		jobs[i].status = "Running";
		jobs[i].fg = 0;

		return 1;
	}

	else if (!strcmp(argv[0], "kill")) {
		pid_t pid;

		if (argv[1][0] == '%') pid = jobs[atoi(&argv[1][1])-1].PID;
		else pid = atoi(argv[1]);

		kill(pid, SIGINT);
		waitpid(pid, NULL, WNOHANG);

		// Locate the pid in jobs array
		for (i = 0; i < MAXJOBS; i++) if (jobs[i].PID == pid) break;
		jobs[i].PID = -1;

		return 1;
	}

	else if (!strcmp(argv[0], "jobs")) {
		for (i = 0; i < MAXJOBS; i++)
			if (jobs[i].PID != -1)
				printf("[%d] (%d) %s %s\n", i+1, (int)jobs[i].PID, jobs[i].status, jobs[i].command_line);
		return 1;
	}

	else if (isalpha(argv[0][0]) == 0) return 1; // Invalid input


	return 0;
}


void eval(char *cmdline) {
	char *argv[MAXARGC]; // argv for execve()/exec()
	int bg; // should the job run in bg or fg?
	pid_t pid; // process id
	int i = 0, inFileID = -1, outFileID = -1, redirect_stdin = -1, redirect_stdout = -1;

	char original_cmdline[MAXLINE]; // The argument to this function gets modified by parseline() and IO_redirection_handler()
	for (i = 0; i < MAXLINE; i++) {
		if (cmdline[i] == '\n' || cmdline[i] == '\0') break;
		original_cmdline[i] = cmdline[i];
	}
	original_cmdline[i] = '\0';

	bg = parseline(cmdline, argv);

	IO_redirection_handler(argv, &inFileID, &outFileID, &redirect_stdin, &redirect_stdout);
	if (!builtin_command(argv)) {
		if ((pid = fork()) == 0) { // child runs user job
			if (execve(argv[0], argv, NULL) < 0 && execv(argv[0], argv) < 0 && execvp(argv[0], argv) < 0) {
				printf("%s: Command not found.\n", argv[0]);
				exit(0);
			}
		}

		if (!bg) { // parent waits for fg job to terminate
			for (i = 0; i < MAXJOBS; i++) {
				if (jobs[i].PID == -1) {
					jobs[i].PID = pid;
					jobs[i].status = "Running";
					strcpy(jobs[i].command_line, original_cmdline);
					jobs[i].fg = 1;
					break;
				}
			}
			waitpid(pid, NULL, WUNTRACED);
		}
		else {
			for (i = 0; i < MAXJOBS; i++) {
				if (jobs[i].PID == -1) {
					jobs[i].PID = pid;
					jobs[i].status = "Running";
					strcpy(jobs[i].command_line, original_cmdline);
					jobs[i].fg = 0;
					break;
				}
			}
			printf("%d %s\n", pid, cmdline);
		}
	}
}