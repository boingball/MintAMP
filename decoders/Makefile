# decoders/Makefile — builds pluggable audio decoder modules for MiniAMP3
#
# Each module is a standalone Amiga hunk-format binary loaded at runtime with
# LoadSeg().  flac_entry.c is compiled and linked FIRST so that
# DecoderModuleEntry() lands at BADDR(seg)+4 (first callable LONG in the code
# hunk).
#
# Prerequisites
# -------------
#   FLAC: git submodule add https://github.com/astoeckel/libfoxenflac flac
#
# Usage
# -----
#   make -C decoders          # build all modules whose libraries are present
#   make -C decoders flac     # build only the FLAC module

CC     := m68k-amigaos-gcc
CPU    := -m68020
CFLAGS := $(CPU) -O2 -fomit-frame-pointer -std=gnu89 -Wall -I. \
          -DHAVE_AMIGA_AUDIO_DEVICE -DAMIGA_M68K

# Post-link: mark hunks as "any RAM, prefer Fast" (same as main binary)
FIXHUNKS := python3 ../tools/amiga_fast_preferred_hunks.py

.PHONY: all flac clean

# Build whichever modules have their upstream library checked out
all: $(if $(wildcard flac/src/foxen-flac.h),flac,)
	@echo "Decoder build complete."
	@if [ ! -f flac/src/foxen-flac.h ]; then \
	    echo "  FLAC skipped: run 'git submodule update --init decoders/flac'"; \
	fi

# --------------------------------------------------------------------------
# FLAC — libfoxenflac (https://github.com/astoeckel/libfoxenflac)
# --------------------------------------------------------------------------
FLAC_SRCS := flac_entry.c flac_alloc.c flac_module.c flac/src/foxen-flac.c

flac: flac.decoder

FLAC_CFLAGS := $(filter-out -std=gnu89 -std=gnu11,$(CFLAGS)) -std=gnu99 -I. -Iflac/src

flac.decoder: $(FLAC_SRCS) decoder_module.h flac/src/foxen-flac.h
	$(CC) $(FLAC_CFLAGS) -nostartfiles -o $@ $(FLAC_SRCS)
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------

clean:
	rm -f flac.decoder
