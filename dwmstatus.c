/*
 * dwmstatus.c
 *
 * See LICENSE file for copyright and license details.
 */

#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ONE_THOUSAND (1000)
#define ARRAY_LEN(x) (sizeof(x) / sizeof(x[0]))
#define BUFSIZE 4096

enum pipe_direction {
	PIPE_READ,
	PIPE_WRITE,
};

struct field {
	/*
	 * Function to populate a field.  Function should print desired field
	 * contents to stdout.  Trailing newlines are automatically stripped.
	 */
	int (*run)(void);
	/*
	 * Function which returns an inotify file descriptor used to trigger a
	 * field update.
	 *
	 * This is optional; it may be NULL.  Some fields may prefer to use
	 * .poll below, or use both this and .poll.
	 */
	int (*init)(void);
	/*
	 * Set to true to have the fields update every minute on the minute.
	 *
	 * This may be used in conjunction with .init().
	 */
	int poll;
	/*
	 * If set to true, executes .run() inline with the rest of the program.
	 *
	 * Otherwise, the field's .run() is forked off to run in its own
	 * process to avoid blocking other fields.  The field contents are
	 * updated when it returns.
	 */
	int synchronous;
};

struct state {
	/*
	 * pipe to capture field output
	 */
	int pipe[2];
	/*
	 * Buffer to cache output from pipe.
	 */
	char buf[PIPE_BUF];
	/*
	 * Process pid when not using synchronous.
	 */
	pid_t pid;
};

void clear_pipe(int fd)
{
	char buf[PIPE_BUF];
	while (read(fd, buf, sizeof(buf)) > 0) {
		;
	}
}

#include "config.h"

#define FCNT ARRAY_LEN(fields)
struct pollfd pollfds[FCNT + 1];
struct state states[FCNT];

static int set_status(Display *dpy, Window root, char buf[BUFSIZE])
{
	/*
	 * Spamming XFlush() will cause dwm to stall trying to catch up.  If
	 * there's no new content, don't send anything to dwm to minimize this
	 * concern.
	 */
	static char prev_buf[BUFSIZE];
	static int prev_buf_set = 0;
	if (prev_buf_set == 0) {
		prev_buf[0] = '\0';
		prev_buf_set = 1;
	}
	if (strcmp(buf, prev_buf) == 0) {
		return 0;
	}
	strncpy(prev_buf, buf, sizeof(prev_buf));

	if (XStoreName(dpy, root, buf) < 0) {
		perror("XStoreName");
		return -1;
	}

	if (XFlush(dpy) < 0) {
		perror("XFlush");
		return -1;
	}

	return 0;
}

static int setup_signal_handling(struct pollfd *pollfd)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGHUP);
	sigaddset(&mask, SIGCHLD);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("sigprocmask");
		return -1;
	}

	if ((pollfd->fd = signalfd(-1, &mask,
					SFD_CLOEXEC | SFD_NONBLOCK)) < 0) {
		perror("signalfd");
		return -1;
	}
	pollfd->events = POLLIN;

	return 0;
}

static int initialize_fields(void)
{
	for (size_t i = 0; i < FCNT; i++) {
		if (pipe2(states[i].pipe, O_NONBLOCK) < 0) {
			perror("pipe2");
			return -1;
		}

		if (fields[i].init && (pollfds[i].fd = fields[i].init()) >= 0) {
			pollfds[i].events = POLLIN;
		} else {
			pollfds[i].fd = -1;
			pollfds[i].events = 0;
			pollfds[i].revents = 0;
		}

		states[i].buf[0] = '\0';
		states[i].pid = -1;
	}

	return 0;
}

