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
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/file.h>
#include <errno.h>
#include <syslog.h>

#include "msg.h"

int debug = 0, msg_fd = -2;

void msg(sev_t sev, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    int sz, i;

    if(msg_fd < -1) return;
    if(!debug && sev == sev_debug) return;

    if(msg_fd >= 0) {
        switch(sev) {
	case sev_debug:
	    strcpy(buf, "DEBUG: ");
	    sz = 7;
	    break;
	case sev_normal:
	    sz = 0;
	    break;
	case sev_error:
	    strcpy(buf, "ERROR: ");
	    sz = 7;
	    break;
	case sev_critical:
	    strcpy(buf, "CRITICAL: ");
	    sz = 10;
	    break;
	}
    } else {
	sz = 0;
        switch(sev) {
	case sev_debug:
	    i = LOG_DEBUG;
	    break;
	case sev_normal:
	    i = LOG_NOTICE;
	    break;
	case sev_error:
	    i = LOG_ERR;
	    break;
	case sev_critical:
	    i = LOG_CRIT;
	    break;
	}
    }
    va_start(ap, fmt);
    vsnprintf(&buf[sz], sizeof(buf) - sz - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = '\0';

    if(msg_fd < 0) {
	syslog(i, "%s", buf);
	return;
    }

    sz = strlen(buf);
    if(sz) {
	i = 0;
	if(sz < sizeof(buf) - 1) {
	    buf[sz] = '\n';
	    sz++;
	    buf[sz] = '\0';
	} else
	    buf[sz-1] = '\n';

	while(flock(msg_fd, LOCK_EX) < 0) {
	    if(errno == EINTR) continue;
	    return;
	}
    
	while(sz) {
	    int done = write(msg_fd, &buf[i], sz);
	    if(done <0) {
		if(errno == EINTR) continue;
		break;
	    }
	    sz -= done;
	    i += done;
	}
	while(flock(msg_fd, LOCK_UN) < 0) {
	    if(errno == EINTR) continue;
	    return;
	}
    }
}
