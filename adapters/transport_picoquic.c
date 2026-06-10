/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — transport_picoquic.c
 *
 * The picoquic + h3zero WebTransport adapter. Everything QUIC-shaped
 * lives here: the per-peer transport state, the h3zero callback that
 * turns picohttp events into session-core calls, the transport-port
 * implementation the core flushes child output through, and the packet
 * loop bootstrap (including autocert installation).
 *
 * The hexagon core (session.c) never sees any of these symbols; it only
 * sees the two ports this file fills in for each accepted session.
 */

#include "transport_picoquic.h"

#include "autocert.h"
#include "child_output.h"
#include "child_process.h"
#include "log.h"
#include "session.h"

#include "picotls.h"
#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "h3zero.h"
#include "h3zero_common.h"
#include "pico_webtransport.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif
#include <unistd.h>

static atomic_int g_should_exit = 0;

static const char *picohttp_event_name(picohttp_call_back_event_t event) {
	switch (event) {
	case picohttp_callback_get: return "GET";
	case picohttp_callback_post: return "POST";
	case picohttp_callback_connecting: return "connecting";
	case picohttp_callback_connect: return "CONNECT";
	case picohttp_callback_connect_refused: return "CONNECT_refused";
	case picohttp_callback_connect_accepted: return "CONNECT_accepted";
	case picohttp_callback_post_data: return "post_data";
	case picohttp_callback_post_fin: return "post_fin";
	case picohttp_callback_provide_data: return "provide_data";
	case picohttp_callback_post_datagram: return "post_datagram";
	case picohttp_callback_provide_datagram: return "provide_datagram";
	case picohttp_callback_reset: return "reset";
	case picohttp_callback_stop_sending: return "stop_sending";
	case picohttp_callback_deregister: return "deregister";
	case picohttp_callback_free: return "free";
	default: return "?";
	}
}

typedef struct server_ctx server_ctx_t;

/* Per-peer transport state. The session core lives inside it but reaches
 * back out only through the two ports wired in peer_create. */
typedef struct wtd_peer {
	picoquic_cnx_t *cnx;
	wtd_child_t child;
	wtd_session_t session;
	wtd_child_output_t output; /* reader thread feeding session->on_output */
	struct wtd_peer *next;
	uint64_t control_stream_id;
	uint64_t data_stream_id;
	h3zero_callback_ctx_t *h3_ctx;
	server_ctx_t *sctx;
	uint8_t *pending_dgram;
	size_t pending_dgram_len;
} wtd_peer_t;

typedef struct server_ctx {
	const char *exec_path;
	const char *dir_path;
	wtd_peer_t *peers;
	picohttp_server_path_item_t *path_items;
	picoquic_network_thread_ctx_t *net_thread_ctx;
} server_ctx_t;

/* ----------------------------------------------------------------------
 * Driven port: transport (core -> peer)
 * -------------------------------------------------------------------- */

static int peer_send_stream(void *ctx, const uint8_t *data, size_t len) {
	wtd_peer_t *p = (wtd_peer_t *)ctx;
	if (p->data_stream_id == UINT64_MAX) {
		wtd_log(WTD_LOG_WARN, "transport: data stream not bound for send");
		return -1;
	}
	int rc = picoquic_add_to_stream(p->cnx, p->data_stream_id,
			data, len, 0);
	wtd_log(WTD_LOG_TRACE,
			"transport: stream send len=%lu (picoquic_add=%d)",
			(unsigned long)len, rc);
	return 0;
}

static int peer_send_datagram(void *ctx, const uint8_t *data, size_t len) {
	wtd_peer_t *p = (wtd_peer_t *)ctx;
	if (p->pending_dgram != NULL) {
		free(p->pending_dgram);
	}
	p->pending_dgram = (uint8_t *)malloc(len);
	if (p->pending_dgram == NULL) {
		p->pending_dgram_len = 0;
		return -1;
	}
	memcpy(p->pending_dgram, data, len);
	p->pending_dgram_len = len;
	h3zero_set_datagram_ready(p->cnx, p->control_stream_id);
	wtd_log(WTD_LOG_TRACE, "transport: datagram queued len=%lu",
			(unsigned long)len);
	return 0;
}

static int peer_send_fin(void *ctx) {
	wtd_peer_t *p = (wtd_peer_t *)ctx;
	if (p->data_stream_id == UINT64_MAX) {
		return -1; /* not bound yet; core retries on the next pump */
	}
	picoquic_add_to_stream(p->cnx, p->data_stream_id, NULL, 0, 1);
	wtd_log(WTD_LOG_TRACE, "transport: sent FIN on data stream %" PRIu64,
			p->data_stream_id);
	return 0;
}