static void run_field(size_t i)
{
	if (!fields[i].run) {
		return;
	}

	if (states[i].pid > 0) {
		return;
	}

	if (fields[i].synchronous) {
		clear_pipe(states[i].pipe[PIPE_READ]);
		dup2(states[i].pipe[PIPE_WRITE], STDOUT_FILENO);

		if (fields[i].run() != 0) {
			strcpy(states[i].buf, "<error>");
		} else {
			memset(states[i].buf, '\0', sizeof(states[i].buf));
			fflush(stdout);
			int r = read(states[i].pipe[PIPE_READ], states[i].buf,
					sizeof(states[i].buf)-1);
			if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				strcpy(states[i].buf, "<error>");
			}
		}

		return;
	}

	states[i].pid = fork();

	if (states[i].pid > 0) {
		return;
	} else if (states[i].pid < 0) {
		strcpy(states[i].buf, "<error>");
		return;
	} else {
		clear_pipe(states[i].pipe[PIPE_READ]);
		dup2(states[i].pipe[PIPE_WRITE], STDOUT_FILENO);

		int r = fields[i].run();
		fflush(stdout);
		if (r != 0) {
			clear_pipe(states[i].pipe[PIPE_READ]);
			printf("<error>");
		}
		exit(r);
	}
}

static void cat_bufs(char *buf, size_t len)
{
	buf[0] = '\0';
	size_t w = 0;
	for (size_t i = 0; i < FCNT; i++) {
		char *s, *e;
		for (s = states[i].buf, e = index(s, '\n'); e && *e;
				e = index(s, '\n') ) {
			*e = '\0';
			w = strncat(buf, s, len - w) - buf;
			*e = '\n';
			s = e+1;
		}
		w = strncat(buf, s, len - w) - buf;
	}
}

int get_ms_until_next_minute(void)
{
	struct timeval tv;
	struct tm *now;
	if (gettimeofday(&tv, NULL) < 0 ||
			(now = localtime(&tv.tv_sec)) == NULL) {
		return 60 * ONE_THOUSAND;
	} else {
		return ((60 - now->tm_sec) * ONE_THOUSAND)
			- (tv.tv_usec / ONE_THOUSAND);
	}
}

void handle_sigchld(void)
{
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		for (size_t i = 0; i < FCNT; i++) {
			if (states[i].pid != pid) {
				continue;
			}
			states[i].pid = 0;
			if (WEXITSTATUS(status) == 0) {
				memset(states[i].buf, '\0',
						sizeof(states[i].buf));
				int r = read(states[i].pipe[PIPE_READ],
						states[i].buf,
						sizeof(states[i].buf)-1);
				if (r < 0 && errno != EAGAIN &&
						errno != EWOULDBLOCK) {
					strcpy(states[i].buf, "<error>");
				}
			} else {
				strcpy(states[i].buf, "<error>");
			}
		}
	}
}

int main(void)
{
	Display *dpy = XOpenDisplay(NULL);
	if (!dpy) {
		perror("XOpenDisplay");
		return -1;
	}
	Window root = XRootWindow(dpy, DefaultScreen(dpy));

	if (set_status(dpy, root, "Loading...") < 0) {
		fprintf(stderr, "Could not set status.\n");
		return -1;
	}

	if (setup_signal_handling(&pollfds[FCNT]) < 0) {
		fprintf(stderr, "Could not setup signal handling.\n");
		return -1;
	}

	if (initialize_fields() < 0) {
		fprintf(stderr, "Could not initialize fields.\n");
		return -1;
	}

	int force_trigger = 1;
	int poll_trigger = 0;
	for (;;) {
		for (size_t i = 0; i < FCNT; i++) {
			if (states[i].pid > 0) {
				continue;
			} else if (force_trigger) {
				run_field(i);
			} else if (poll_trigger && fields[i].poll) {
				run_field(i);
			} else if (pollfds[i].revents) {
				clear_pipe(pollfds[i].fd);
				run_field(i);
			}
		}
		force_trigger = 0;
		poll_trigger = 0;

		char buf[BUFSIZE];
		cat_bufs(buf, sizeof(buf));
		if (set_status(dpy, root, buf) < 0) {
			fprintf(stderr, "Could not set status\n");
			return -1;
		}

		if (poll(pollfds, FCNT+1, get_ms_until_next_minute()) == 0) {
			poll_trigger = 1;
		}

		if (pollfds[FCNT].revents) {
			struct signalfd_siginfo sinfo;
			if (read(pollfds[FCNT].fd, &sinfo, sizeof(sinfo)) < 0) {
				fprintf(stderr, "Could not read inotify fd\n");
				return -1;
			}

			switch (sinfo.ssi_signo) {
			case SIGHUP:
				force_trigger = 1;
				break;
			case SIGCHLD:
				handle_sigchld();
				break;
			}
		}
	}
}
