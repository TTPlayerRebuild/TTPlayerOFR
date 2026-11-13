# OpenFROG production-source snapshot

- Source: https://github.com/TTPlayerRebuild/OpenFROG
- Commit: `c37926cf6fc612cb2e9a0a2371964e0b4ff919d4`.
- Imported from the user-selected local checkout; all 24 production source/header
  and license files are byte-identical to that clean checkout.
- Per-file SHA-256: `SOURCE_MANIFEST.json`, checked at configure and packaging.
- Snapshot SHA-256: `3006513145ee448c247e2b16f814d241d8ccd78021f37510af059c35b798a51c`.
  This hashes UTF-8 lines `sha256 + "  " + path + "\n"`, sorted by path.
  `upstream_worktree: clean` records the checkout state at import, not merely HEAD.
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
safety/resource limits. Integer PCM 8/16/24/32-bit, OFS and OFRX/OFF FLOAT32
decoding are supported. The new `float_details.cpp/.hpp` reconstructs real
COMP/DETA pairs; IEEE32 words are packed as bytes without host float conversion.
The plugin always requests lossless-width output (`max16bit=false`).

This is the decoder dependency closure only. Upstream MD5 verification,
standalone verify/WAVE tools and OFF encoder/range-encoder targets are not
referenced by the SDK decode path and are not built or bundled. Their presence
in the upstream commit does not add a TTPlayer encoder or CRC/MD5 verification
interface. Default decode follows upstream's non-verifying CRC/MD5 policy;
malformed float details fail instead of being concealed as silence.

Unsafe LPC orders over 128 and unproved predictor domains remain unsupported.
Ordinary blocks retain the upstream 64 MiB budget.
