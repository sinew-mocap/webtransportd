/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/*
 * examples/shout.c — a webtransportd child that SHOUTS.
 *
 * Reads framed bytes from stdin (flag | varint len | payload), uppercases
 * the ASCII letters in each payload, and writes the result back under the
 * same flag — so a reliable message returns on the stream and a datagram
 * returns as a datagram. Only a-z are touched, so UTF-8 multibyte
 * sequences pass through unchanged.
 *
 * This is examples/echo.c with one transform added; use it as a template
 * for a real child by replacing the uppercase loop with your own logic.
 *
 * Exit codes match examples/echo.c (0 EOF, 1 malloc, 2 frame too big,
 * 3 read error, 4 malformed frame, 5 encode failed, 6 write error).
 */

#include "frame.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define BUF_CAP (1 + 4 + WTD_FRAME_MAX_PAYLOAD)

static int write_all(int fd, const uint8_t *buf, size_t len) {
	size_t done = 0;
	while (done < len) {
		ssize_t n = write(fd, buf + done, len - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return -1;
		}
	}
	return 0;
}

int main(void) {
	uint8_t *in_buf = (uint8_t *)malloc(BUF_CAP);
	uint8_t *up_buf = (uint8_t *)malloc(BUF_CAP);
	uint8_t *out_buf = (uint8_t *)malloc(BUF_CAP);
	if (in_buf == NULL || up_buf == NULL || out_buf == NULL) {
		free(in_buf);
		free(up_buf);
		free(out_buf);
		return 1;
	}
	size_t used = 0;

	for (;;) {
		if (used >= BUF_CAP) {
			free(in_buf);
			free(up_buf);
			free(out_buf);
			return 2;
		}
		ssize_t n = read(STDIN_FILENO, in_buf + used, BUF_CAP - used);
		if (n == 0) {
			break; /* EOF */
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			free(in_buf);
			free(up_buf);
			free(out_buf);
			return 3;
		}
		used += (size_t)n;

		for (;;) {
			size_t consumed = 0;
			uint8_t flag = 0;
			const uint8_t *payload = NULL;
			size_t plen = 0;
			wtd_frame_status_t st = wtd_frame_decode(in_buf, used,
					&consumed, &flag, &payload, &plen);
			if (st == WTD_FRAME_INCOMPLETE) {
				break;
			}
			if (st != WTD_FRAME_OK) {
				free(in_buf);
				free(up_buf);
				free(out_buf);
				return 4;
			}
			/* Uppercase ASCII a-z; leave every other byte (including
			 * UTF-8 continuation bytes) untouched. */
			for (size_t i = 0; i < plen; i++) {
				uint8_t c = payload[i];
				up_buf[i] = (c >= 'a' && c <= 'z') ? (uint8_t)(c - 32) : c;
			}
			size_t out_len = 0;
			wtd_frame_status_t est = wtd_frame_encode(flag, up_buf, plen,
					out_buf, BUF_CAP, &out_len);
			if (est != WTD_FRAME_OK) {
				free(in_buf);
				free(up_buf);
				free(out_buf);
				return 5;
			}
			if (write_all(STDOUT_FILENO, out_buf, out_len) != 0) {
				free(in_buf);
				free(up_buf);
				free(out_buf);
				return 6;
			}
			memmove(in_buf, in_buf + consumed, used - consumed);
			used -= consumed;
		}
	}
	free(in_buf);
	free(up_buf);
	free(out_buf);
	return 0;
}
