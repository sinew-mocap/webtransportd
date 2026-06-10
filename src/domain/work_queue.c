/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — work_queue.c
 *
 * Mutex-guarded FIFO of outbound frames. Pure domain primitive: no I/O,
 * no threads of its own.
 */

#include "work_queue.h"

#include <stdlib.h>

void wtd_work_queue_init(wtd_work_queue_t *q) {
	pthread_mutex_init(&q->mutex, NULL);
	q->head = NULL;
	q->tail = NULL;
}

void wtd_work_queue_destroy(wtd_work_queue_t *q) {
	wtd_outbound_frame_t *cur = wtd_work_queue_drain(q);
	while (cur != NULL) {
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
	}
	pthread_mutex_destroy(&q->mutex);
}

void wtd_work_queue_push(wtd_work_queue_t *q, wtd_outbound_frame_t *f) {
	if (f == NULL) {
		return;
	}
	pthread_mutex_lock(&q->mutex);
	f->next = NULL;
	if (q->tail != NULL) {
		q->tail->next = f;
	} else {
		q->head = f;
	}
	q->tail = f;
	pthread_mutex_unlock(&q->mutex);
}

wtd_outbound_frame_t *wtd_work_queue_drain(wtd_work_queue_t *q) {
	pthread_mutex_lock(&q->mutex);
	wtd_outbound_frame_t *h = q->head;
	q->head = NULL;
	q->tail = NULL;
	pthread_mutex_unlock(&q->mutex);
	return h;
}
