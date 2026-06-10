/* SPDX-License-Identifier: BSD-2-Clause */
/* Copyright (c) 2026, K. S. Ernest (iFire) Lee */
/* picoquic_link_test is a minimal linkage check against vendored picoquic.
 * picoquic_error_name(PICOQUIC_TRANSPORT_INTERNAL_ERROR) returns a non-NULL
 * string containing "internal", proving the picoquic source tree's headers
 * are reachable under our flags and that at least one of its translation
 * units (error_names.c) compiles and links into our binary.
 */

#include "picoquic.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define FAIL(msg) do { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } while (0)
#define EXPECT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

int main(void) {
	const char *name = picoquic_error_name(PICOQUIC_TRANSPORT_INTERNAL_ERROR);
	EXPECT(name != NULL);
	EXPECT(name != NULL && strcmp(name, "internal") == 0);
	return failures == 0 ? 0 : 1;
}
