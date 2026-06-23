# decoders/Makefile — builds pluggable audio decoder modules for MiniAMP3
#
# Each module is a standalone Amiga hunk-format binary loaded at runtime with
# LoadSeg().  flac_entry.c is compiled and linked FIRST so that its first
# function lands at BADDR(seg)+4 (the decoder module ABI entry point).
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
          -fno-builtin \
          -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
          -fno-builtin-free -fno-builtin-exit -fno-builtin-memset \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections -DNDEBUG

# Post-link: mark hunks as "any RAM, prefer Fast" (same as main binary)
FIXHUNKS := python3 ../tools/amiga_fast_preferred_hunks.py

.PHONY: all flac aac clean

# Build whichever modules have their upstream library checked out
all: $(if $(wildcard flac/src/foxen-flac.h),flac,) \
     $(if $(wildcard aac/aacdec.h),aac,)
	@echo "Decoder build complete."
	@if [ ! -f flac/src/foxen-flac.h ]; then \
	    echo "  FLAC skipped: run 'git submodule update --init decoders/flac'"; \
	fi
	@if [ ! -f aac/aacdec.h ]; then \
	    echo "  AAC  skipped: clone libhelix-aac into decoders/aac/"; \
	    echo "    e.g. git submodule add https://github.com/earlephilhower/ESP8266Audio decoders/esp8266audio"; \
	    echo "         then symlink or copy esp8266audio/src/libhelix-aac/ to decoders/aac/"; \
	fi

# --------------------------------------------------------------------------
# FLAC — libfoxenflac (https://github.com/astoeckel/libfoxenflac)
# --------------------------------------------------------------------------
FLAC_SRCS := flac_entry.c flac_alloc.c flac_module.c flac/src/foxen-flac.c
FLAC_OBJS := $(FLAC_SRCS:.c=.o)

flac: flac.decoder

FLAC_CFLAGS := $(filter-out -std=gnu89 -std=gnu11,$(CFLAGS)) -std=gnu99 -I. -Iflac/src

flac_entry.o flac_alloc.o flac_module.o: decoder_module.h
flac_alloc.o flac_module.o flac/src/foxen-flac.o: flac_alloc.h
flac/src/foxen-flac.o: flac_standalone.h
flac_module.o flac/src/foxen-flac.o: flac/src/foxen-flac.h
flac/src/foxen-flac.o: FLAC_CFLAGS += -include flac_standalone.h \
	-Dmalloc=FlacModuleMalloc -Dcalloc=FlacModuleCalloc \
	-Drealloc=FlacModuleRealloc -Dfree=FlacModuleFree \
	-Dexit=FlacModuleExit -DNDEBUG \
	-DFX_FLAC_NO_CRC \
	-DAMIGA_M68K_ASM

%.o: %.c
	$(CC) $(FLAC_CFLAGS) -c -o $@ $<

flac.decoder: $(FLAC_OBJS) decoder_module.h flac/src/foxen-flac.h
	rm -f $@ $@.map
	@echo "FLAC decoder unresolved-symbol audit:"; \
	    $(NM) -u flac_entry.o flac_alloc.o flac_module.o flac/src/foxen-flac.o || true
	@if $(NM) -u flac_entry.o flac_module.o flac/src/foxen-flac.o 2>/dev/null | grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: FLAC decoder objects reference forbidden libc/startup symbols outside flac_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(FLAC_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(FLAC_OBJS)
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: FLAC decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: FLAC decoder has forbidden unresolved libc/startup symbols" >&2; \
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
# AAC — libhelix-aac (ESP8266Audio/src/libhelix-aac or compatible)
#
# Prerequisites
# -------------
#   Place the libhelix-aac source files in decoders/aac/.  The simplest
#   approach is to add the ESP8266Audio repo as a submodule and symlink:
#
#     git submodule add https://github.com/earlephilhower/ESP8266Audio \
#                       decoders/esp8266audio
#     ln -s esp8266audio/src/libhelix-aac decoders/aac
#
#   Or clone a standalone libhelix-aac repo directly into decoders/aac/.
#
# SBR (HE-AAC)
# ------------
#   SBR is disabled by default (aac_standalone.h sets ESP8266 which
#   suppresses AAC_ENABLE_SBR).  To enable SBR: remove the ESP8266 define
#   from aac_standalone.h and include the sbr*.c files in AAC_LIB_SRCS.
# --------------------------------------------------------------------------

# Exclude SBR files — enabled by default to keep memory footprint small.
# Remove the filter-out to build with HE-AAC/SBR support.
AAC_LIB_SRCS := $(filter-out $(wildcard aac/sbr%.c), $(wildcard aac/*.c))
AAC_SRCS     := aac_entry.c aac_alloc.c aac_module.c $(AAC_LIB_SRCS)
AAC_OBJS     := $(AAC_SRCS:.c=.o)

aac: aac.decoder

# Arduino/pgmspace shim directory — intercepted before any system headers.
AAC_SHIM_DIR := aac-arduino-shim

AAC_CFLAGS := $(CFLAGS) -I. -I$(AAC_SHIM_DIR) -Iaac -DARDUINO

aac_entry.o aac_alloc.o aac_module.o: decoder_module.h
aac_alloc.o aac_module.o: aac_alloc.h
aac_module.o: aac/aacdec.h

# Compile our wrapper objects with base flags (no Arduino shim needed).
aac_entry.o aac_alloc.o aac_module.o: %.o: %.c
	$(CC) $(AAC_CFLAGS) -c -o $@ $<

# Compile every libhelix-aac .c with the forced-include shim so that
# malloc/calloc/realloc/free/exit are redirected and Arduino headers are
# resolved through aac-arduino-shim/.
aac/%.o: aac/%.c
	$(CC) $(AAC_CFLAGS) \
	    -include aac_standalone.h \
	    -Dmalloc=AacModuleMalloc \
	    -Dcalloc=AacModuleCalloc \
	    -Drealloc=AacModuleRealloc \
	    -Dfree=AacModuleFree \
	    -Dexit=AacModuleExit \
	    -DNDEBUG \
	    -c -o $@ $<

aac.decoder: $(AAC_OBJS) decoder_module.h aac/aacdec.h
	rm -f $@ $@.map
	@echo "AAC decoder unresolved-symbol audit:"; \
	    $(NM) -u aac_entry.o aac_alloc.o aac_module.o || true
	@if $(NM) -u aac_entry.o aac_module.o $(AAC_LIB_SRCS:.c=.o) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf)$$'; then \
	    echo "ERROR: AAC decoder objects reference forbidden libc/startup symbols outside aac_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(AAC_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(AAC_OBJS)
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: AAC decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: AAC decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'aac_module\.o' $@.map; then \
	    echo "ERROR: AAC decoder map does not mention aac_module.o" >&2; \
	    exit 1; \
	fi
	@if ! strings $@ | grep -q 'AAC MODULE BUILD MARKER 12345'; then \
	    echo "ERROR: AAC decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------

clean:
	rm -f flac.decoder flac.decoder.map $(FLAC_OBJS)
	rm -f aac.decoder  aac.decoder.map  $(AAC_OBJS)
