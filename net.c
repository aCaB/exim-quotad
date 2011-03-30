/*
 *  Copyright (C) 2011 aCaB
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>

#include "msg.h"
#include "net.h"

unsigned int timeout_msec = 10 * 1000;
static sem_t sem;


static void sig_chld(int signo, siginfo_t *si, void *ctx) {
    ctx = ctx;
    if(signo != SIGCHLD || !si || si->si_signo != SIGCHLD)
	return;
    if(si->si_code != CLD_EXITED && si->si_code != CLD_KILLED && si->si_code != CLD_DUMPED)
	return;
    waitpid(si->si_pid, NULL, 0);
    sem_post(&sem);
}

void sendres(int sock, char *line) {
    int len = strlen(line);
    while(len) {
	int res = send(sock, line, len, 0);
	if(!res) {
	    msg(sev_normal, "remote disconnected (response time)");
	    return;
	}
	if(res < 0) {
	    if(errno == EINTR) continue;
	    msg(sev_error, "send fail in worker: %s", strerror(errno));
	    return;
	}
	len -= res;
	line += res;
    }
}

#define SENDOK do { sendres(sock, "0\n"); close(sock); return; } while(0)
#define MDSBUFSZ 5120

static void handle_req(int sock) {
    struct timeval t_now, t_temp = { timeout_msec / 1000 , (timeout_msec % 1000) * 1000 }, t_end;
    char fname[8192], buf[MDSBUFSZ + 2];
    fd_set fds;
    int res, avail = sizeof(fname), done = 0;
    struct stat st;

    msg(sev_debug, "accepted connection");

    if(gettimeofday(&t_now, NULL)) {
	msg(sev_error, "gettimeoufday() failed in worker: %s", strerror(errno));
	close(sock);
	return;
    }
    timeradd(&t_now, &t_temp, &t_end);

    while(1) {
	if(gettimeofday(&t_now, NULL)) {
	    msg(sev_error, "gettimeoufday() failed in worker: %s", strerror(errno));
	    close(sock);
	    return;
	}
	if(timercmp(&t_now, &t_end, >=)) {
	    msg(sev_normal, "worker timed out");
	    close(sock);
	    return;
	}

	timersub(&t_end, &t_now, &t_temp);
	FD_ZERO(&fds);
	FD_SET(sock, &fds);
	res = select(sock+1, &fds, NULL, NULL, &t_temp);
	if(res < 0) {
	    if(errno == EINTR) continue;
	    msg(sev_error, "select() failed in worker: %s", strerror(errno));
	    close(sock);
	    return;
	}
	if(res == 0){
	    msg(sev_normal, "worker timed out");
	    close(sock);
	    return;
	}

	while(1) {
	    res = recv(sock, &fname[done], avail, 0);
	    if(res < 0) {
		if(errno == EINTR) continue;
		msg(sev_normal, "recv failed in worker: %s", strerror(errno));
		close(sock);
		return;
	    }
	    if(!res) {
		msg(sev_normal, "remote disconnected (command time)");
		close(sock);
		return;
	    }
	    break;
	}
	if(fname[done + res -1] == '\n') {
	    done += res - 1;
	    fname[done] = '\0';
	    break;
	}
	avail -= res;
	done += res;
    }

    if(*fname != '/') {
	msg(sev_error, "refusing to handle request for non absolute path %s", fname);
	SENDOK;
    }

    if(stat(fname, &st)) {
	msg(sev_debug, "stat failed for %s: %s", fname, strerror(errno));
	SENDOK;
    }

    if(S_ISREG(st.st_mode)) { /* MBOX handler */
	msg(sev_debug, "query for mbox %s", fname);
	sprintf(buf, "%llu\n", (unsigned long long)st.st_size);
	sendres(sock, buf);
	close(sock);
	return;
    } else if(S_ISDIR(st.st_mode)) { /* MAILDIR handler */
	int d, line = 0, unparsed = 0;
	long long sum = 0;

	msg(sev_debug, "query for maildir %s", fname);
	if(sizeof(fname) - done <= 13) {
	    msg(sev_normal, "maildir name %s too long", fname);
	    SENDOK;
	}

	memcpy(&fname[done], "/maildirsize", 13);
	if(stat(fname, &st)) {
	    msg(sev_debug, "stat failed for %s: %s", fname, strerror(errno));
	    SENDOK;
	}

	if((d = open(fname, O_RDONLY)) < 0) {
	    msg(sev_debug, "cannot open %s: %s", fname, strerror(errno));
	    SENDOK;
	}
	while(flock(d, LOCK_SH)) {
	    if(errno == EINTR) continue;
	    msg(sev_normal, "cannot lock %s: %s", fname, strerror(errno));
	    close(d);
	    SENDOK;
	}

	while(st.st_size) {
	    int todo = (st.st_size < MDSBUFSZ - unparsed) ? st.st_size : MDSBUFSZ - unparsed, got = 0;
	    char *cur = buf, *next;

	    got = read(d, &buf[unparsed], todo);
	    if(got < 0) {
		if(errno == EINTR) continue;
		msg(sev_error, "cannot read %s: %s", fname, strerror(errno));
		sum = 0;
		break;
	    }
	    if(!got) {
		msg(sev_normal, "short read from %s: %u bytes still expected", fname, st.st_size);
		sum = 0;
		break;
	    }
	    st.st_size -= got;
	    if(st.st_size || buf[got - 1] == '\n')
		buf[got] = '\0';
	    else {
		buf[got] = '\n';
		buf[got+1] = '\0';
	    }

	    while((next = strchr(cur, '\n'))) {
		*next = '\0';
		if(++line == 1) { /* Parse header line */
		    int expect = 0;

		    do {
			char c = *cur;
			cur++;
			switch(expect) {
			case 0: /* number */
			    if(c < '0' || c > '9')
				expect = -1;
			    else
				expect++;
			    break;
			case 1: /* number or letter */
			    if(c >= '0' && c <= '9') {
			    } else if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
				expect++;
			    else
				expect = -1;
			    break;
			case 2: /* comma or EOL */
			    if(c == ',')
				expect = 0;
			    else if(!c)
				expect = -2;
			    else
				expect = -1;
			}
		    } while (expect >= 0);
		    if(expect == -1) {
			msg(sev_normal, "bad header in %s", fname);
			break;
		    }
		} else { /* Parse body lines */
		    long long sz;
		    if(sscanf(cur, "%lld %*d", &sz) != 1) {
			msg(sev_normal, "bad quota format in %s:%u", fname, line);
			break;
		    }
		    sum += sz;
		}
		cur = next + 1;
	    }
	    if(next) {
		sum = 0;
		break;
	    }
	    unparsed = strlen(cur);
	    if(unparsed == MDSBUFSZ) {
		msg(sev_normal, "line too long in %s", fname);
		sum = 0;
		break;
	    }
	    memmove(buf, cur, unparsed);
	}

	while(flock(d, LOCK_UN)) {
	    if(errno == EINTR) continue;
	    msg(sev_error, "cannot unlock %s: %s", fname, strerror(errno));
	    break;
	}
	close(d);

	if(sum < 0) sum = 0;
	msg(sev_debug, "query for maildirsize %s: %llu", fname, sum);
	sprintf(buf, "%llu\n", sum);
	sendres(sock, buf);
	close(sock);
	return;
    } else {
	msg(sev_debug, "query for unsupported file %s", fname);
	SENDOK;
    }
}


