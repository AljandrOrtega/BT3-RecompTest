# Contributors & roles

BT3-Recomp — a statically recompiled, native PC port of *Dragon Ball Z: Budokai
Tenkaichi 3* (PS2, USA, SLUS-21678), built on
[PS2Recomp](https://github.com/ran-j/PS2Recomp).

## Developers

| Dev | Rol | Áreas |
| --- | --- | --- |
| **z3xox** | Owner / Lead developer | Recompilador (`ps2xRecomp`), runtime EE/GS/VU1/scheduler, renderer OpenGL + paraLLEl-GS, game overrides, generators del juego, docs |
| **RexxColder** | **Supporter** / Colaborador | **Optimización** (perf/async, batching), Launcher Qt6 + install wizard + ISO9660, Input & gamepads, Build/Release (Docker CI, floor gate, packaging), Deploy layout, Game-data (AFS/AFL), Docs |
| **valenvivaldi** | Colaborador | Port macOS arm64, packaging/empaquetado, audio |

## Third-party

| Autor | Aporte | Licencia |
| --- | --- | --- |
| **ran-j** | [PS2Recomp](https://github.com/ran-j/PS2Recomp) — recompilador estático (upstream) | GPL-3.0 |
| **ViveTheModder** | Listas AFS NTSC-U (`PZS3US1.AFL`/`PZS3US2.AFL`) | Apache-2.0 |
| **Arntzen Software** | [paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs) — GS en Vulkan compute | LGPL-3.0-or-later |

## License

This repository is GPL-3.0 (see `LICENSE`). *Dragon Ball Z: Budokai Tenkaichi 3*
© Spike / Bandai Namco. This project is not affiliated with or endorsed by them;
it distributes no game content — the game is recompiled at build time from the
user's own disc image.