/* ----------------------------------------------------------------------
 * Driven port: child input (core -> child stdin)
 * -------------------------------------------------------------------- */

static long peer_child_write(void *ctx, const uint8_t *data, size_t len) {
	wtd_peer_t *p = (wtd_peer_t *)ctx;
	if (p->child.stdin_fd < 0) {
		return -1;
	}
	return (long)write(p->child.stdin_fd, data, len);
}

static int peer_child_close(void *ctx) {
	wtd_peer_t *p = (wtd_peer_t *)ctx;
	if (p->child.stdin_fd >= 0) {
		close(p->child.stdin_fd);
		p->child.stdin_fd = -1;
	}
	return 0;
}

/* ----------------------------------------------------------------------
 * Peer lifecycle
 * -------------------------------------------------------------------- */

static void on_reader_ready(void *ctx) {
	picoquic_wake_up_network_thread((picoquic_network_thread_ctx_t *)ctx);
}

static void on_sigterm(int sig) {
	(void)sig;
	atomic_store(&g_should_exit, 1);
}

static wtd_peer_t *peer_create(server_ctx_t *sctx, picoquic_cnx_t *cnx,
		h3zero_callback_ctx_t *h3_ctx) {
	wtd_peer_t *p = (wtd_peer_t *)malloc(sizeof(wtd_peer_t));
	if (p == NULL) {
		return NULL;
	}
	memset(p, 0, sizeof(wtd_peer_t));
	p->cnx = cnx;
	p->h3_ctx = h3_ctx;
	p->sctx = sctx;
	p->control_stream_id = UINT64_MAX;
	p->data_stream_id = UINT64_MAX;

	const char *argv[] = { sctx->exec_path, NULL };
	if (wtd_child_spawn(argv, NULL, &p->child) != 0) {
		free(p);
		return NULL;
	}

	wtd_transport_port_t transport = {
		.ctx = p,
		.send_stream = peer_send_stream,
		.send_datagram = peer_send_datagram,
		.send_fin = peer_send_fin,
	};
	wtd_child_port_t child = {
		.ctx = p,
		.write = peer_child_write,
		.close_input = peer_child_close,
	};
	wtd_session_init(&p->session, &transport, &child);

	if (wtd_child_output_start(&p->output, p->child.stdout_fd, &p->session,
			sctx->net_thread_ctx ? on_reader_ready : NULL,
			sctx->net_thread_ctx) != 0) {
		wtd_log(WTD_LOG_ERROR, "child_output_start failed");
		wtd_child_terminate(&p->child);
		wtd_session_destroy(&p->session);
		free(p);
		return NULL;
	}

	p->next = sctx->peers;
	sctx->peers = p;
	return p;
}

static void peer_remove(server_ctx_t *sctx, wtd_peer_t *p) {
	if (p == NULL) {
		return;
	}
	wtd_peer_t **pp = &sctx->peers;
	while (*pp != NULL && *pp != p) {
		pp = &(*pp)->next;
	}
	if (*pp == p) {
		*pp = p->next;
	}
	/* Join the reader (it exits on child-stdout EOF) before tearing down
	 * the session it delivers into, then reap the child. */
	wtd_child_output_stop(&p->output);
	wtd_session_destroy(&p->session);
	wtd_child_terminate(&p->child);
	if (p->pending_dgram != NULL) {
		free(p->pending_dgram);
	}
	free(p);
}

/* ----------------------------------------------------------------------
 * Driving adapter: picohttp events -> session core
 * -------------------------------------------------------------------- */

