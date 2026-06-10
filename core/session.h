/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — session.h  (the hexagon core)
 *
 * One WebTransport session's application logic, expressed purely in
 * terms of ports — two driven ports the core calls outward (a transport
 * port toward the peer, a child port toward the spawned program) and two
 * driving entry points adapters call inward. The core owns no sockets,
 * no QUIC state, no process handles, no fds, and no threads. Everything
 * outside the hexagon is reached through a port, which makes the whole
 * byte-routing policy unit-testable with fakes (tests/unit/session_test.c).
 *
 * Four symmetric flows cross the hexagon:
 *
 *   peer  -> core  (driving, from the transport adapter)
 *       wtd_session_deliver_stream / _deliver_datagram / _deliver_fin
 *   core  -> child (driven, the child_port)
 *       child.write / child.close_input
 *
 *   child -> core  (driving, from the child_output adapter's reader thread)
 *       wtd_session_on_output / wtd_session_on_output_eof
 *   core  -> peer  (driven, the transport_port)
 *       transport.send_stream / send_datagram / send_fin
 *
 * Threading: deliver_* and pump run on the picoquic network thread. The
 * child_output adapter calls on_output / on_output_eof from its reader
 * thread; those touch only the (mutex-guarded) outbound queue and the
 * (atomic) output_eof flag. pump is the sole queue consumer and the only
 * caller of the ports — so the ports are always invoked on the network
 * thread.
 */

#ifndef WEBTRANSPORTD_SESSION_H
#define WEBTRANSPORTD_SESSION_H

#include "child_port.h"
#include "transport_port.h"
#include "work_queue.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_session {
	wtd_transport_port_t transport;
	wtd_child_port_t child;
	wtd_work_queue_t outbound;  /* child output frames awaiting flush */
	_Atomic int output_eof;     /* set once the child closes stdout */
	int child_input_closed;     /* set after the child port's input is closed */
	int stream_fin_sent;        /* set after the transport FIN is emitted */
} wtd_session_t;

/* Wire the core to its two driven ports. Spawns nothing. */
void wtd_session_init(wtd_session_t *s,
		const wtd_transport_port_t *transport,
		const wtd_child_port_t *child);

/* peer -> child: a reliable message arrived on the data stream. */
void wtd_session_deliver_stream(wtd_session_t *s,
		const uint8_t *data, size_t len);

/* peer -> child: an unreliable datagram arrived. */
void wtd_session_deliver_datagram(wtd_session_t *s,
		const uint8_t *data, size_t len);

/* peer -> child: the peer half-closed its stream; close child input once. */
void wtd_session_deliver_fin(wtd_session_t *s);

/* child -> core: the child_output adapter decoded one frame from the
 * child's stdout. Thread-safe; queues the frame for the next pump. */
void wtd_session_on_output(wtd_session_t *s, uint8_t flag,
		const uint8_t *data, size_t len);

/* child -> core: the child closed stdout (EOF). Thread-safe; the next
 * pump that finds the queue empty will emit a transport FIN. */
void wtd_session_on_output_eof(wtd_session_t *s);

/* Flush queued child output to the transport port, and emit a stream FIN
 * once the child has closed stdout. Idempotent; call whenever the
 * transport can accept data or the reader signals. Network thread only. */
void wtd_session_pump(wtd_session_t *s);

/* Free the outbound queue. The caller must already have stopped the
 * child_output reader (so no more on_output calls arrive). */
void wtd_session_destroy(wtd_session_t *s);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_SESSION_H */
