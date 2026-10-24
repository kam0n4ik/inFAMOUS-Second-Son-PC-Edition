LibAtrac9 (Alex Barney, MIT license: LICENSE) - the ATRAC9 decoder of SecondSonPC's own AJM (src/runtime/audio/ajm.cpp).

  src/                  the C sources of https://github.com/shadps4-emu/ext-LibAtrac9 at 946e05a ("Prevent bitstream
                        overruns (#4)"), the fork of https://github.com/Thealexbarney/LibAtrac9 that adds the 32-bit and
                        float outputs and the non-interleaved output (Atrac9DecodeS32/F32, `nointerleave`). Unmodified.
  sspc_atrac9_prefix.h  SecondSonPC's: renames every external symbol (SspcAt9_*), force-included when the sources are
                        compiled (CMakeLists.txt, target sspc_atrac9), so this copy is independent of the bridge's.

SecondSonPC's own pinned third-party code (docs/AUDIT.md, Phase 6), not part of shadPS4. Packaged builds carry the
license as licenses\LibAtrac9.txt (build.ps1).
