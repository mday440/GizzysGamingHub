# Third-party code in MikesGameRoom

- **nxnative** (Engines/nxnative), Copyright (c) 2026 seedds, MIT licence (Engines/nxnative/LICENSE).
  Imported at the commit named in Engines/nxnative/UPSTREAM.txt. Its own third-party parts keep their
  licences: Ryujinx's shader translator (MIT), dav1d (BSD-2), libvpx (BSD-3), FLIP (BSD-3),
  stb_vorbis (MIT / public domain), Vulkan headers (Apache-2.0 / MIT), macnativegames (MIT).
  MikesGameRoom change: Engines/nxnative/dm/tools/nso.py decodes ZBIC segments with MikesGameRoom's
  own decoder (Sources/mgrzbic) instead of the GPLv2 `zbic` package, which is not used.
- **Zstandard** decoder (Sources/MGRCore/zstddeclib.c), Meta Platforms, BSD licence (ZSTD_LICENSE).
- Instruction field layouts in Sources/MGRCore/sass.h from Mesa NAK (MIT, Collabora).
- BC6H decoder bcdec.h (MIT / Unlicense); BC7 tables after bc7enc (MIT / public domain).

## Fonts

- **Unbounded** (The Unbounded Project Authors) and **Manrope** (The Manrope Project Authors), SIL Open Font License 1.1: `Resources/Fonts`, with their licences beside them.