static int wt_session_cb(picoquic_cnx_t *cnx, uint8_t *bytes, size_t length,
		picohttp_call_back_event_t event,
		h3zero_stream_ctx_t *stream_ctx, void *path_app_ctx) {
	if (event == picohttp_callback_connect) {
		server_ctx_t *sctx = (server_ctx_t *)path_app_ctx;
		h3zero_callback_ctx_t *h3_ctx =
				(h3zero_callback_ctx_t *)picoquic_get_callback_context(cnx);
		wtd_log(WTD_LOG_TRACE,
				"[WebTransport] CONNECT received on stream %" PRIu64,
				stream_ctx->stream_id);
		wtd_peer_t *p = peer_create(sctx, cnx, h3_ctx);
		if (p == NULL) {
			wtd_log(WTD_LOG_ERROR, "peer_create failed");
			return -1;
		}
		p->control_stream_id = stream_ctx->stream_id;
		stream_ctx->path_callback_ctx = p;
		if (h3zero_declare_stream_prefix(h3_ctx,
				stream_ctx->stream_id, wt_session_cb, p) != 0) {
			wtd_log(WTD_LOG_ERROR, "h3zero_declare_stream_prefix failed");
			peer_remove(sctx, p);
			return -1;
		}
		wtd_log(WTD_LOG_TRACE,
				"[WebTransport] CONNECT accepted on stream %" PRIu64,
				p->control_stream_id);
		return 0;
	}

	wtd_peer_t *p = (wtd_peer_t *)path_app_ctx;
	if (p != NULL) {
		wtd_log(WTD_LOG_TRACE,
				"wt_session_cb: event=%s stream=%" PRIu64 " length=%lu",
				picohttp_event_name(event), stream_ctx->stream_id,
				(unsigned long)length);
	}

	switch (event) {
	case picohttp_callback_post_data:
	case picohttp_callback_post_fin:
		if (p == NULL) {
			break;
		}
		if (stream_ctx->stream_id == p->control_stream_id) {
			wtd_log(WTD_LOG_TRACE,
					" post_data on control stream (capsule), skipping");
			break;
		}
		if (p->data_stream_id == UINT64_MAX) {
			p->data_stream_id = stream_ctx->stream_id;
			wtd_log(WTD_LOG_TRACE, " data_stream_id set to %" PRIu64,
					p->data_stream_id);
		}
		if (length > 0) {
			wtd_session_deliver_stream(&p->session, bytes, length);
		}
		if (event == picohttp_callback_post_fin) {
			wtd_session_deliver_fin(&p->session);
		}
		break;

	case picohttp_callback_post_datagram:
		if (p != NULL && length > 0) {
			wtd_session_deliver_datagram(&p->session, bytes, length);
		}
		break;

	case picohttp_callback_provide_data:
		if (p != NULL) {
			wtd_session_pump(&p->session);
		}
		break;

	case picohttp_callback_provide_datagram:
		if (p != NULL && p->pending_dgram != NULL) {
			uint8_t *buf = h3zero_provide_datagram_buffer(
					(void *)bytes, p->pending_dgram_len, 0);
			if (buf != NULL) {
				memcpy(buf, p->pending_dgram, p->pending_dgram_len);
			}
			free(p->pending_dgram);
			p->pending_dgram = NULL;
			p->pending_dgram_len = 0;
		}
		break;

	case picohttp_callback_deregister:
		if (p != NULL) {
			wtd_log(WTD_LOG_TRACE, "[WebTransport] session closed");
			peer_remove(p->sctx, p);
		}
		break;

	default:
		wtd_log(WTD_LOG_TRACE, "unhandled callback event: %s",
				picohttp_event_name(event));
		break;
	}

	return 0;
}

/* Packet loop callback: pump every peer so child output drains, and
 * honor the stop flag. */
static int server_loop_cb(picoquic_quic_t *quic,
		picoquic_packet_loop_cb_enum cb, void *cb_ctx, void *cb_arg) {
	(void)quic;
	(void)cb_arg;

	if (cb == picoquic_packet_loop_ready) {
		printf("server ready\n");
		fflush(stdout);
		wtd_log(WTD_LOG_TRACE, "packet loop ready");
		return 0;
	}

	if (atomic_load(&g_should_exit)) {
		return PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP;
	}

	server_ctx_t *sctx = (server_ctx_t *)cb_ctx;
	for (wtd_peer_t *p = sctx->peers; p != NULL; p = p->next) {
		wtd_session_pump(&p->session);
	}
	return 0;
}

/* ----------------------------------------------------------------------
 * Bootstrap
 * -------------------------------------------------------------------- */

