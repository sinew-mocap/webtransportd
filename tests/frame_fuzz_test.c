/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* frame_fuzz_test is a deterministic fuzz harness for the codec, in two
 * passes:
 *
 *     1. fuzz_decode feeds N random buffers (length 0..127, random bytes)
 *        to wtd_frame_decode. Every call returns a defined status code,
 *        and an OK result yields a (consumed, payload) triple that stays
 *        inside the input buffer. ASAN is the teeth: an OOB read from a
 *        malformed length prefix trips it regardless of what the C-level
 *        assertions say.
 *
 *     2. roundtrip encodes and decodes N random (flag, payload_len,
 *        payload_bytes) inputs; the result matches the input
 *        byte-for-byte. It exercises the 1-byte and 2-byte varint length
 *        ranges; frame_test covers the 4-byte form.
 *
 *   The seed is fixed (srand(0xC0DE)) so a regression surfaces on the
 *   same iteration on every run.
 */

#include "frame.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

enum {
	FUZZ_DECODE_ITERATIONS   = 20000,
	ROUNDTRIP_ITERATIONS     = 2000,
	FUZZ_DECODE_BUF_MAX      = 128,
	ROUNDTRIP_PAYLOAD_MAX    = 2048,
};

static void fuzz_decode(void) {
	uint8_t buf[FUZZ_DECODE_BUF_MAX];
	for (int i = 0; i < FUZZ_DECODE_ITERATIONS; i++) {
		size_t buf_len = (size_t)(rand() % (int)sizeof(buf));
		for (size_t j = 0; j < buf_len; j++) {
			buf[j] = (uint8_t)rand();
		}
		size_t consumed = 0;
		uint8_t flag = 0;
		const uint8_t *payload = NULL;
		size_t payload_len = 0;
		wtd_frame_status_t st = wtd_frame_decode(buf, buf_len,
				&consumed, &flag, &payload, &payload_len);
		/* Every return path must be one of the documented codes.
		 * BUF_TOO_SMALL is encode-only. */
		EXPECT(st == WTD_FRAME_OK
				|| st == WTD_FRAME_INCOMPLETE
				|| st == WTD_FRAME_ERR_RESERVED
				|| st == WTD_FRAME_ERR_TOO_BIG);
		if (st == WTD_FRAME_OK) {
			EXPECT(consumed <= buf_len);
			/* payload pointer is inside buf, and
			 * payload+payload_len stays inside buf too. */
			EXPECT(payload >= buf);
			EXPECT(payload + payload_len <= buf + buf_len);
		}
	}
}

static void roundtrip(void) {
	static uint8_t payload[ROUNDTRIP_PAYLOAD_MAX];
	static uint8_t wire[ROUNDTRIP_PAYLOAD_MAX + 8];
	for (int i = 0; i < ROUNDTRIP_ITERATIONS; i++) {
		uint8_t flag = (uint8_t)(rand() & 1);
		size_t payload_len = (size_t)(rand() % (sizeof(payload) + 1));
		for (size_t j = 0; j < payload_len; j++) {
			payload[j] = (uint8_t)rand();
		}

		size_t wire_len = 0;
		wtd_frame_status_t est = wtd_frame_encode(flag, payload,
				payload_len, wire, sizeof(wire), &wire_len);
		EXPECT(est == WTD_FRAME_OK);
		if (est != WTD_FRAME_OK) {
			continue;
		}

		size_t consumed = 0;
		uint8_t out_flag = 0xff;
		const uint8_t *out_payload = NULL;
		size_t out_payload_len = 0;
		wtd_frame_status_t dst = wtd_frame_decode(wire, wire_len,
				&consumed, &out_flag, &out_payload, &out_payload_len);
		EXPECT(dst == WTD_FRAME_OK);
		EXPECT(consumed == wire_len);
		EXPECT(out_flag == flag);
		EXPECT(out_payload_len == payload_len);
		if (payload_len > 0) {
			EXPECT(memcmp(out_payload, payload, payload_len) == 0);
		}
	}
}

int main(void) {
	srand(0xC0DE);
	fuzz_decode();
	roundtrip();
	return failures == 0 ? 0 : 1;
}
