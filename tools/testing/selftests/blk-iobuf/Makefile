# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -Wextra -O2 $(KHDR_INCLUDES)
LDLIBS += -luring

TEST_GEN_PROGS := iobuf-fixed-test

include ../lib.mk

# liburing is required; if absent, skip the build rather than fail the tree.
$(OUTPUT)/iobuf-fixed-test: iobuf-fixed-test.c
	@if ! echo '#include <liburing.h>' | $(CC) $(CFLAGS) -E - >/dev/null 2>&1; then \
		echo "skipping blk-iobuf: liburing headers not found"; \
	else \
		$(CC) $(CFLAGS) $< $(LDLIBS) -o $@; \
	fi
