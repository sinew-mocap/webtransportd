/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
#ifdef _WIN32
/* POSIX-only test (pipe + pthread reader). The Windows child_output
 * adapter would need a CreateProcess/_pipe harness; until that cycle
 * lands, skip on Windows so the build stays green. The body still
 * compiles and runs on linux-gcc + macos-clang. */
#include <stdio.h>
int main(void) {
	fprintf(stderr, "SKIP: POSIX-only test on Windows\n");
	return 0;
}
#else
/*
 * webtransportd — child_output_test.c
 *
 * The child-output driving adapter, end to end: write two encoded frames
 * (one reliable, one unreliable) into a pipe, point wtd_child_output at
 * the read end and a real session whose transport port is faked, then
 * close the write end so the reader sees EOF. After the wake callback has
 * fired for both frames and the EOF, pump the session and assert the
 * reliable frame routed to the stream, the unreliable to a datagram, and
 * that EOF produced exactly one FIN.
 */

#include "child_output.h"
#include "frame.h"
#include "session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

/* ---- fake transport port: record what the core flushes to the peer ---- */

typedef struct fake_transport {
	uint8_t stream[64];
	size_t stream_len;
	uint8_t dgram[64];
	size_t dgram_len;
	int fin_calls;
} fake_transport_t;

static int fake_send_stream(void *ctx, const uint8_t *data, size_t len) {
	fake_transport_t *t = (fake_transport_t *)ctx;
	memcpy(t->stream + t->stream_len, data, len);
	t->stream_len += len;
	return 0;
}
static int fake_send_datagram(void *ctx, const uint8_t *data, size_t len) {
	fake_transport_t *t = (fake_transport_t *)ctx;
	memcpy(t->dgram, data, len);
	t->dgram_len = len;
	return 0;
}
static int fake_send_fin(void *ctx) {
	((fake_transport_t *)ctx)->fin_calls++;
	return 0;
}

/* child port is unused in this direction. */
static long noop_write(void *c, const uint8_t *d, size_t n) {
	(void)c; (void)d; return (long)n;
}
static int noop_close(void *c) { (void)c; return 0; }

/* ---- wake counter ---- */

typedef struct {
	pthread_mutex_t m;
	pthread_cond_t cv;
	int count;
} notify_t;

static void on_wake(void *ctx) {
	notify_t *n = (notify_t *)ctx;
	pthread_mutex_lock(&n->m);
	n->count++;
	pthread_cond_broadcast(&n->cv);
	pthread_mutex_unlock(&n->m);
}

int main(void) {
	const uint8_t p1[] = "alpha";
	const uint8_t p2[] = "bravo-bravo";
	const size_t p1_len = sizeof(p1) - 1;
	const size_t p2_len = sizeof(p2) - 1;

	uint8_t wire[64];
	size_t n1 = 0, n2 = 0;
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_RELIABLE, p1, p1_len,
			wire, sizeof(wire), &n1) == WTD_FRAME_OK);
	EXPECT(wtd_frame_encode(WTD_FRAME_FLAG_UNRELIABLE, p2, p2_len,
			wire + n1, sizeof(wire) - n1, &n2) == WTD_FRAME_OK);
	const size_t total = n1 + n2;

	int fds[2] = { -1, -1 };
	EXPECT(pipe(fds) == 0);
	EXPECT(write(fds[1], wire, total) == (ssize_t)total);
	/* EOF after the two frames lets the reader exit on its own. */
	EXPECT(close(fds[1]) == 0);
	fds[1] = -1;

	fake_transport_t t = { 0 };
	wtd_transport_port_t tp = {
		.ctx = &t,
		.send_stream = fake_send_stream,
		.send_datagram = fake_send_datagram,
		.send_fin = fake_send_fin,
	};
	wtd_child_port_t cp = { .ctx = NULL, .write = noop_write, .close_input = noop_close };

	wtd_session_t s;
	wtd_session_init(&s, &tp, &cp);

	notify_t n;
	pthread_mutex_init(&n.m, NULL);
	pthread_cond_init(&n.cv, NULL);
	n.count = 0;

	wtd_child_output_t out;
	memset(&out, 0, sizeof(out));
	EXPECT(wtd_child_output_start(&out, fds[0], &s, on_wake, &n) == 0);

	/* Two frames + one EOF wake = 3 wakes. */
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	pthread_mutex_lock(&n.m);
	while (n.count < 3) {
		if (pthread_cond_timedwait(&n.cv, &n.m, &deadline) == ETIMEDOUT) {
			break;
		}
	}
	int got = n.count;
	pthread_mutex_unlock(&n.m);
	EXPECT(got >= 3);

	wtd_child_output_stop(&out);

	/* Now the core flushes the decoded frames to the transport port. */
	wtd_session_pump(&s);

	EXPECT(t.stream_len == p1_len);
	EXPECT(memcmp(t.stream, p1, p1_len) == 0);
	EXPECT(t.dgram_len == p2_len);
	EXPECT(memcmp(t.dgram, p2, p2_len) == 0);
	EXPECT(t.fin_calls == 1); /* EOF -> exactly one FIN */

	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 1); /* latched, not re-sent */

	wtd_session_destroy(&s);
	if (fds[0] >= 0) {
		close(fds[0]);
	}
	pthread_cond_destroy(&n.cv);
	pthread_mutex_destroy(&n.m);
	return failures == 0 ? 0 : 1;
}
#endif /* !_WIN32 */
