/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — session.c  (the hexagon core)
 *
 * Pure byte-routing policy for one WebTransport session. Depends only on
 * the frame codec, the work queue, and the four port surfaces. No
 * picoquic, no h3zero, no process/socket/thread headers, no fds — the
 * whole point of the hexagon.
 */

#include "session.h"

#include "frame.h"
#include "log.h"
#include "work_queue.h"

#include <stdlib.h>
#include <string.h>

void wtd_session_init(wtd_session_t *s,
		const wtd_transport_port_t *transport,
		const wtd_child_port_t *child) {
	memset(s, 0, sizeof(*s));
	s->transport = *transport;
	s->child = *child;
	wtd_work_queue_init(&s->outbound);
	atomic_store(&s->output_eof, 0);
}

/* peer -> child. Frame one peer message and hand it to the child port.
 * flag picks the reliability bit; the child re-derives the delivery mode
 * from it. */
static void deliver_to_child(wtd_session_t *s, uint8_t flag,
		const uint8_t *data, size_t len) {
	if (s->child_input_closed || len == 0) {
		return;
	}
	size_t cap = len + 9; /* flag byte + up to an 8-byte varint */
	uint8_t *frame = (uint8_t *)malloc(cap);
	if (frame == NULL) {
		wtd_log(WTD_LOG_ERROR, "session: frame alloc failed");
		return;
	}
	size_t frame_len = 0;
	wtd_frame_status_t st = wtd_frame_encode(flag, data, len,
			frame, cap, &frame_len);
	if (st == WTD_FRAME_OK) {
		long n = s->child.write(s->child.ctx, frame, frame_len);
		wtd_log(WTD_LOG_TRACE,
				"session: wrote %lu framed bytes to child (ret=%ld)",
				(unsigned long)frame_len, n);
	} else {
		wtd_log(WTD_LOG_ERROR, "session: frame encode failed: %d", st);
	}
	free(frame);
}

void wtd_session_deliver_stream(wtd_session_t *s,
		const uint8_t *data, size_t len) {
	deliver_to_child(s, WTD_FRAME_FLAG_RELIABLE, data, len);
}

void wtd_session_deliver_datagram(wtd_session_t *s,
		const uint8_t *data, size_t len) {
	deliver_to_child(s, WTD_FRAME_FLAG_UNRELIABLE, data, len);
}

void wtd_session_deliver_fin(wtd_session_t *s) {
	if (s->child_input_closed) {
		return;
	}
	s->child.close_input(s->child.ctx);
	s->child_input_closed = 1;
	wtd_log(WTD_LOG_TRACE, "session: closed child input on peer FIN");
}

/* child -> core. Queue a decoded child frame for the next pump. Called
 * from the child_output reader thread, so it touches only the
 * mutex-guarded queue. */
void wtd_session_on_output(wtd_session_t *s, uint8_t flag,
		const uint8_t *data, size_t len) {
	wtd_outbound_frame_t *f = (wtd_outbound_frame_t *)malloc(
			sizeof(wtd_outbound_frame_t) + len);
	if (f == NULL) {
		wtd_log(WTD_LOG_ERROR, "session: output frame alloc failed");
		return;
	}
	f->next = NULL;
	f->flag = flag;
	f->payload_len = len;
	if (len > 0) {
		memcpy(f->payload, data, len);
	}
	wtd_work_queue_push(&s->outbound, f);
}

void wtd_session_on_output_eof(wtd_session_t *s) {
	atomic_store(&s->output_eof, 1);
}

void wtd_session_pump(wtd_session_t *s) {
	wtd_outbound_frame_t *frame = wtd_work_queue_drain(&s->outbound);

	while (frame != NULL) {
		wtd_outbound_frame_t *next = frame->next;
		if (frame->flag == WTD_FRAME_FLAG_RELIABLE) {
			(void)s->transport.send_stream(s->transport.ctx,
					frame->payload, frame->payload_len);
		} else {
			(void)s->transport.send_datagram(s->transport.ctx,
					frame->payload, frame->payload_len);
		}
		free(frame);
		frame = next;
	}

	if (atomic_load(&s->output_eof) && !s->stream_fin_sent) {
		if (s->transport.send_fin(s->transport.ctx) == 0) {
			s->stream_fin_sent = 1;
			wtd_log(WTD_LOG_TRACE, "session: sent stream FIN");
		}
	}
}

void wtd_session_destroy(wtd_session_t *s) {
	wtd_work_queue_destroy(&s->outbound);
}
