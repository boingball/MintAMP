# decoders/Makefile — builds pluggable audio decoder modules for MiniAMP3
#
# Each module is a standalone Amiga hunk-format binary loaded at runtime with
# LoadSeg().  flac_entry.c is compiled and linked FIRST so that its first
# function lands at BADDR(seg)+4 (the decoder module ABI entry point).
#
# Prerequisites
# -------------
#   FLAC: git submodule add https://github.com/astoeckel/libfoxenflac flac
#   OGG:  git submodule add https://github.com/pschatzmann/arduino-libvorbis-tremor tremor
#         git submodule add https://github.com/xiph/ogg libogg
#
# Usage
# -----
#   make -C decoders          # build FLAC, AAC and OGG modules
#   make -C decoders flac     # build only the FLAC module
#   make -C decoders aac      # build only the AAC module
#   make -C decoders ogg      # build only the OGG/Vorbis (Tremor) module
#   make -C decoders ogg OGGASM=0  # build OGG/Tremor without m68k asm helpers

CC     := m68k-amigaos-gcc
NM     := m68k-amigaos-nm

# CPU selects the target processor: 00=68000, 20=68020, 30=68030 (default),
# 40=68040, 60=68060 -- same numeric convention as Makefile.amiga, which is
# the single source of truth when this is built via `make -f Makefile.amiga
# decoder-modules` (it forwards its own CPU= here). Run standalone (`make -C
# decoders`) it defaults to 30 to match this project's primary target.
CPU ?= 30
CPU_MFLAG_00 := -m68000
CPU_MFLAG_20 := -m68020
CPU_MFLAG_30 := -m68030
CPU_MFLAG_40 := -m68040
CPU_MFLAG_60 := -m68060
CPU_MFLAG := $(CPU_MFLAG_$(CPU))
ifeq ($(CPU_MFLAG),)
$(error Unknown CPU=$(CPU) -- expected one of: 00 20 30 40 60)
endif

OGGASM ?= 1
# override: these decoders link with -nostartfiles against a hand-written
# Exec-backed allocator shim (see ogg_alloc.c/aac_alloc.c/flac_alloc.c) that
# only works if GCC never recognizes/inlines a call as "the real libc
# malloc/free/memset" -- -fno-builtin-* is what prevents that. Previously
# Makefile.amiga's decoder-modules target passed its own CFLAGS="..." on
# the command line, which (per normal GNU Make precedence) silently
# replaced this entire line -- including those flags and -ffunction-
# sections/-fdata-sections needed for the --gc-sections link step -- every
# time this was built via `make -f Makefile.amiga gui/guir/fast030` rather
# than standalone `make -C decoders`. override makes this immune to that.
override CFLAGS := $(CPU_MFLAG) -O2 -fomit-frame-pointer -std=gnu89 -Wall -I. \
          -DHAVE_AMIGA_AUDIO_DEVICE -DAMIGA_M68K \
          -fno-builtin \
          -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc \
          -fno-builtin-free -fno-builtin-exit -fno-builtin-memset \
          -fno-tree-loop-distribute-patterns \
          -ffunction-sections -fdata-sections -DNDEBUG

# Post-link: mark hunks as "any RAM, prefer Fast" (same as main binary)
FIXHUNKS := python3 ../tools/amiga_fast_preferred_hunks.py

.PHONY: all flac aac ogg clean

