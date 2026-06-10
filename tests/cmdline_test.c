/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* wtd_build_cmdline emits an MSDN-compliant quoted-argv string so
 * CommandLineToArgvW parses it back to the same argv[] byte-for-byte: it
 * quotes args, escapes embedded quotes and backslash runs (so an arg
 * containing `"` or trailing `\` survives), and passes UTF-8 bytes
 * through unchanged.
 */

#include "cmdline.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)
#define EXPECT_STREQ(actual, expected) do { \
	if (strcmp((actual), (expected)) != 0) { \
		fprintf(stderr, "FAIL %s:%d: expected %s, got %s\n", \
			__FILE__, __LINE__, (expected), (actual)); \
		failures++; \
	} \
} while (0)

static void cycle40b_plain_bare(void) {
	/* A single argv[0] with no special characters passes through
	 * unmodified and unquoted. */
	const char *argv[] = { "program", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("plain_bare encode failed");
		return;
	}
	EXPECT_STREQ(buf, "program");
}

static void cycle40b_space_wrapped(void) {
	/* An arg with a space is wrapped in double quotes. Adjacent
	 * args join with a space separator; bare args stay bare. */
	const char *argv[] = { "a b", "c", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("space_wrapped encode failed");
		return;
	}
	EXPECT_STREQ(buf, "\"a b\" c");
}

static void cycle40b_tab_wrapped(void) {
	/* Tab is whitespace at the top level too. */
	const char *argv[] = { "a\tb", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("tab_wrapped encode failed");
		return;
	}
	EXPECT_STREQ(buf, "\"a\tb\"");
}

static void cycle40b_backslashes_only_bare(void) {
	/* A path with embedded backslashes but no spaces and no quotes
	 * passes through unwrapped and uncorrupted — MSDN rule 4:
	 * backslashes not preceding a quote are literal. */
	const char *argv[] = { "path\\to\\file", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("backslashes_only_bare encode failed");
		return;
	}
	EXPECT_STREQ(buf, "path\\to\\file");
}

static void cycle40b_embedded_quote(void) {
	/* A quote inside the arg forces wrapping. The quote itself
	 * escapes as `\"`. Input: he said "hi" → `"he said \"hi\""`. */
	const char *argv[] = { "he said \"hi\"", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("embedded_quote encode failed");
		return;
	}
	EXPECT_STREQ(buf, "\"he said \\\"hi\\\"\"");
}

static void cycle40b_trailing_backslash_in_quoted(void) {
	/* An arg that forces wrapping (because of a space) and ends
	 * with a backslash: the trailing backslash run gets doubled
	 * before the closing quote. MSDN rule: N backslashes followed
	 * by the closing `"` encode as 2N backslashes. */
	const char *argv[] = { "foo bar\\", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("trailing_backslash_in_quoted encode failed");
		return;
	}
	EXPECT_STREQ(buf, "\"foo bar\\\\\"");
}

static void cycle40b_backslashes_before_embedded_quote(void) {
	/* A run of N backslashes followed by `"` inside the arg
	 * encodes as 2N backslashes + `\"`. Input: foo\"bar (space not
	 * needed — the quote forces wrapping). */
	const char *argv[] = { "a\\\\\"b", NULL }; /* a\\"b (two backslashes, one quote, then b) */
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("backslashes_before_embedded_quote encode failed");
		return;
	}
	/* 2 backslashes + " → 4 backslashes + \"
	 * Quoted wrapping adds surrounding ". Full: "a\\\\\"b" */
	EXPECT_STREQ(buf, "\"a\\\\\\\\\\\"b\"");
}

static void cycle40b_utf8_passthrough(void) {
	/* UTF-8 bytes are not special in Windows command-line encoding
	 * — they pass through byte-for-byte as long as no whitespace
	 * or quote is present in the arg. The daemon's manifest pins
	 * the active code page to UTF-8, so CommandLineToArgvW on the
	 * receiving side decodes them back as UTF-8. */
	const char *argv[] = { "文档/bin/echo", NULL };
	char buf[64];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("utf8_passthrough encode failed");
		return;
	}
	EXPECT_STREQ(buf, "文档/bin/echo");
}

static void cycle40b_buffer_too_small(void) {
	/* Insufficient capacity → -1, no partial write observable via
	 * return value (caller must not inspect `out` after -1). */
	const char *argv[] = { "abcdefghij", NULL };
	char buf[4];
	int rc = wtd_build_cmdline(argv, buf, sizeof(buf));
	if (rc != -1) {
		FAIL("buffer_too_small must return -1");
	}
}

static void cycle40b_empty_argv(void) {
	/* argv[0] == NULL → empty command line, trailing NUL only. */
	const char *argv[] = { NULL };
	char buf[8];
	if (wtd_build_cmdline(argv, buf, sizeof(buf)) != 0) {
		FAIL("empty_argv encode failed");
		return;
	}
	EXPECT_STREQ(buf, "");
}

/* wtd_split_cmdline tokenizes an --exec string into argv. */
static void split_single(void) {
	char cmd[] = "./examples/echo";
	const char *argv[8];
	size_t n = wtd_split_cmdline(cmd, argv, 8);
	if (n != 1) { FAIL("split_single argc"); return; }
	EXPECT_STREQ(argv[0], "./examples/echo");
	EXPECT(argv[1] == NULL);
}

static void split_program_and_arg(void) {
	char cmd[] = "python3 ./examples/echo.py";
	const char *argv[8];
	size_t n = wtd_split_cmdline(cmd, argv, 8);
	if (n != 2) { FAIL("split_two argc"); return; }
	EXPECT_STREQ(argv[0], "python3");
	EXPECT_STREQ(argv[1], "./examples/echo.py");
	EXPECT(argv[2] == NULL);
}

static void split_quoted_keeps_spaces(void) {
	char cmd[] = "py \"a b.py\" tail";
	const char *argv[8];
	size_t n = wtd_split_cmdline(cmd, argv, 8);
	if (n != 3) { FAIL("split_quoted argc"); return; }
	EXPECT_STREQ(argv[0], "py");
	EXPECT_STREQ(argv[1], "a b.py");
	EXPECT_STREQ(argv[2], "tail");
}

static void split_extra_whitespace(void) {
	char cmd[] = "  a\t b  ";
	const char *argv[8];
	size_t n = wtd_split_cmdline(cmd, argv, 8);
	if (n != 2) { FAIL("split_ws argc"); return; }
	EXPECT_STREQ(argv[0], "a");
	EXPECT_STREQ(argv[1], "b");
}

static void split_empty(void) {
	char cmd[] = "   ";
	const char *argv[4];
	size_t n = wtd_split_cmdline(cmd, argv, 4);
	EXPECT(n == 0);
	EXPECT(argv[0] == NULL);
}

int main(void) {
	cycle40b_plain_bare();
	cycle40b_space_wrapped();
	cycle40b_tab_wrapped();
	cycle40b_backslashes_only_bare();
	cycle40b_embedded_quote();
	cycle40b_trailing_backslash_in_quoted();
	cycle40b_backslashes_before_embedded_quote();
	cycle40b_utf8_passthrough();
	cycle40b_buffer_too_small();
	cycle40b_empty_argv();
	split_single();
	split_program_and_arg();
	split_quoted_keeps_spaces();
	split_extra_whitespace();
	split_empty();
	return failures == 0 ? 0 : 1;
}
