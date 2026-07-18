MiniAmp3 radio icon assets
==========================

Made as a small Amiga-style radio/player icon for MiniAmp3.

Files:
- miniamp3_radio_normal_32_16col.png       32x32 indexed PNG, 16 colours, transparent index 0
- miniamp3_radio_selected_32_16col.png     selected/active state
- miniamp3_radio_normal_64_16col.png       64x64 nearest-neighbour scale-up
- miniamp3_radio_selected_64_16col.png     selected/active state
- miniamp3_radio_normal_32.ilbm            Amiga IFF/ILBM brush, 4 bitplanes
- miniamp3_radio_selected_32.ilbm          selected/active ILBM
- miniamp3_radio_normal_64.ilbm            64x64 ILBM
- miniamp3_radio_selected_64.ilbm          selected/active 64x64 ILBM
- miniamp3_radio_icon_32_4bpp.h            C header: palette + planar 4bpp body data
- miniamp3_radio_preview_320.png           enlarged preview on checkerboard

Notes:
- Palette index 0 is transparent.
- ILBM files are uncompressed 4-bitplane BODY data with BMHD masking set to transparent colour.
- This is not a ready-made Workbench .info file. It is a source icon/brush asset you can convert into .info
  using your preferred Amiga icon workflow.
