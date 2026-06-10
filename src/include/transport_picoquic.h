/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — transport_picoquic.h
 *
 * Driving (primary) adapter: the picoquic + h3zero WebTransport server.
 * It owns all QUIC/HTTP3 state, translates picohttp callback events into
 * calls on the session core (the hexagon), and implements the transport
 * port the core sends peer output through. The composition root
 * (app/main.c) hands it a config and a stop flag and lets it run the
 * packet loop.
 */

#ifndef WEBTRANSPORTD_TRANSPORT_PICOQUIC_H
#define WEBTRANSPORTD_TRANSPORT_PICOQUIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wtd_transport_config {
	const char *cert;      /* PEM path, or "auto" for a self-signed cert */
	const char *key;       /* PEM key path (ignored when cert == "auto") */
	uint16_t port;         /* UDP port to listen on */
	const char *exec_path; /* child program spawned per session */
	const char *dir_path;  /* static file root for non-WT GET, or NULL */
} wtd_transport_config_t;

/* Install SIGTERM/SIGINT handlers, stand up the HTTP/3 WebTransport
 * server, and run the packet loop until a signal arrives. Returns 0 on a
 * clean shutdown, 1 on a fatal setup or loop error. */
int wtd_transport_run(const wtd_transport_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* WEBTRANSPORTD_TRANSPORT_PICOQUIC_H */
