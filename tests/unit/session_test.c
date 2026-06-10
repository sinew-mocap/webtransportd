/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — session_test.c
 *
 * Exercises the hexagon core (domain/session.c) entirely through its
 * ports. There is no picoquic, no child process, no socket, and no
 * reader thread here — the test drives the core's two inbound surfaces
 * (deliver_* and on_output) and inspects what the core hands to the fake
 * transport and child ports. That it can be written at all is the point
 * of ports and adapters: the byte-routing policy is testable in isolation.
 */

#include "session.h"

#include "frame.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

/* ---- fake child port: capture everything written to "child stdin" ---- */

typedef struct fake_child {
	uint8_t buf[4096];
	size_t len;
	int close_calls;
} fake_child_t;

static long fake_child_write(void *ctx, const uint8_t *data, size_t len) {
	fake_child_t *c = (fake_child_t *)ctx;
	if (c->len + len > sizeof(c->buf)) {
		return -1;
	}
	memcpy(c->buf + c->len, data, len);
	c->len += len;
	return (long)len;
}

static int fake_child_close(void *ctx) {
	((fake_child_t *)ctx)->close_calls++;
	return 0;
}

/* ---- fake transport port: capture peer-bound sends ---- */

typedef struct fake_transport {
	uint8_t stream[4096];
	size_t stream_len;
	uint8_t last_dgram[4096];
	size_t last_dgram_len;
	int dgram_calls;
	int fin_calls;
	int fin_return; /* what send_fin reports (0 = bound/sent) */
} fake_transport_t;

static int fake_send_stream(void *ctx, const uint8_t *data, size_t len) {
	fake_transport_t *t = (fake_transport_t *)ctx;
	memcpy(t->stream + t->stream_len, data, len);
	t->stream_len += len;
	return 0;
}

static int fake_send_datagram(void *ctx, const uint8_t *data, size_t len) {
	fake_transport_t *t = (fake_transport_t *)ctx;
	memcpy(t->last_dgram, data, len);
	t->last_dgram_len = len;
	t->dgram_calls++;
	return 0;
}

static int fake_send_fin(void *ctx) {
	fake_transport_t *t = (fake_transport_t *)ctx;
	t->fin_calls++;
	return t->fin_return;
}

static void wire(wtd_session_t *s, fake_transport_t *t, fake_child_t *c) {
	wtd_transport_port_t tp = {
		.ctx = t,
		.send_stream = fake_send_stream,
		.send_datagram = fake_send_datagram,
		.send_fin = fake_send_fin,
	};
	wtd_child_port_t cp = {
		.ctx = c,
		.write = fake_child_write,
		.close_input = fake_child_close,
	};
	wtd_session_init(s, &tp, &cp);
}

/* Drive the child-output port exactly as the child_output adapter's
 * reader thread would, so pump() has something to flush. */
static void enqueue(wtd_session_t *s, uint8_t flag, const char *payload) {
	wtd_session_on_output(s, flag, (const uint8_t *)payload, strlen(payload));
}

/* peer -> child: a reliable message is framed flag=0 onto child stdin. */
static void peer_to_child_frames_reliable(void) {
	fake_transport_t t = { 0 };
	fake_child_t c = { 0 };
	wtd_session_t s;
	wire(&s, &t, &c);

	const char *msg = "hello";
	wtd_session_deliver_stream(&s, (const uint8_t *)msg, strlen(msg));

	/* Decode what landed on the fake child's stdin. */
	size_t consumed = 0, plen = 0;
	uint8_t flag = 0xFF;
	const uint8_t *payload = NULL;
	EXPECT(wtd_frame_decode(c.buf, c.len, &consumed, &flag, &payload, &plen)
			== WTD_FRAME_OK);
	EXPECT(flag == WTD_FRAME_FLAG_RELIABLE);
	EXPECT(plen == strlen(msg));
	EXPECT(payload != NULL && memcmp(payload, msg, plen) == 0);

	wtd_session_destroy(&s);
}

