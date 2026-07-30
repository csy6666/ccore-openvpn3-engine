# OpenVPN 3 Engine Third-Party Notices

This inventory applies to the Android arm64 engine binary built from the
pinned OpenVPN 3 source described in `CCORE-MPL-BOUNDARY.md`. The proprietary
c-core and CcoreBox trees are not part of this engine source distribution.

## MPL-covered engine

| Component | Version | License |
| --- | --- | --- |
| OpenVPN 3 | `1512c16622288f3c01da09d3278ac61a86dca26d` | MPL-2.0 option selected |
| c-core OpenVPN 3 C ABI and runtime bridge | fork overlay in the compliance bundle | MPL-2.0 |

The compliance bundle contains the exact upstream source archive, the exact
fork overlay, `LICENSE.md`, and `LICENSES/MPL-2.0.txt`. Recipients can recreate
the complete corresponding source by expanding the upstream archive and then
copying the overlay over it.

## Statically linked build dependencies

| Component | Version | License |
| --- | --- | --- |
| Asio | `1.36.0` | Boost Software License 1.0 |
| fmt | `12.1.0` | MIT |
| LZ4 | `1.10.0` | BSD-2-Clause |
| OpenSSL | `3.6.2` | Apache-2.0 |
| LLVM libc++ from Android NDK | `28.1.13356709` | Apache-2.0 with LLVM exceptions and bundled notices |

The engine has no dependency on `libc++_shared.so`; its Android dynamic
dependencies are the platform libraries `libdl.so`, `libm.so`, and `libc.so`.
The generated compliance bundle copies the exact vcpkg copyright/SPDX files
and Android NDK notices used for this build.

`pkgconf` and the vcpkg CMake helper ports are host build tools and are not
linked into `libccore_openvpn3.so`.

This inventory is engineering compliance evidence, not legal advice.
