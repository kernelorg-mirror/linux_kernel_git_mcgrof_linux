# SPDX-License-Identifier: GPL-2.0
CFLAGS += -static -O3 -Wl,-no-as-needed -Wall $(KHDR_INCLUDES)

TEST_GEN_PROGS = dmabuf-heap dmabuf-block-io
# Pair-specific diagnostic is installed but never automatically run on hardware.
TEST_FILES += pci-peer-check.py pci-peer-policy.json pci-peer-check.rst
TEST_PROGS += pci-peer-check-test.py

include ../lib.mk
