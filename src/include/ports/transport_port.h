/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — ports/transport_port.h
 *
 * Driven (secondary) port: the outbound edge of the hexagon toward the
 * peer. The session core calls through this interface to push child
 * output back to the WebTransport peer; it never names picoquic, h3zero,
 * or stream/datagram ids. The picoquic adapter
 * (adapters/transport_picoquic.c) supplies an implementation.
 *
 * Contract:
 *   send_stream   — deliver `len` reliable bytes onto the session's bidi
 *                   data stream. Returns 0 on success, non-zero if the
 *                   stream is not yet bound (the core may retry later).
 *   send_datagram — deliver `len` bytes as one unreliable datagram.
 *                   Returns 0 if queued for transmission.
 *   send_fin      — half-close the data stream (no more child output).
 *                   Returns 0 once the FIN is actually emitted; non-zero
 *                   if the stream is not bound yet, so the core retries.
 *
 * All callbacks run on the picoquic network thread.
 */

#ifndef WEBTRANSPORTD_PORTS_TRANSPORT_PORT_H
#define WEBTRANSPORTD_PORTS_TRANSPORT_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_transport_port {
	void *ctx;
	int (*send_stream)(void *ctx, const uint8_t *data, size_t len);
	int (*send_datagram)(void *ctx, const uint8_t *data, size_t len);
	int (*send_fin)(void *ctx);
} wtd_transport_port_t;

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_PORTS_TRANSPORT_PORT_H */
