# Texture replacement

The runtime loads packs from **`data/Textures`** (the deploy's `data/` folder, next to the
extracted ISO tree). Drop a **PCSX2 texture pack** there and enable **Texture Replacement** in the
in-game overlay (Select+Start, or LShift+Tab → Video).

This repo folder, `textures/`, is only the staging source: at build time CMake copies its
contents into `<runner dir>/data/Textures`. To keep packs elsewhere at runtime, use
`PS2X_TEXREPLACE=<dir>`.

## It is PCSX2-compatible on purpose

Textures are identified exactly the way PCSX2 identifies them, so its existing packs work
unchanged — no conversion, no renaming:

    <TEX0Hash>-<CLUTHash>-<bits>.png

Both hashes are XXH3-64: the first over the raw texture data in GS block order, the second over
the palette. Verified against real PCSX2 dumps of this game — filenames come out byte-identical.

## Layout

Anything inside the pack folder is found, **at any depth**. You can unpack an archive there without
flattening it, and a pack that ships as `SLUS-xxxxx/replacements/*.png` works as-is.
PCSX2 searches its own replacements folder recursively too.

Higher-resolution replacements need no special handling: the renderer samples with normalised
texture coordinates, so a 4x or 8x texture drops straight in.

## Notes

- The toggle applies **live** — it flushes the texture cache so the change is immediate.
- Coverage is whatever the pack has. A pack with 1,500 textures will not cover every surface in
  every fight, so expect some upgraded and some untouched.
- Replacements cost VRAM: a 4x texture is 16x the memory of the original, 8x is 64x. Large packs
  on a GPU with little memory to spare are the case to watch.
- `PS2X_TEXREPLACE=<dir>` overrides the default `data/Textures` folder if you keep packs elsewhere.
