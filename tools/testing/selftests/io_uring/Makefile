# SPDX-License-Identifier: GPL-2.0
CFLAGS += -Wall -Wextra -O2 -D_GNU_SOURCE $(KHDR_INCLUDES)
LDLIBS += -luring

TEST_GEN_PROGS := mock-prov-test

include ../lib.mk

# liburing is required; if absent, skip the build rather than fail the tree.
$(OUTPUT)/mock-prov-test: mock-prov-test.c
	@if ! echo '#include <liburing.h>' | $(CC) $(CFLAGS) -E - >/dev/null 2>&1; then \
		echo "skipping io_uring: liburing headers not found"; \
	else \
		$(CC) $(CFLAGS) $< $(LDLIBS) -o $@; \
	fi
