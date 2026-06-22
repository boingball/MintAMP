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
NM     := m68k-amigaos-nm
CPU    := -m68020
CFLAGS := $(CPU) -O2 -fomit-frame-pointer -std=gnu89 -Wall -I. \
          -DHAVE_AMIGA_AUDIO_DEVICE -DAMIGA_M68K \
          -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
          -fno-builtin-free -fno-builtin-exit

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
FLAC_OBJS := $(FLAC_SRCS:.c=.o)

flac: flac.decoder

FLAC_CFLAGS := $(filter-out -std=gnu89 -std=gnu11,$(CFLAGS)) -std=gnu99 -I. -Iflac/src

flac_entry.o flac_alloc.o flac_module.o: decoder_module.h
flac_alloc.o flac_module.o: flac_alloc.h
flac_module.o flac/src/foxen-flac.o: flac/src/foxen-flac.h

%.o: %.c
	$(CC) $(FLAC_CFLAGS) -c -o $@ $<

flac.decoder: $(FLAC_OBJS) decoder_module.h flac/src/foxen-flac.h
	rm -f $@ $@.map
	$(CC) $(FLAC_CFLAGS) -nostartfiles -Wl,-Map,$@.map -o $@ $(FLAC_OBJS)
	@if grep -E '(^|/)(malloc\.o|lib_a-exit\.o)([)[:space:]]|$$)' $@.map; then \
	    echo "ERROR: FLAC decoder linked libc malloc.o or lib_a-exit.o" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | grep -E '(^|[[:space:]])(_?SysBase|_?_exit)$$'; then \
	    echo "ERROR: FLAC decoder has unresolved SysBase or _exit" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'flac_module\.o' $@.map; then \
	    echo "ERROR: FLAC decoder map does not mention flac_module.o" >&2; \
	    exit 1; \
	fi
	@if ! strings $@ | grep -q 'FLAC MODULE BUILD MARKER 12345'; then \
	    echo "ERROR: FLAC decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------

clean:
	rm -f flac.decoder flac.decoder.map $(FLAC_OBJS)
