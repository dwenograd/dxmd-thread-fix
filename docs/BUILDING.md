# Building from source

## Prerequisites

- **Windows 10 or 11** (64-bit)
- **Visual Studio 2022** (Community is fine), with the
  **"Desktop development with C++"** workload installed. This
  provides the toolchain we need: `cl.exe`, `link.exe`, `ml64.exe`,
  `rc.exe`. Other compilers (GCC, Clang) are not supported.
- **PowerShell 5.1 or 7+** (5.1 ships with Windows).

## Build

```powershell
git clone https://github.com/dwenograd/dxmd-thread-fix.git
cd dxmd-thread-fix
powershell.exe -ExecutionPolicy Bypass -File build.ps1
# Or, if you have PowerShell 7 installed:
pwsh -File build.ps1
```

Output:
- `dist\dxgi.dll` — ~150 KB. The artifact you'd install or
  distribute.

(The Release build does NOT emit a PDB. If you want symbols for
debugging, edit `build.ps1` to add `/Zi` to the cl flags and
`/DEBUG` to the link flags.)

The script prints the SHA-256 of the built DLL at the end.
Self-builds are NOT expected to match the SHA-256 of official
releases byte-for-byte — MSVC embeds timestamps and other linker /
toolchain metadata into PE files that differ between machines. What
you CAN verify identically is:

- The export table (`dumpbin /exports dist\dxgi.dll`) — exactly 20
  names + ordinals matching `C:\Windows\System32\dxgi.dll`.
- The import table (`dumpbin /imports dist\dxgi.dll`) — only
  `KERNEL32.dll`.
- The embedded VERSIONINFO `ProductName`
  (`(Get-Item dist\dxgi.dll).VersionInfo.ProductName`) should be
  `dxmd-thread-fix`.

## Customizing the hardcoded defaults

v1.1.0 hardcodes three values into the DLL that earlier (v1.0.0)
versions read from an INI:

| Constant | Default | Where |
|---|---|---|
| `kLogicalProcessors` | `8`     | `src/dxmd_thread_fix.cpp` SECTION 8 (in `attach()`) |
| `kClampAffinity`     | `false` | same |
| `kLogLevel`          | `1`     | same |

If you have a CPU where the community-validated 8-logical-processor
report doesn't work and need to try a different value, edit those
constants and rebuild. Same for enabling the `SetThreadAffinityMask`
hook (`kClampAffinity = true`) or quieting the log
(`kLogLevel = 0`).

The vast majority of users should never need to touch these — that's
why the INI was removed.

## Build flags

The Release build uses:

- `/O2 /Oi /GL /MT` — optimize for speed, enable intrinsics,
  link-time code gen, static CRT (so no MSVC runtime DLL dependency
  on the user's machine).
- `/GS /sdl` — stack-buffer security checks and additional SDL
  checks.
- `/W4 /EHsc /std:c++17` — high warning level, EH semantics, C++17.
- `/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT` — explicit security
  mitigations.
- **No `/guard:cf`** — Control Flow Guard instruments indirect calls
  in compiled code with a runtime check against a bitmap of legal
  call targets. SECTION 7 calls the original (unhooked) API
  implementations through MinHook-provided trampolines
  (`g_real_GetSystemInfo`, etc.) — function pointers into runtime-
  allocated executable memory MinHook obtains via `VirtualAlloc`.
  Whether those runtime targets are valid under CFG depends on the
  Windows version and on whether MinHook registers them
  (`SetProcessValidCallTargets`); the v1.3.3 release we vendor does
  not. We have not validated `/guard:cf` + our MinHook vintage
  across the Windows versions we support, so enabling CFG could turn
  a working install into a hard crash for some users. The other
  mitigations (DEP, ASLR, /GS stack cookies, /sdl, static CRT) are
  still on.

  Note: `dumpbin /loadconfig dist\dxgi.dll` shows some CFG-related
  load config metadata (e.g. `Guard CF address of check-function
  pointer`, `Guard Flags: CF instrumented`). That metadata comes
  from the static CRT (MSVC ships its CRT objects with CFG
  instrumentation) and is NOT actively enforced — the DLL
  Characteristics field does NOT set the
  `IMAGE_DLLCHARACTERISTICS_GUARD_CF` bit, so the OS loader doesn't
  treat the image as CFG-enabled. Confirm via `dumpbin /headers
  dist\dxgi.dll` → the "DLL characteristics" line will not include
  "Guard CF".

## Packaging a release

```powershell
powershell.exe -ExecutionPolicy Bypass -File package.ps1 -Version 1.1.0
```

Produces, in `release\`:

- `dxmd-thread-fix-v<VERSION>.zip` — the release artifact
- `dxmd-thread-fix-v<VERSION>-SHA256SUMS.txt` — per-file manifest (coreutils format)
- `dxmd-thread-fix-v<VERSION>-headers.txt` — `dumpbin /headers` output
- `dxmd-thread-fix-v<VERSION>-exports.txt` — `dumpbin /exports` output
- `dxmd-thread-fix-v<VERSION>-imports.txt` — `dumpbin /imports` output

The zip's contents:

```
dxgi.dll
README.md
LICENSE
CHANGELOG.md
SHA256SUMS.txt              (same as the standalone file)
docs/                       (full doc set)
third_party/minhook/
    LICENSE.txt
    PROVENANCE.md
```

## Project layout

```
dxmd-thread-fix/
├── src/
│   ├── dxmd_thread_fix.cpp  The entire fix, single translation unit
│   ├── dxgi_stubs.asm       20 tail-jump stubs (one per dxgi export)
│   ├── dxgi.def             Module-def file: exports by name AND ordinal
│   └── version.rc           Embedded VERSIONINFO resource
├── docs/
│   ├── DESIGN.md            Architectural narrative (start here if reading source)
│   ├── TRUST.md             Auditing, imports, what gets written to disk
│   ├── COMPATIBILITY.md     OS / GPU / other-mod compatibility
│   ├── TROUBLESHOOTING.md   AV, MOTW, FIX STATUS: INACTIVE
│   └── BUILDING.md          (this file)
├── third_party/
│   └── minhook/             Vendored MinHook 1.3.3 (BSD-2-Clause)
│       ├── LICENSE.txt
│       └── PROVENANCE.md    Upstream URL, tag, commit hash, API call list
├── build.ps1                One-shot MSVC build script (produces dist/dxgi.dll)
├── package.ps1              Build + assemble the release zip into release/
├── CHANGELOG.md             Release notes
├── LICENSE                  MIT
└── README.md                User-facing summary

(generated; not tracked in git)
├── build/                   Intermediate objects
├── dist/                    Build output (dxgi.dll)
└── release/                 Packaged release artifacts
```
