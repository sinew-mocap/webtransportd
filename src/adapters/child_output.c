/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — child_output.c
 *
 * The child-stdout reader. Runs on its own thread, decodes frames with
 * the domain codec, and drives the session core. All of the OS-shaped
 * machinery the hexagon must not contain — read(), pthreads, the raw
 * fd — lives here.
 */

#include "child_output.h"

#include "frame.h"
#include "session.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Large enough to hold one full-size frame (flag + 4-byte varint + max
 * payload) without forcing a second read round-trip. */
#define WTD_READ_BUF_SIZE (1 + 4 + WTD_FRAME_MAX_PAYLOAD)

static void *reader_main(void *arg) {
	wtd_child_output_t *o = (wtd_child_output_t *)arg;
	uint8_t *buf = (uint8_t *)malloc(WTD_READ_BUF_SIZE);
	if (buf == NULL) {
		wtd_session_on_output_eof(o->session);
		if (o->wake != NULL) {
			o->wake(o->wake_ctx);
		}
		return NULL;
	}
	size_t used = 0;

	for (;;) {
		if (used >= WTD_READ_BUF_SIZE) {
			/* No space to make progress — pathological peer or a
			 * decode bug. Stop cleanly. */
			break;
		}
		ssize_t n = read(o->fd, buf + used, WTD_READ_BUF_SIZE - used);
		if (n == 0) {
			break; /* EOF */
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		used += (size_t)n;

		for (;;) {
			size_t consumed = 0;
			uint8_t flag = 0;
			const uint8_t *payload = NULL;
			size_t plen = 0;
			wtd_frame_status_t st = wtd_frame_decode(buf, used,
					&consumed, &flag, &payload, &plen);
			if (st == WTD_FRAME_INCOMPLETE) {
				break;
			}
			if (st != WTD_FRAME_OK) {
				goto done;
			}
			wtd_session_on_output(o->session, flag, payload, plen);
			if (o->wake != NULL) {
				o->wake(o->wake_ctx);
			}
			memmove(buf, buf + consumed, used - consumed);
			used -= consumed;
		}
	}

done:
	wtd_session_on_output_eof(o->session);
	if (o->wake != NULL) {
		o->wake(o->wake_ctx);
	}
	free(buf);
	return NULL;
}

int wtd_child_output_start(wtd_child_output_t *o, int fd,
		struct wtd_session *session, wtd_wake_fn wake, void *wake_ctx) {
	if (o == NULL || fd < 0 || session == NULL) {
		return -EINVAL;
	}
	if (o->thread_started) {
		return -EALREADY;
	}
	o->fd = fd;
	o->session = session;
	o->wake = wake;
	o->wake_ctx = wake_ctx;
	int rc = pthread_create(&o->thread, NULL, reader_main, o);
	if (rc != 0) {
		return -rc;
	}
	o->thread_started = 1;
	return 0;
}

void wtd_child_output_stop(wtd_child_output_t *o) {
	if (o == NULL || !o->thread_started) {
		return;
	}
	(void)pthread_join(o->thread, NULL);
	o->thread_started = 0;
}
