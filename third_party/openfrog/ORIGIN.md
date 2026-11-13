# OpenFROG production-source snapshot

- Source: https://github.com/TTPlayerRebuild/OpenFROG
- Commit: `5dc0c80cf0c357e8cadbbae9b593f1b3e62d917d`.
- Imported from the user-selected local checkout; all 22 production source/header
  and license files are byte-identical to that clean checkout.
- Per-file SHA-256: `SOURCE_MANIFEST.json`, checked at configure and packaging.
- License: MIT, retained in `LICENSE`. VC-LTL and YY-Thunks remain separately
  licensed; the plugin supplies their compatibility runtime once.

This is not the proprietary OptimFROG SDK/DLL. No original DLL, encoder,
decompilation output, private media, tests or build artifacts are included.
The plugin builds these sources directly into a private static library. Its
`cmake/codec.cmake` replaces the upstream shared-library build; no upstream
DLL/import library or absolute local checkout path is required.

The upstream public header already supports plain C declarations using
`OPENFROG_BUILD` + `OPENFROG_USE_DEF` (backend) or `OPENFROG_DYNAMIC_API` (caller).
These suppress dllexport/dllimport for the private static references; none of
the SDK functions are added to the TTPlayer plugin's exported ABI.

Keep `/fp:strict`, scoped FPU/MXCSR handling, C++ exceptions and the upstream
safety/resource limits. Integer PCM 8/16/24/32-bit and OFS are supported;
OFRX/FLOAT32 output, unsafe LPC orders over 128 and unproved predictor domains
are not newly enabled. Ordinary blocks have the upstream 64 MiB budget.
