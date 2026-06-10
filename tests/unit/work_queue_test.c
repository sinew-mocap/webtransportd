/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — work_queue_test.c
 *
 * The outbound FIFO primitive: three pushes then one drain must return
 * all three frames in FIFO order with payloads + flags intact, and a
 * second drain of the now-empty queue must return NULL.
 */

#include "work_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static wtd_outbound_frame_t *make_frame(uint8_t flag, const char *s) {
	size_t n = strlen(s);
	wtd_outbound_frame_t *f = (wtd_outbound_frame_t *)malloc(
			sizeof(wtd_outbound_frame_t) + n);
	f->next = NULL;
	f->flag = flag;
	f->payload_len = n;
	memcpy(f->payload, s, n);
	return f;
}

int main(void) {
	wtd_work_queue_t q;
	wtd_work_queue_init(&q);

	wtd_work_queue_push(&q, make_frame(0, "first"));
	wtd_work_queue_push(&q, make_frame(1, "second"));
	wtd_work_queue_push(&q, make_frame(0, "third"));

	wtd_outbound_frame_t *head = wtd_work_queue_drain(&q);
	EXPECT(head != NULL);

	const char *expected[] = { "first", "second", "third" };
	uint8_t expected_flag[] = { 0, 1, 0 };
	int i = 0;
	wtd_outbound_frame_t *cur = head;
	while (cur != NULL) {
		EXPECT(i < 3);
		if (i < 3) {
			size_t want = strlen(expected[i]);
			EXPECT(cur->payload_len == want);
			EXPECT(cur->flag == expected_flag[i]);
			EXPECT(memcmp(cur->payload, expected[i], want) == 0);
		}
		wtd_outbound_frame_t *next = cur->next;
		free(cur);
		cur = next;
		i++;
	}
	EXPECT(i == 3);

	/* Draining an empty queue returns NULL. */
	EXPECT(wtd_work_queue_drain(&q) == NULL);

	wtd_work_queue_destroy(&q);
	return failures == 0 ? 0 : 1;
}
