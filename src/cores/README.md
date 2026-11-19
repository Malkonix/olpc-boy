# libretro cores for the OLPC XO-1

Builds gambatte (GB/GBC), QuickNES (NES, fast) and FCEUmm (NES, compatibility)
as `out/cores/<name>_libretro.so` for the Geode LX (`-march=geode`, no SSE),
linked against musl with libstdc++/libgcc linked statically into each .so
(only `libc.musl-x86.so.1` is needed at runtime).

Run from the repo root (same container invocation as the Makefile, builder image
from `make builder`):

    docker run --rm -v $PWD:/work -w /work olpc-boy-builder sh src/cores/build.sh

Pinned upstream commits, licenses and flags are in `build.sh` and are written to
`out/cores/VERSIONS`. Build one core: `... sh src/cores/build.sh quicknes`.

SSE check (accepts a directory):

    docker run --rm -v $PWD:/work -w /work olpc-boy-builder bash scripts/check-sse.sh out/cores

## Smoke test

`build.sh` also builds `out/cores/coretest` (`coretest.c`, dlopens a core, runs N
frames with dummy callbacks, prints geometry/fps/format and host frames per
second) and runs it. Re-run only the test: `sh src/cores/smoke.sh run`
(`FRAMES=n` to change the count). Host fps is not representative of the Geode;
use it only for relative cost.

Test ROMs are downloaded into `out/cores/test-roms/` with pinned sha256 and are
never committed:

- `cpu_instrs.gb`: Blargg's Game Boy CPU test, LGPL-2.1+, from
  github.com/retrio/gb-test-roms.
- `nestest.nes`: kevtris' nestest, freely distributable, from
  github.com/christopherpow/nes-test-roms.

## Licenses

gambatte: GPL-2.0. FCEUmm: GPL-2.0. QuickNES: LGPL-2.1+ core (nes_emu) with GPL-2.0
libretro glue; treat the combined binary as GPL-2.0. A notice with source
location (repo URL + commit, see VERSIONS) is required when distributing the images.
