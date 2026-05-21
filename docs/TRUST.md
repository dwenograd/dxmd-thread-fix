# Trust and auditing

A DLL from the internet that loads into your game and patches Windows
APIs is exactly the kind of thing you should be suspicious of. Here's
what to verify before you decide.

## Reading the source

Everything is in the GitHub repository
(<https://github.com/dwenograd/dxmd-thread-fix>). The C++ source is a
single translation unit, [`src/dxmd_thread_fix.cpp`](../src/dxmd_thread_fix.cpp)
(~620 lines, including comments), plus three non-C++ toolchain
inputs the build needs:

- [`src/dxgi_stubs.asm`](../src/dxgi_stubs.asm) — 20 tail-jump export stubs
- [`src/dxgi.def`](../src/dxgi.def) — exports table (name + ordinal preservation)
- [`src/version.rc`](../src/version.rc) — VERSIONINFO resource

Plus ~76 KB of [MinHook][mh] source for the hook installation
(MinHook is vendored unmodified in `third_party/minhook/` — see
[`PROVENANCE.md`](../third_party/minhook/PROVENANCE.md)).

[mh]: https://github.com/TsudaKageyu/minhook

For the architectural narrative — why the source is shaped the way
it is, why the trap functions exist, why we hook six APIs instead
of one — read [`DESIGN.md`](DESIGN.md).

## What the DLL is *allowed* to do (and what it isn't)

Imports are a useful first check, not a complete proof — a DLL can
dynamically expand its behaviour with `LoadLibrary` + `GetProcAddress`,
so the import table alone doesn't *prove* what the DLL does at runtime.
The full proof is in reading the source (which is small enough to
skim in 20 minutes). But imports are a strong first signal, and ours
imports **only `KERNEL32.dll`**. You can verify this yourself:

```powershell
dumpbin.exe /imports dxgi.dll
```

You'll see APIs from these categories (the full per-release
imported-symbol list is published as
`dxmd-thread-fix-v<VERSION>-imports.txt` on each GitHub release):

| Category | Examples | Why it's needed |
|---|---|---|
| File I/O | `CreateFileW`, `WriteFile`, `FindFirstFileEx*` | Writing `dxmd-thread-fix.log`; CRT runtime support |
| Module loading | `LoadLibraryExW`, `GetProcAddress`, `GetModuleHandleW`, `GetModuleFileNameW`, `GetSystemDirectoryW` | Loading the real `System32\dxgi.dll` (one absolute path; see code) |
| Synchronization | `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`, `DeleteCriticalSection`, `InterlockedCompareExchange`, `Sleep` | Thread-safe log writes and first-hit flags; MinHook back-off |
| Code patching | `VirtualProtect`, `VirtualQuery`, `FlushInstructionCache`, `VirtualAlloc`, `VirtualFree` | Required by MinHook to install API hooks in our process |
| Thread management | `SuspendThread`, `ResumeThread`, `GetThreadContext`, `SetThreadContext`, `OpenThread`, `CreateToolhelp32Snapshot`, `Thread32First`, `Thread32Next` | Required by MinHook to safely patch code under other threads in our process |
| Exception / CRT plumbing | `IsDebuggerPresent`, `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter`, `TerminateProcess`, `RaiseException`, `GetCurrentProcess`, exception/unwind helpers | Brought in by MSVC's static C runtime (`/MT`); not used by our code |
| Heap / string / locale | `HeapAlloc`, `HeapFree`, `GetLastError`, `lstrlen*`, TLS/FLS slot helpers, console I/O, locale/codepage APIs, environment APIs | Used by MinHook and the static CRT plumbing |

The only *dynamic* DLL load our code does is `LoadLibraryExW` of the
absolute system-directory path resolved by `GetSystemDirectoryW()` —
normally `C:\Windows\System32\dxgi.dll` but correctly handles
non-`C:` Windows installs. See
[`src/dxmd_thread_fix.cpp`](../src/dxmd_thread_fix.cpp) SECTION 6.
No other dynamic loads.

What you should **not** see (and won't):

- Anything from `ws2_32`, `wininet`, `winhttp`, `urlmon` (no networking)
- Anything from `advapi32.RegCreateKey*`, `Reg*Value*` (no registry writes)
- Anything from `kernel32.CreateProcess*`, `shell32.ShellExecute*` (no process launching)

## What gets written to disk

The DLL writes exactly one file at runtime, in the same folder as
itself (i.e. inside `<game>\retail\`):

- `dxmd-thread-fix.log` — diagnostic log of the current run. Opened
  (and truncated) once at DLL load, then appended to per line. If an
  old log exists from a previous run, we truncate it; we never delete
  it. You can safely tail it while the game is running.

Nothing else. No `Documents/`, no `%APPDATA%`, no registry writes, no
network. Your saves, your `PSOCache.bin`, your Steam Cloud, your
achievements — all untouched throughout.

## Reproducible-ish build

The full build is one PowerShell script:

```powershell
powershell.exe -ExecutionPolicy Bypass -File build.ps1
```

The output is `dist\dxgi.dll`. The script prints the SHA-256 at the
end. **Caveat:** MSVC embeds timestamps and other linker / toolchain
metadata into the PE, so two builds on two machines won't be
byte-identical even from the same source — that's a known MSVC quirk,
not a Trojan indicator. What you *can* verify identically is:

- The exports table (`dumpbin /exports dxgi.dll`) — exactly 20 exports, exact ordinals matching System32 dxgi.
- The import table (`dumpbin /imports dxgi.dll`) — only kernel32.
- The function signatures of our hook detours (read `src/dxmd_thread_fix.cpp` SECTION 7).

For each official release we publish on the GitHub release page:

- The git commit tag (`vX.Y.Z`).
- The release zip (`dxmd-thread-fix-v<VERSION>.zip`) and its SHA-256,
  plus the SHA-256 of `dxgi.dll` itself.
- A per-file SHA-256 manifest (`dxmd-thread-fix-v<VERSION>-SHA256SUMS.txt`)
  covering every file inside the zip. Strict coreutils format, so you
  can verify with `sha256sum -c SHA256SUMS.txt` on
  Linux/macOS/Git-for-Windows. The same manifest also ships *inside*
  the zip as `SHA256SUMS.txt` so you can verify locally without
  re-downloading.
- The `dumpbin /headers`, `/exports`, and `/imports` output as text
  files (`dxmd-thread-fix-v<VERSION>-headers.txt`, `-exports.txt`,
  `-imports.txt`) so you can inspect the PE characteristics, export
  table, and import surface without needing dumpbin installed.

If a third party publishes a `dxgi.dll` *claiming* to be ours, hash
it against the GitHub release. If it doesn't match — don't run it.

## MinHook provenance

[MinHook][mh] is vendored unmodified in `third_party/minhook/`. See
[`third_party/minhook/PROVENANCE.md`](../third_party/minhook/PROVENANCE.md)
for the upstream URL, the exact tag (`v1.3.3`), the tag's commit hash
(`9fbd087432700d73fc571118d6a9697a36443d88`), and the exhaustive list
of MinHook APIs our code calls.

## What we're *not* claiming

This DLL is **not** code-signed — we can't afford a code-signing
certificate, and self-signing is worse than nothing for trust. That
means Windows SmartScreen may warn you when you download the zip, and
some aggressive antivirus configurations may quarantine the DLL. See
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md#av--smartscreen).