# The normal decoder build must produce every supported external module.
all: flac aac ogg
	@echo "Decoder build complete."

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
	$(CC) $(FLAC_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(FLAC_OBJS) -lgcc
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
# Exclude SBR files from the AAC module.
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
# OGG/Vorbis — Tremor (fixed-point libvorbis, pschatzmann/arduino-libvorbis-
# tremor, upstream https://gitlab.xiph.org/xiph/tremor) + libogg
# (https://github.com/xiph/ogg).
#
# Prerequisites
# -------------
#   git submodule add https://github.com/pschatzmann/arduino-libvorbis-tremor \
#                     decoders/tremor
#   git submodule add https://github.com/xiph/ogg decoders/libogg
#
# Path layout (verified against the checked-out pschatzmann/arduino-
# libvorbis-tremor submodule — note the extra tremor/ nesting under src/):
#   decoders/tremor/src/tremor/*.c, ivorbisfile.h, ivorbiscodec.h
#   decoders/libogg/src/*.c, decoders/libogg/include/ogg/ogg.h
#
# decoders/tremor/src/tremor/ also ships iseeking_example.c and
# ivorbisfile_example.c — standalone demo apps with their own main() and
# stdio use.  These are filtered out of TREMOR_LIB_SRCS below; they must
# not be linked into the module.
#
# Like AAC, all libogg/Tremor object files are compiled with the forced
# ogg_standalone.h include so malloc/calloc/realloc/free/exit resolve to the
# Exec-backed shim in ogg_alloc.c instead of pulling in libc/newlib startup.
#
# libogg/include/ogg/os_types.h pulls in <ogg/config_types.h>, which upstream
# is generated by libogg's own ./configure or CMake run (from
# config_types.h.in) and isn't checked into the repo.  Since this Makefile
# doesn't run libogg's native build system, ogg-config-shim/ogg/config_types.h
# provides a hand-written equivalent (plain stdint.h typedefs) ahead of
# libogg/include on the search path.
# --------------------------------------------------------------------------

# decoders/tremor is a third-party submodule (pschatzmann/arduino-libvorbis-
# tremor) we don't own, so the m68k asm additions can't live as commits inside
# it; they're carried as ogg-tremor-m68k-asm.patch and applied here. Tremor's
# own src/tremor/misc.h already includes asm_arm.h unconditionally for the
# equivalent ARM fast path, so this follows the same upstream-provided hook
# (see docs/ogg-tremor-m68k-asm.md). Using the new header itself as the
# make target means the patch is only (re-)applied when it isn't already
# present, so a repeat build or an already-patched checkout is a no-op.
OGG_ASM_PATCH  := ogg-tremor-m68k-asm.patch
OGG_ASM_MARKER := tremor/src/tremor/asm_m68k.h

$(OGG_ASM_MARKER): $(OGG_ASM_PATCH)
	@if [ -f $(OGG_ASM_MARKER) ]; then \
	    echo "Tremor m68k asm patch already applied, skipping."; \
	else \
	    echo "Applying Tremor m68k asm patch ($(OGG_ASM_PATCH))..."; \
	    cd tremor && patch -p1 < ../$(OGG_ASM_PATCH); \
	fi

TREMOR_LIB_SRCS := $(filter-out %_example.c,$(wildcard tremor/src/tremor/*.c))
LIBOGG_LIB_SRCS := $(wildcard libogg/src/*.c)
OGG_SRCS := ogg_entry.c ogg_alloc.c ogg_module.c $(TREMOR_LIB_SRCS) $(LIBOGG_LIB_SRCS)
OGG_OBJS := $(OGG_SRCS:.c=.o)

ogg: $(OGG_ASM_MARKER) ogg.decoder

OGG_CFLAGS := $(CFLAGS) -I. -Itremor/src -Itremor/src/tremor -Iogg-config-shim -Ilibogg/include

ifeq ($(OGGASM),1)
OGG_CFLAGS += -D_M68K_ASSEM_
endif

ogg_entry.o ogg_alloc.o ogg_module.o: decoder_module.h
ogg_alloc.o ogg_module.o: ogg_alloc.h
ogg_module.o: tremor/src/tremor/ivorbiscodec.h tremor/src/tremor/ivorbisfile.h

# Compile our wrapper objects with base flags (no forced include needed).
ogg_entry.o ogg_alloc.o ogg_module.o: %.o: %.c
	$(CC) $(OGG_CFLAGS) -c -o $@ $<

# Compile every Tremor / libogg .c with the forced-include shim so that
# malloc/calloc/realloc/free/exit are redirected to ogg_alloc.c.  Also
# redirect fopen/fclose/fread/fseek/ftell: Tremor's vorbisfile.c has a
# legacy FILE*-based ov_fopen()/ov_open() API we never call, but the real
# newlib fopen() would pull in dummy.o's SysBase-dependent syscall stubs
# regardless of whether it's ever reached at runtime.  Also redirect
# _impure_ptr (see ogg_alloc.c) for the same reason -- referenced from an
# errno check in that same dead code, and pulling in the real one cascades
# into the entire stdio implementation and DOSBase.
tremor/src/tremor/%.o: tremor/src/tremor/%.c $(OGG_ASM_MARKER)
	$(CC) $(OGG_CFLAGS) \
	    -include ogg_standalone.h \
	    -Dmalloc=OggModuleMalloc \
	    -Dcalloc=OggModuleCalloc \
	    -Drealloc=OggModuleRealloc \
	    -Dfree=OggModuleFree \
	    -Dexit=OggModuleExit \
	    -Dfopen=OggModuleFopen \
	    -Dfclose=OggModuleFclose \
	    -Dfread=OggModuleFread \
	    -Dfseek=OggModuleFseek \
	    -Dftell=OggModuleFtell \
	    -D_impure_ptr=OggModuleImpurePtr \
	    -DNDEBUG \
	    -c -o $@ $<

libogg/src/%.o: libogg/src/%.c
	$(CC) $(OGG_CFLAGS) \
	    -include ogg_standalone.h \
	    -Dmalloc=OggModuleMalloc \
	    -Dcalloc=OggModuleCalloc \
	    -Drealloc=OggModuleRealloc \
	    -Dfree=OggModuleFree \
	    -Dexit=OggModuleExit \
	    -Dfopen=OggModuleFopen \
	    -Dfclose=OggModuleFclose \
	    -Dfread=OggModuleFread \
	    -Dfseek=OggModuleFseek \
	    -Dftell=OggModuleFtell \
	    -D_impure_ptr=OggModuleImpurePtr \
	    -DNDEBUG \
	    -c -o $@ $<

ogg.decoder: $(OGG_OBJS) decoder_module.h tremor/src/tremor/ivorbisfile.h
	rm -f $@ $@.map
	@echo "OGG decoder unresolved-symbol audit:"; \
	    $(NM) -u ogg_entry.o ogg_alloc.o ogg_module.o || true
	@if $(NM) -u ogg_entry.o ogg_module.o $(TREMOR_LIB_SRCS:.c=.o) $(LIBOGG_LIB_SRCS:.c=.o) 2>/dev/null | \
	    grep -iE '(^|[[:space:]])(_?malloc_r|_?calloc_r|_?realloc_r|_?free_r|malloc|calloc|realloc|free|exit|_exit|abort|assert|___assert_func|printf|fprintf|puts|putchar|fflush|stdio|vfprintf|fopen|fclose|fread|fseek|ftell|getpid|_?sbrk|isatty|_impure_ptr|_?fstat|_?kill|_?lseek|_?unlink|ioctl)$$'; then \
	    echo "ERROR: OGG decoder objects reference forbidden libc/startup symbols outside ogg_alloc.o" >&2; \
	    exit 1; \
	fi
	$(CC) $(OGG_CFLAGS) -nostartfiles -Wl,--gc-sections -Wl,-Map,$@.map -o $@ $(OGG_OBJS) -lgcc
	@if grep -iE "printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|_exit|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise" $@.map; then \
	    echo "ERROR: OGG decoder linked forbidden libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if $(NM) -u $@ 2>/dev/null | \
	    grep -iE 'printf|fprintf|puts|putchar|fflush|stdio|vfprintf|malloc\.o|lib_a-exit|abort|assert|___assert_func|(^|[[:space:]])_?exit$$|__EXIT_LIST__|DOSBase|SysBase|dummy\.o|raise'; then \
	    echo "ERROR: OGG decoder has forbidden unresolved libc/startup symbols" >&2; \
	    exit 1; \
	fi
	@if ! grep -q 'ogg_module\.o' $@.map; then \
	    echo "ERROR: OGG decoder map does not mention ogg_module.o" >&2; \
	    exit 1; \
	fi
	@if ! strings $@ | grep -q 'OGG MODULE BUILD MARKER 12345'; then \
	    echo "ERROR: OGG decoder is missing build marker string" >&2; \
	    exit 1; \
	fi
	@first_sym=$$($(NM) -n $@ | grep -iE ' [TtWw] ' | head -1 | awk '{print $$3}'); \
	    if [ "$$first_sym" != "_DecoderModuleEntry" ] && [ "$$first_sym" != "DecoderModuleEntry" ]; then \
	    echo "ERROR: OGG decoder first text symbol is '$$first_sym', expected DecoderModuleEntry" >&2; \
	    exit 1; \
	fi
	$(FIXHUNKS) $@
	@echo "Built $@"

# --------------------------------------------------------------------------

clean:
	rm -f flac.decoder flac.decoder.map $(FLAC_OBJS)
	rm -f aac.decoder  aac.decoder.map  $(AAC_OBJS)
	rm -f ogg.decoder  ogg.decoder.map  $(OGG_OBJS)
