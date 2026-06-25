# External Decoder Modules

## Overview

The player supports external decoder modules for formats such as FLAC and AAC. Each module exports a `DecoderOps` table through the module entry function, `DecoderModuleEntry()`.

At runtime, the main player loads the decoder module, calls `DecoderModuleEntry()` to obtain the `DecoderOps` table, and then uses the table's `open`, `decode`, and `close` callbacks to manage decoding.

## Critical Amiga Loader Rule

`DecoderModuleEntry` must be at the first callable code address in the module.

In practice, verify every decoder module with:

```sh
m68k-amigaos-nm -n decoders/<name>.decoder | head -20
```

Expected output begins with:

```text
00000000 T _DecoderModuleEntry
```

If another symbol such as `_memset`, `___ashldi3`, or another libgcc helper appears before `_DecoderModuleEntry`, the module may crash immediately with `80000004` or jump into the wrong function.

Always treat entrypoint ordering as the first thing to check when a decoder module crashes before producing any reliable diagnostics.

## Required Entrypoint Pattern

Declare `DecoderModuleEntry` in plain `.text`:

```c
__attribute__((section(".text")))
struct DecoderOps *DecoderModuleEntry(void)
{
    return &gXxxOps;
}
```

The entry function should do almost nothing. It should only return the module's `DecoderOps` table.

Do not allocate memory, open libraries, print debug output, initialise runtime allocator state, or perform decoder setup in `DecoderModuleEntry`. Do runtime setup in `DecoderOps::open` instead.

## Build and Link Rules

Follow these rules for every decoder module build:

- Put the entry object first in the decoder object list.
- Use `-ffunction-sections` carefully.
- Force `DecoderModuleEntry` into plain `.text` so `.text.*` helpers do not sort before it.
- If libgcc helpers are required, link them only after the entry object and verify final symbol order.
- Rebuild cleanly when testing entrypoint or ABI changes.

A conservative clean rebuild sequence is:

```sh
make -C decoders clean
find decoders -name "*.o" -delete
rm -f decoders/*.decoder decoders/*.decoder.map
make -C decoders <decoder>
```

After rebuilding, rerun the `m68k-amigaos-nm -n` check and confirm `_DecoderModuleEntry` is still at address `00000000`.

## Decoder ABI

`DecoderOps::open` initialises decoder state and performs runtime allocator setup.

`DecoderOps::decode` returns the number of samples per channel, also called sample frames, produced by that decode call. Internal decoder libraries may report total interleaved shorts; convert those counts before returning to the player.

PCM output is signed 16-bit interleaved audio:

- Stereo: `L,R,L,R,...`
- Mono: `M,M,...`

Do not return total interleaved shorts as frames. For stereo output, a buffer containing `L,R,L,R` has four interleaved shorts but only two sample frames.

It is acceptable for decoder internals such as `outbufFill` or `outbufPos` to track total interleaved shorts, but the `decode()` return value must match the player ABI and report sample frames.

## Debugging Rules

Avoid `printf`, libc output, DOS `Output()`, and DOS `Write()` from decoder modules unless that path has been proven safe in the current module and loader context.

Prefer host-side diagnostics, deterministic test files, or counters exposed through safe paths. If a module crashes before debug output appears, first check entrypoint ordering with `nm`.

Keep tiny generated test files for codec validation, including:

- Mono AAC-LC sine.
- Stereo AAC-LC sine.
- AAC-LC music with and without TNS.
- FLAC 16-bit stereo test.
- MP3 known-good test.

These files are validation inputs, not source artefacts; do not commit local generated audio unless the repository explicitly adds a curated fixture.

## Submodule Rules

Some decoder source lives in submodules, especially `ESP8266Audio/libhelix-aac`.

When a fix belongs to decoder source inside a submodule:

1. Fix the decoder source inside the submodule repository first.
2. Commit and push the submodule repository.
3. Update the parent repository submodule pointer.
4. Commit the parent repository so it points to the tested submodule commit.

Do not commit `.o` files, Windows `Zone.Identifier` files, local test audio files, or other generated artefacts. Parent repository commits should point only to the tested submodule commit and any necessary parent-repo documentation or build-system changes.

## Known Historical Bugs Fixed

These issues are recorded here so future decoder work does not repeat them:

- FLAC rebuilds crashed because libgcc/helper code landed before `DecoderModuleEntry`.
- AAC initially crashed because `_memset` appeared before `DecoderModuleEntry`.
- AAC real-music glitches were traced to TNS `FilterRegion` fixed-point/endian behaviour on big-endian m68k.
- TNS corruption was proven with `test-tns-only.aac` and fixed in the `ESP8266Audio` submodule.

## Verification Checklist for Any New Decoder

Before merging a new decoder module or decoder ABI change, verify all of the following:

- Clean build completes.
- `m68k-amigaos-nm -n decoders/<name>.decoder | head -20` shows `_DecoderModuleEntry` at `00000000`.
- Local file opens without crashing.
- `decode()` returns expected sample frames.
- Mono and stereo tests pass.
- Real music test passes.
- Rebuild from scratch reproduces the same working binary.
- No generated artefacts are committed.
