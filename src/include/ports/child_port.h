/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — ports/child_port.h
 *
 * Driven (secondary) port: the outbound edge of the hexagon toward the
 * child program's input. The session core calls through this interface
 * to forward peer messages to the child's stdin; it never touches file
 * descriptors, pipes, or process handles. The process adapter
 * (adapters/child_process.c, wrapped in transport_picoquic.c) supplies
 * an implementation.
 *
 * The reverse direction — child stdout back to the peer — is a *driving*
 * edge handled by the session's reader thread (see session.h), not by
 * this port.
 *
 * Contract:
 *   write       — hand `len` raw framed bytes to the child's input.
 *                 Returns the number of bytes accepted, or < 0 on error.
 *                 (Return type is `long`, not `ssize_t`: the picoquic
 *                 vendor headers redefine `ssize_t` on Windows, which
 *                 would clash with this port's signature.)
 *   close_input — half-close the child's input (EOF on its stdin). Safe
 *                 to call at most once; the core guards against re-close.
 */

#ifndef WEBTRANSPORTD_PORTS_CHILD_PORT_H
#define WEBTRANSPORTD_PORTS_CHILD_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_child_port {
	void *ctx;
	long (*write)(void *ctx, const uint8_t *data, size_t len);
	int (*close_input)(void *ctx);
} wtd_child_port_t;

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_PORTS_CHILD_PORT_H */
