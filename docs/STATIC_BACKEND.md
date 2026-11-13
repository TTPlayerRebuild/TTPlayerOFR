# Static OpenFROG backend

`cmake/codec.cmake` builds the pinned, source-only `third_party/openfrog` snapshot
as a private static library. `src/decoder.cpp` calls its C API directly, retaining
the SDK's pack(1) x86 declarations without dllimport/dllexport. The public TTPlayer
AddIn/Reader/metadata/thumbnail/Commit/Probe interfaces remain unchanged.

There is no embedded codec PE, resource 101, `EmbeddedLibrary`, runtime codec
lookup, or TEMP extraction. The final plugin owns the only XP-compatible CRT /
YY-Thunks entry point; the upstream shared-library CMake target is not built.
The backend is compiled with `/fp:strict` and SSE2; do not replace that with
fast-math or change exception/safety behavior to save size.

The snapshot's provenance and per-file byte identities are in `ORIGIN.md` and
`SOURCE_MANIFEST.json`, checked at configure and source packaging. Source packages
carry all used production files and licenses, not the official SDK archive or
the user's original checkout, test data or build products. No adjacent repository
is needed to rebuild the plugin; only the normal pinned compatibility-toolchain
downloads/caches are required.

This removes the TEMP requirement for playback, information and seek. A metadata
commit still uses safe replacement/backup files beside the audio file, then
cleans them up. Those transactions are independent of codec loading and must not
be removed to implement a blanket "no temporary files" policy.
