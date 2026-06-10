/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — work_queue.h
 *
 * A tiny mutex-guarded FIFO of outbound frames: the hand-off point
 * inside the hexagon between the child-output driving adapter (which
 * pushes decoded child frames from its reader thread) and the network
 * thread (which drains and flushes them to the transport port via
 * wtd_session_pump). The queue is domain state — it owns no fds and
 * spawns no threads; the mutex only guards the list against the two
 * threads that touch it.
 */

#ifndef WEBTRANSPORTD_WORK_QUEUE_H
#define WEBTRANSPORTD_WORK_QUEUE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_outbound_frame {
	struct wtd_outbound_frame *next;
	uint8_t flag;
	size_t payload_len;
	uint8_t payload[]; /* flexible array member */
} wtd_outbound_frame_t;

typedef struct wtd_work_queue {
	pthread_mutex_t mutex;
	wtd_outbound_frame_t *head;
	wtd_outbound_frame_t *tail;
} wtd_work_queue_t;

void wtd_work_queue_init(wtd_work_queue_t *q);
void wtd_work_queue_destroy(wtd_work_queue_t *q);

/* Takes ownership of f. Thread-safe. */
void wtd_work_queue_push(wtd_work_queue_t *q, wtd_outbound_frame_t *f);

/* Atomically detach the whole list; caller owns every node and must
 * free() each one. Returns NULL if empty. */
wtd_outbound_frame_t *wtd_work_queue_drain(wtd_work_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_WORK_QUEUE_H */