/* peer -> child: a datagram is framed flag=1; FIN closes child input once. */
static void peer_to_child_datagram_and_fin(void) {
	fake_transport_t t = { 0 };
	fake_child_t c = { 0 };
	wtd_session_t s;
	wire(&s, &t, &c);

	const char *msg = "ping";
	wtd_session_deliver_datagram(&s, (const uint8_t *)msg, strlen(msg));

	size_t consumed = 0, plen = 0;
	uint8_t flag = 0xFF;
	const uint8_t *payload = NULL;
	EXPECT(wtd_frame_decode(c.buf, c.len, &consumed, &flag, &payload, &plen)
			== WTD_FRAME_OK);
	EXPECT(flag == WTD_FRAME_FLAG_UNRELIABLE);
	EXPECT(plen == strlen(msg) && memcmp(payload, msg, plen) == 0);

	/* FIN closes the child once; a second FIN is a no-op. */
	wtd_session_deliver_fin(&s);
	wtd_session_deliver_fin(&s);
	EXPECT(c.close_calls == 1);

	/* After close, further delivery is dropped (no bytes past the frame). */
	size_t before = c.len;
	wtd_session_deliver_stream(&s, (const uint8_t *)"x", 1);
	EXPECT(c.len == before);

	wtd_session_destroy(&s);
}

/* child -> peer: pump routes reliable frames to the stream, unreliable to
 * a datagram, in queue order. */
static void child_to_peer_routes_by_flag(void) {
	fake_transport_t t = { 0 };
	fake_child_t c = { 0 };
	wtd_session_t s;
	wire(&s, &t, &c);

	enqueue(&s, WTD_FRAME_FLAG_RELIABLE, "AB");
	enqueue(&s, WTD_FRAME_FLAG_UNRELIABLE, "DG");
	enqueue(&s, WTD_FRAME_FLAG_RELIABLE, "CD");

	wtd_session_pump(&s);

	EXPECT(t.stream_len == 4);
	EXPECT(memcmp(t.stream, "ABCD", 4) == 0); /* reliable concatenated */
	EXPECT(t.dgram_calls == 1);
	EXPECT(t.last_dgram_len == 2 && memcmp(t.last_dgram, "DG", 2) == 0);

	wtd_session_destroy(&s);
}

/* child EOF -> FIN, emitted exactly once and only after reader_done. */
static void pump_emits_fin_once_when_reader_done(void) {
	fake_transport_t t = { 0 };
	fake_child_t c = { 0 };
	wtd_session_t s;
	wire(&s, &t, &c);

	/* Reader still running: pump must not emit a FIN. */
	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 0);

	/* Reader hits EOF. */
	wtd_session_on_output_eof(&s);
	wtd_session_pump(&s);
	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 1); /* sent once, then latched */

	wtd_session_destroy(&s);
}

/* If the transport can't send the FIN yet (stream unbound), the core
 * retries on the next pump rather than dropping it. */
static void pump_retries_fin_until_accepted(void) {
	fake_transport_t t = { 0 };
	fake_child_t c = { 0 };
	wtd_session_t s;
	wire(&s, &t, &c);

	t.fin_return = -1; /* transport reports "not bound, retry" */
	wtd_session_on_output_eof(&s);
	wtd_session_pump(&s);
	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 2); /* retried, not latched */

	t.fin_return = 0; /* now the stream is bound */
	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 3);
	wtd_session_pump(&s);
	EXPECT(t.fin_calls == 3); /* latched after success */

	wtd_session_destroy(&s);
}

int main(void) {
	peer_to_child_frames_reliable();
	peer_to_child_datagram_and_fin();
	child_to_peer_routes_by_flag();
	pump_emits_fin_once_when_reader_done();
	pump_retries_fin_until_accepted();
	return failures == 0 ? 0 : 1;
}
