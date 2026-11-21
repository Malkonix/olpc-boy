# olpc-covers

Box art for the launcher: `png2obc` (PNG/JPEG to `.obc`) and
`olpc-covers-match` (ROM name to libretro thumbnail name).  The driver script is
`rootfs/overlay/usr/local/sbin/olpc-covers`.

* `png2obc.c` -- `png2obc in.png out.obc [maxw maxh]` (default 480x480).
  Output: 16-byte header (`OBC1`, u16le width, u16le height, 8 zero bytes) and
  RGB565 little-endian pixels, top row first.  Composited over RGB(24,24,24),
  exact area-average downscale, never upscaled, written atomically.  Rejects
  images above 4096 px per side or 8 Mpixel (exit 3).
* `olpc-covers-match.c` -- see the header comment of the file for the rules.
  Also `--tree2html` (GitHub tree JSON on stdin -> Apache-like index) and
  `--tree-sha DIR`; test.sh checks `--tree2html` byte for byte against the awk
  implementation it replaced (set `OLPC_COVERS_REAL_TREE=file.json` to add a
  real tree).
* `t_gen.c`, `test.sh` -- host tests (`sh src/olpc-covers/test.sh`).
* `stb_image.h` -- v2.32, vendored unmodified from
  https://github.com/nothings/stb at commit
  `9f755b604db3492df2cbeb39bf84b81e3e3932e3` (2026-11-12).  Public domain
  (alternatively MIT; both texts are at the end of the file).  Built with
  `STBI_NO_SIMD` (no SSE2 paths), PNG and JPEG only, no HDR/linear (no libm).

Build (builder container): `gcc -O2 -march=geode -mtune=geode -static`.
