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

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <pwd.h>
#include <getopt.h>
#include <syslog.h>

#include "msg.h"
#include "net.h"

static void help(const char *me) {
    printf("Usage: %s <options>\n\
Options:\n\
  %-32s creates socket in <path> (mandatory)\n\
  %-32s set socket mode to <mode> (default: 666)\n\
  %-32s change to user <user> (default: mail)\n\
  %-32s writes pid to the file in <path> (default: no pidfile)\n\
  %-32s do not fork into background (default: daemonize)\n\
  %-32s enable debug logging (default: debug is off)\n\
  %-32s set logfile to <path> (default: logging is disabled)\n\
  %-32s sets socket timeout to <msecs> (default: 10000)\n\
  %-32s limits the number of forked children to <num> (default: 20)\n\
\n\
  %-32s print this help\n\n",
	   me,
	   "-s, --socket <path>",
	   "-m, --socketmode <mode>",
	   "-u, --user <user>",
	   "-p, --pidfile <path>",
	   "-f, --foreground",
	   "-d, --debug",
	   "-l, --log <path> or --log syslog",
	   "-t, --timeout <msecs>",
	   "-c, --max-children <num>",
	   "-h, --help");
}

int main(int argc, char **argv) {
    char *sock = NULL, *pid = NULL, *user = "mail", pidd[32], *logfile = NULL;
    int foreground = 0, s, p_fd, pidsz, maxchld = 20;
    mode_t mode = 0666, mask;
    pid_t npid;
    const struct option long_options[] = {
	{"max-children", 1, NULL, 'c'},
	{"foreground", 0, NULL, 'f'},
	{"timeout", 1, NULL, 't'},
	{"pidfile", 1, NULL, 'p'},
	{"socket", 1, NULL, 's'},
	{"debug", 0, NULL, 'd'},
	{"mode", 1, NULL, 'm'},
	{"user", 1, NULL, 'u'},
	{"log", 1, NULL, 'l'},
	{"help", 0, NULL, 'h'},
	{NULL, 0, NULL, 0}
    };
    struct sockaddr_un sa;
    struct passwd *dropto;

    while(1) {
	int arg;
	arg = getopt_long(argc, argv, ":fp:s:m:u:l:dt:c:h", long_options, NULL);
	if(arg == -1)
	    break;
	switch(arg) {
	case 'f':
	    foreground = 1;
	    break;
	case 'p':
	    pid = optarg;
	    break;
	case 's':
	    sock = optarg;
	    break;
	case 'm':
	    mode = strtol(optarg, NULL, 8);
	    if(mode<=0 || mode>0777) {
		printf("ERROR: bad socket mode %s\n", optarg);
		return 1;
	    }
	    break;
	case 'u':
	    user = optarg;
	    break;
	case 'l':
	    logfile = optarg;
	    break;
	case 'd':
	    debug = 1;
	    break;
	case 't':
	    timeout_msec = atoi(optarg);
	    if(timeout_msec<=0) {
		printf("ERROR: bad timeout value %s\n", optarg);
		return 1;
	    }
	    break;
	case 'c':
	    maxchld = atoi(optarg);
	    if(maxchld<=0) {
		printf("ERROR: bad max-children count %s\n", optarg);
		return 1;
	    }
	    break;
	case 'h':
	case ':':
	    help(argv[0]);
	    return 0;
	}
    }

    if(!sock) {
	printf("ERROR: No socket provided!\n");
	help(argv[0]);
	return 1;
    }

    if(logfile) {
	if(!strcasecmp(logfile, "syslog")) {
	    openlog("exim-quotad", LOG_PID, LOG_MAIL);
	    msg_fd = -1;
	} else {
	    msg_fd = open(logfile, O_WRONLY|O_CREAT|O_APPEND, 0644);
	    if(msg_fd < 0) {
		printf("ERROR: failed to open logfile %s in append mode: %s\n", logfile, strerror(errno));
		return 1;
	    }
	}
    }

    errno = 0;
    if(!(dropto = getpwnam(user))) {
	printf("ERROR: lookup failed for user %s: %s\n", pid, errno ? strerror(errno) : "user not found");
	return 1;
    }
    if(geteuid() && geteuid() != dropto->pw_uid) {
	printf("ERROR: cannot run as user %s (uid %d) unless started (or setuid) as root or %s\n", user, dropto->pw_uid, user);
	return 1;
    }

    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sock, sizeof(sa.sun_path));
    sa.sun_path[sizeof(sa.sun_path) - 1] = '\0';
    if(strcmp(sa.sun_path, sock)) {
	printf("ERROR: Socket name too long\n");
	return 1;
    }

    if(pid) {
	p_fd = open(pid, O_WRONLY|O_CREAT|O_EXCL, 0644);
	while(p_fd<0) {
	    if(errno == EEXIST) {
		p_fd = open(pid, O_RDWR);
		if(p_fd>=0) {
		    pidsz = read(p_fd, pidd, sizeof(pidd)-1);
		    if(pidsz>=0) {
			pidd[pidsz] = '\0';
			npid = atoi(pidd);
			if(npid>0) {
			    if(kill(npid, 0) == -1) {
				if(errno == ESRCH) {
				    lseek(p_fd, 0, SEEK_SET);
				    ftruncate(p_fd, 0);
				    break;
				}
				printf("ERROR: pidfile exists and process %d cannot be signalled: %s\n", npid, strerror(errno));
				return 1;
			    }
			    printf("ERROR: pidfile exists and process %d is alive\n", npid);
			    return 1;
			}
			printf("ERROR: pidfile exists with bogus content\n");
			return 1;
		    }
		}
		printf("ERROR: failed to read pid file %s: %s\n", pid, strerror(errno));
		return 1;
	    }
	    break;
	}
	if(p_fd<0) {
	    printf("ERROR: failed to create pid file %s: %s\n", pid, strerror(errno));
	    return 1;
	}
    }

    if((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	printf("ERROR: socket creation failed: %s\n", strerror(errno));
	return 1;
    }
    mask = umask(0777);
    while(bind(s, (const struct sockaddr *)&sa, sizeof(sa))) {
	if(errno == EADDRINUSE) {
	    if(unlink(sa.sun_path)<0) {
		printf("ERROR: failed to remove existing socket: %s\n",  strerror(errno));
		return 1;
	    }
	    if(!bind(s, (const struct sockaddr *)&sa, sizeof(sa)))
		break;
	}
	printf("ERROR: failed to bind socket: %s\n",  strerror(errno));
	return 1;
    }
    umask(mask);
    if(chmod(sa.sun_path, mode)) {
	printf("ERROR: failed to set socket mode: %s\n",  strerror(errno));
	return 1;
    }
    if(listen(s, 10)) {
	printf("ERROR: cannot listen on socket: %s\n",  strerror(errno));
	return 1;
    }

    if(chdir("/")) {
	printf("ERROR: cannot chdir to root\n");
	return 1;
    }

    if(setgid(dropto->pw_gid)) {
	printf("ERROR: setgid(%d) failed\n", dropto->pw_gid);
	return 1;
    }
    if(setuid(dropto->pw_uid)) {
	printf("ERROR: setgid(%d) failed\n", dropto->pw_uid);
	return 1;
    }

    if(!foreground) {
	int nulfd;
	pid_t pid = fork();
	if(pid == -1) {
	    printf("ERROR: failed to fork into background: %s\n", strerror(errno));
	    return 1;
	}
	if(pid>0)
	    return 0;
	umask(0);
	if(setsid()<0) {
	    msg(sev_critical, "failed to get a new session: %s", strerror(errno));
	    return 1;
	}

	if((nulfd = open("/dev/null", O_RDONLY))<0 || dup2(nulfd, 0) < 0) {
	    msg(sev_critical, "failed to replace stdin: %s", strerror(errno));
	    return 1;
	}
	if((nulfd = open("/dev/null", O_WRONLY))<0 || dup2(nulfd, 1) < 0) {
	    msg(sev_critical, "failed to replace stdout: %s", strerror(errno));
	    return 1;
	}
	if((nulfd = open("/dev/null", O_WRONLY))<0 || dup2(nulfd, 2) < 0) {
	    msg(sev_critical, "failed to replace stderr: %s", strerror(errno));
	    return 1;
	}
    }

    if(pid) {
	sprintf(pidd, "%d", getpid());
	pidsz = strlen(pidd);
	if(write(p_fd, pidd, pidsz) < pidsz) {
	    msg(sev_critical, "failed to write pid file %s: %s", pid, strerror(errno));
	    return 1;
	}
	close(p_fd);
    }

    server(s, maxchld);

    return 0;
}

