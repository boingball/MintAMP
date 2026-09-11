# Optimization and feature status

MintAMP 1.3's release-enabled m68k optimizations are established, tested paths. Some command-line options and public/internal setter names still contain `exp` or `Experimental` because those names were introduced while the optimizations were being developed. They are retained for compatibility; the name is historical and does **not** mean the corresponding release-enabled path is untested.

## Current release status

- The 68030/68040 release path uses the established full m68k optimization bundle and has been validated on classic hardware.
- The 68060 release path uses the dedicated, 68060-safe combination `lowrate060 huffman midside planars8`, avoiding register-pair long multiply forms that the 68060 handles through software emulation.
- The runtime quality and speed modes — Faster/Fast/Normal/Best plus Fast, Superfast and Ultrafast playback modes — are supported features. The faster modes intentionally trade bandwidth/quality for CPU headroom where documented; that is a user-selectable quality tradeoff, not an experimental-status warning.
- Classic WMA support is working for WMAv1/WMAv2 in ASF containers. WMA Pro, Lossless and Voice remain out of scope.
- AAC, FLAC, Ogg/Tremor and WMA m68k helper paths enabled by the normal release recipes are validated. Their `*ASM=0` switches remain useful as portable-C A/B/reference fallbacks.

## Legacy names kept for compatibility

The following names are historical compatibility/debug interfaces:

- `--exp-huff`
- `--exp-poly`
- `MP3SetExperimentalHuffman()`
- `MP3SetExperimentalPolyphase()`

Do not rename or remove them casually: scripts, frontends or host applications may use the existing API/CLI spelling. Documentation should describe the underlying paths as **validated**, **supported**, **release-tested**, or **legacy-named**, rather than inferring stability from the old name.

## 68060 warning is still important

"Validated" does not mean every 68030 assembly group is suitable for a 68060. The 68060 does not implement the register-pair 32x32-to-64 `MULS.L`/`MULU.L` forms in hardware. The old 68030-heavy paths that use those forms can trap into software emulation and cause severe playback stalls. Groups documented as `AVOID` for 68060, and the `full030` compatibility/audit configuration, remain test-only on that CPU.

The dedicated 68060 release configuration exists specifically to keep the proven speedups while avoiding those trap-prone instructions.

## Historical development notes

Several files under `docs/` are optimization and benchmark logs written while these paths were being developed. Statements such as "not yet verified", "experiment", or "record on target" can describe the state at that point in the development history. They should not be treated as the current release status.

For current build/release decisions, use this page together with:

- `README.md`
- `README.amiga.md`
- `docs/building-amiga.md`
- `BUILD-RELEASE.txt`

Those current-facing documents and the release recipe take precedence over older benchmark-stage wording.