int wtd_transport_run(const wtd_transport_config_t *cfg) {
#ifdef _WIN32
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
#else
	struct sigaction sa = { 0 };
	sa.sa_handler = on_sigterm;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
#endif

	picohttp_server_path_item_t path_items[1] = {
		{ "/wt", 3, wt_session_cb, NULL }
	};

	picohttp_server_parameters_t server_param = { 0 };
	server_param.web_folder = cfg->dir_path;
	server_param.path_table = path_items;
	server_param.path_table_nb = 1;

	server_ctx_t sctx = { cfg->exec_path, cfg->dir_path, NULL, path_items, NULL };
	path_items[0].path_app_ctx = &sctx;

	int use_autocert = (cfg->cert != NULL && strcmp(cfg->cert, "auto") == 0);
	uint8_t *cert_der = NULL;
	uint8_t *key_der = NULL;
	size_t cert_der_len = 0;
	size_t key_der_len = 0;

	if (use_autocert) {
		if (wtd_autocert_generate(&cert_der, &cert_der_len,
				&key_der, &key_der_len) != 0) {
			wtd_log(WTD_LOG_ERROR, "webtransportd: --cert=auto failed");
			return 1;
		}
		/* Print base64-encoded SHA-256 of DER cert so callers can use
		 * serverCertificateHashes to bypass CA trust. */
		unsigned char hash[32];
		mbedtls_sha256(cert_der, cert_der_len, hash, 0);
		unsigned char b64[48];
		size_t b64_len = 0;
		mbedtls_base64_encode(b64, sizeof(b64), &b64_len, hash, sizeof(hash));
		printf("cert-hash: %.*s\n", (int)b64_len, b64);
		fflush(stdout);
	}

	uint8_t reset_seed[PICOQUIC_RESET_SECRET_SIZE] = { 0 };

	picoquic_quic_t *quic = picoquic_create(
			8,
			use_autocert ? NULL : cfg->cert,
			use_autocert ? NULL : cfg->key,
			NULL, "h3",
			h3zero_callback, &server_param, NULL, NULL, reset_seed,
			picoquic_current_time(), NULL, NULL, NULL, 0);
	if (quic == NULL) {
		wtd_log(WTD_LOG_ERROR, "webtransportd: picoquic_create failed");
		free(cert_der);
		free(key_der);
		return 1;
	}

	wtd_log(WTD_LOG_INFO, "webtransportd: HTTP/3 server on port %u",
			cfg->port);

	picowt_set_default_transport_parameters(quic);
	(void)picoquic_set_default_tp_value(quic,
			picoquic_tp_max_datagram_frame_size, 1500);

	if (use_autocert) {
		/* picoquic_create() sets enforce_client_only=1 when cert_file_name
		 * is NULL; clear it so the server accepts incoming connections. */
		picoquic_enforce_client_only(quic, 0);
		wtd_log(WTD_LOG_TRACE, "webtransportd: using autocert");
		ptls_iovec_t *chain = (ptls_iovec_t *)malloc(sizeof(ptls_iovec_t));
		if (chain == NULL) {
			wtd_log(WTD_LOG_ERROR, "webtransportd: chain alloc failed");
			picoquic_free(quic);
			free(cert_der);
			free(key_der);
			return 1;
		}
		chain[0].base = cert_der;
		chain[0].len = cert_der_len;
		picoquic_set_tls_certificate_chain(quic, chain, 1);
		wtd_log(WTD_LOG_TRACE, "webtransportd: cert chain installed");
		cert_der = NULL;

		wtd_log(WTD_LOG_TRACE, "webtransportd: installing key (len=%lu)",
				(unsigned long)key_der_len);
		if (picoquic_set_tls_key(quic, key_der, key_der_len) != 0) {
			wtd_log(WTD_LOG_ERROR, "webtransportd: key install failed");
			picoquic_free(quic);
			free(key_der);
			return 1;
		}
		wtd_log(WTD_LOG_TRACE, "webtransportd: key installed");
		free(key_der);
		key_der = NULL;
	}
	/* else: picoquic_create already loaded cert/key from the file paths */

	picoquic_packet_loop_param_t param = { 0 };
	param.local_af = 0; /* 0 = dual-stack: bind both AF_INET and AF_INET6 */
	param.local_port = cfg->port;

	picoquic_network_thread_ctx_t tctx = { 0 };
	tctx.quic = quic;
	tctx.param = &param;
	tctx.loop_callback = server_loop_cb;
	tctx.loop_callback_ctx = &sctx;

#ifdef _WIN32
	tctx.wake_up_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (tctx.wake_up_event != NULL) {
		tctx.wake_up_defined = 1;
	}
#else
	if (pipe(tctx.wake_up_pipe_fd) == 0) {
		int wflags = fcntl(tctx.wake_up_pipe_fd[1], F_GETFL, 0);
		fcntl(tctx.wake_up_pipe_fd[1], F_SETFL, wflags | O_NONBLOCK);
		tctx.wake_up_defined = 1;
	}
#endif
	sctx.net_thread_ctx = &tctx;

	(void)picoquic_packet_loop_v3(&tctx);
	int rc = tctx.return_code;

	picoquic_free(quic);

	if (tctx.wake_up_defined) {
#ifdef _WIN32
		CloseHandle(tctx.wake_up_event);
#else
		close(tctx.wake_up_pipe_fd[0]);
		close(tctx.wake_up_pipe_fd[1]);
#endif
		tctx.wake_up_defined = 0;
	}

	if (rc == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP || rc == 0 ||
			atomic_load(&g_should_exit)) {
		return 0;
	}
	wtd_log(WTD_LOG_ERROR, "webtransportd: packet loop rc=%d", rc);
	return 1;
}
