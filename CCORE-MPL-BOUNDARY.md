# c-core OpenVPN 3 MPL Boundary

This fork is the separately distributable OpenVPN engine for c-core and
CcoreBox. It is distributed under the Mozilla Public License 2.0 option
offered by upstream OpenVPN 3.

## Pinned upstream

- Repository: <https://github.com/OpenVPN/openvpn3>
- Commit: `1512c16622288f3c01da09d3278ac61a86dca26d`
- Selected license: `MPL-2.0`
- Upstream license file: `LICENSES/MPL-2.0.txt`

The files under `ccore/` and `scripts/` added by this fork are MPL-2.0. A
distributed binary must be accompanied by the compliance bundle produced by
`scripts/Package-MPLCompliance.ps1`, or an equivalent durable source offer.
That bundle contains the pinned upstream source, the exact fork overlay and
manifest, MPL terms, linked-dependency notices, and the binary hash.

## Closed-product boundary

The exported API is the C header `ccore/include/ccore_openvpn3.h`. Proprietary
c-core and CcoreBox code must communicate only through this ABI and must not
copy OpenVPN 3 implementation files into their source trees.

The Android product packages this engine as `libccore_openvpn3.so`. CcoreBox
continues to own the only Android `VpnService` and system TUN. OpenVPN 3 uses a
private packet channel supplied by the bridge; it must not install Android
routes or DNS itself.

The runtime transport adapter routes every OpenVPN control and data
channel connection through the c-core dialer selected by the OpenVPN outbound's
`detour`. A failed detour is terminal for that attempt; direct fallback is
forbidden.

This is an engineering compliance boundary, not legal advice.
