/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * webtransportd — main.c  (composition root)
 *
 * The outermost shell of the hexagon: parse the command line, then wire
 * the chosen adapter to the chosen ports and let it run. It owns no
 * protocol logic — the session core (domain/session.c) holds the policy,
 * and the picoquic adapter (adapters/transport_picoquic.c) holds the
 * QUIC machinery. This file just decides which to stand up.
 */

#include "version.h"

#include "frame.h"
#include "log.h"
#include "transport_picoquic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --selftest: exercise the frame codec round-trip without any I/O. */
static int cmd_selftest(void) {
	uint8_t buf[256];
	const char *payload = "test";
	size_t payload_len = strlen(payload);
	size_t out_len = 0;

	wtd_frame_status_t encode_rc = wtd_frame_encode(0,
			(const uint8_t *)payload, payload_len,
			buf, sizeof(buf), &out_len);
	if (encode_rc != WTD_FRAME_OK || out_len == 0) {
		return 1;
	}

	uint8_t flag = 0;
	size_t consumed = 0;
	const uint8_t *decoded = NULL;
	size_t decoded_len = 0;
	wtd_frame_status_t decode_rc = wtd_frame_decode(buf, out_len,
			&consumed, &flag, &decoded, &decoded_len);
	if (decode_rc != WTD_FRAME_OK || decoded_len != payload_len ||
			decoded == NULL || memcmp(decoded, payload, payload_len) != 0) {
		return 1;
	}

	printf("selftest ok\n");
	return 0;
}

static int print_usage(FILE *out) {
	fprintf(out, "webtransportd: HTTP/3 WebTransport daemon\n");
	fprintf(out, "usage:\n");
	fprintf(out, "  --version          Print version and exit\n");
	fprintf(out, "  --selftest         Run self-tests\n");
	fprintf(out, "  --server --cert=<pem> --key=<pem> --port=<N>\n");
	fprintf(out, "                      Start server\n");
	fprintf(out, "    --exec=<bin>      Spawn bin on connection\n");
	fprintf(out, "    --dir=<path>      Serve static files\n");
	fprintf(out, "    --log-level=<0-4> Set logging level\n");
	fprintf(out, "\nframing: WebTransport frames encode flag, varint length, payload\n");
	return 0;
}

static int parse_arg_value(const char *arg, const char *prefix,
		const char **out) {
	size_t plen = strlen(prefix);
	if (strncmp(arg, prefix, plen) == 0) {
		*out = arg + plen;
		return 1;
	}
	return 0;
}

int main(int argc, char **argv) {
	int is_server = 0;
	const char *cert = NULL;
	const char *key = NULL;
	const char *port_str = NULL;
	const char *exec_path = NULL;
	const char *dir_path = NULL;
	const char *log_level_str = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--version") == 0) {
			printf("webtransportd %s\n", WTD_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "--selftest") == 0) {
			return cmd_selftest();
		}
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			(void)print_usage(stdout);
			return 0;
		}
		if (strcmp(argv[i], "--server") == 0) {
			is_server = 1;
			continue;
		}
		if (parse_arg_value(argv[i], "--cert=", &cert)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--key=", &key)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--port=", &port_str)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--exec=", &exec_path)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--dir=", &dir_path)) {
			continue;
		}
		if (parse_arg_value(argv[i], "--log-level=", &log_level_str)) {
			continue;
		}
		wtd_log(WTD_LOG_ERROR, "webtransportd: unknown argument: %s", argv[i]);
		(void)print_usage(stderr);
		return 2;
	}

	if (log_level_str != NULL) {
		long level = strtol(log_level_str, NULL, 10);
		if (level < WTD_LOG_QUIET || level > WTD_LOG_TRACE) {
			wtd_log(WTD_LOG_ERROR, "webtransportd: bad --log-level=%s",
					log_level_str);
			return 2;
		}
		wtd_log_set_level((wtd_log_level_t)level);
	}

	if (is_server) {
		int is_auto_cert = (cert != NULL && strcmp(cert, "auto") == 0);
		if (port_str == NULL || cert == NULL ||
				(!is_auto_cert && key == NULL)) {
			wtd_log(WTD_LOG_ERROR,
					"webtransportd: --server requires cert, key, port");
			(void)print_usage(stderr);
			return 2;
		}
		long port = strtol(port_str, NULL, 10);
		if (port <= 0 || port > 65535) {
			wtd_log(WTD_LOG_ERROR, "webtransportd: bad --port=%s", port_str);
			return 2;
		}
		wtd_transport_config_t cfg = {
			.cert = cert,
			.key = key,
			.port = (uint16_t)port,
			.exec_path = exec_path,
			.dir_path = dir_path,
		};
		return wtd_transport_run(&cfg);
	}

	(void)print_usage(stderr);
	return 2;
}
