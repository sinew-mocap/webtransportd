/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — child_output.h
 *
 * Driving (primary) adapter for the child's stdout edge. It owns a
 * reader thread that read()s the child's stdout fd, decodes each
 * complete frame with the domain codec, and drives the session core
 * through wtd_session_on_output / wtd_session_on_output_eof. After each
 * delivery it fires a wake callback so the network thread runs
 * wtd_session_pump.
 *
 * This is the symmetric counterpart of the child_port: child input
 * leaves the core through the (driven) child_port; child output enters
 * the core through this (driving) adapter. Neither the fd, the thread,
 * nor read() appear inside the hexagon.
 *
 * fd ownership stays with the caller. To shut the thread down the caller
 * must cause EOF on the fd (typically by terminating the child), then
 * call wtd_child_output_stop to join.
 */

#ifndef WEBTRANSPORTD_CHILD_OUTPUT_H
#define WEBTRANSPORTD_CHILD_OUTPUT_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wtd_session; /* forward decl; full type in session.h */

/* Fired (with its ctx) after a frame is delivered or EOF is reached, so
 * the network thread can be woken to pump. May be NULL. */
typedef void (*wtd_wake_fn)(void *ctx);

typedef struct wtd_child_output {
	pthread_t thread;
	int thread_started;
	int fd;
	struct wtd_session *session;
	wtd_wake_fn wake;
	void *wake_ctx;
} wtd_child_output_t;

/* Spawn the reader thread on `fd`, delivering frames into `session`.
 * Returns 0 on success, -errno on failure. */
int wtd_child_output_start(wtd_child_output_t *o, int fd,
		struct wtd_session *session, wtd_wake_fn wake, void *wake_ctx);

/* Join the reader thread. The caller must already have caused EOF on the
 * fd so the read() returns. Safe to call if never started. */
void wtd_child_output_stop(wtd_child_output_t *o);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_CHILD_OUTPUT_H */