void server(int s, unsigned int maxchld) {
    struct sockaddr_un sa;
    struct sigaction act;
    socklen_t salen;
    int conns;
    pid_t pid;

    msg(sev_normal, "server started");

    if(sem_init(&sem, 0, maxchld)) {
	msg(sev_critical, "failed to init semaphore: %s", strerror(errno));
	return;
    }

    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGUSR1, &act, NULL);
    sigaction(SIGUSR2, &act, NULL);

    act.sa_handler = NULL;
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = sig_chld;
    sigaction(SIGCHLD, &act, NULL);

    while(1) {
	salen = sizeof(sa);
	conns = accept(s, (struct sockaddr *)&sa, &salen);
	if(conns < 0) {
	    if(errno == EINTR) continue;
	    msg(sev_critical, "failed to accept incoming connection: %s", strerror(errno));
	    return;
	}

	pid = fork();
	if(pid < 0) {
	    msg(sev_critical, "failed to fork worker: %s", strerror(errno));
	    return;
	}

	if(!pid) {
	    handle_req(conns);
	    exit(0);
	}
	close(conns);
	while(sem_wait(&sem)) {
	    if(errno == EINTR) continue;
	    msg(sev_critical, "failed to wait on semaphore: %s", strerror(errno));
	    return;
	}
    }
}

