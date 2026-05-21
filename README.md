# dxmd-thread-fix

> A tiny `dxgi.dll` shim that fixes the long-standing high-core-CPU
> crash in **Deus Ex: Mankind Divided** (build 1.19, the final 2017
> patch). It also fixes the alt-tab crash as a side effect.
>
> Drop two files next to `DXMD.exe`, launch the game. That's it.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](#supported-platforms)
[![Game version: 1.19 (final)](https://img.shields.io/badge/DXMD-1.19%20%28final%29-orange.svg)](#supported-platforms)

---

## Table of contents

- [Should you trust this DLL?](#should-you-trust-this-dll)
- [What it does (and doesn't do)](#what-it-does-and-doesnt-do)
- [Quick install](#quick-install)
- [Verifying the fix is active](#verifying-the-fix-is-active)
- [Uninstall](#uninstall)
- [Configuration](#configuration)
- [Compatibility](#compatibility)
- [Troubleshooting](#troubleshooting)
- [Theory of operation](#theory-of-operation)
- [Building from source](#building-from-source)
- [Project layout](#project-layout)
- [Reporting bugs](#reporting-bugs)
- [License](#license)
- [Acknowledgements](#acknowledgements)

---

## Should you trust this DLL?

A DLL from the internet that loads into your game and patches Windows
APIs is exactly the kind of thing you should be suspicious of. Here's
what to verify before you decide:

### Reading the source

Everything is in the GitHub repository
(<https://github.com/dwenograd/dxmd-thread-fix>). The total size is
~1,300 lines of our own code, plus ~76 KB of [MinHook][mh] source for
the hook installation (MinHook is vendored unmodified in
`third_party/minhook/` — see
[`PROVENANCE.md`](third_party/minhook/PROVENANCE.md)). The files that
matter:

> **Note for release-zip users:** the binary release zip you download
> contains only the runtime files you need (DLL, INI, scripts, docs,
> license attribution). The full source is in the GitHub repo above and
> in GitHub's auto-generated source archives for each tag. The source
> file links below point at the repo.

- [`src/dllmain.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dllmain.cpp) — entry point, ~200 lines
- [`src/dtf_traps.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dtf_traps.cpp) — pre-resolve trap functions, ~110 lines
- [`src/dxgi_exports.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dxgi_exports.cpp) — proxy plumbing, ~100 lines
- [`src/dxgi_stubs.asm`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dxgi_stubs.asm) — 20 tail-jump stubs
- [`src/cpu_hooks.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/cpu_hooks.cpp) — the actual API hooks, ~300 lines
- [`src/topology.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/topology.cpp) — fake CPU topology state, ~85 lines
- [`src/config.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/config.cpp), [`src/log.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/log.cpp) — config & log

[mh]: https://github.com/TsudaKageyu/minhook

### What the DLL is *allowed* to do (and what it isn't)

Imports are a useful first check, not a complete proof — a DLL can
dynamically expand its behavior with `LoadLibrary` + `GetProcAddress`,
so the import table alone doesn't *prove* what the DLL does at runtime.
The full proof is in reading the source (which is small enough to skim
in 20 minutes). But imports are a strong first signal, and ours
imports **only `KERNEL32.dll`**. You can verify this yourself:

```powershell
dumpbin.exe /imports dxgi.dll
```

You'll see APIs from these categories (this is a summary; the
complete imported-symbol list is in `dumpbin /imports dxgi.dll` and
the per-release `dxmd-thread-fix-v<VERSION>-imports.txt` published
alongside the zip on the GitHub release page):

| Category | Examples | Why it's needed |
|---|---|---|
| File I/O | `CreateFileW`, `WriteFile`, `GetPrivateProfileIntW`, `FindFirstFileEx*` | Reading `dxmd-thread-fix.ini`, writing `dxmd-thread-fix.log`; CRT runtime support |
| Module loading | `LoadLibraryExW`, `GetProcAddress`, `GetModuleHandleW`, `GetModuleFileNameW`, `GetSystemDirectoryW` | Loading the real `System32\dxgi.dll` (one absolute path; see code) |
| Synchronization | `InitializeCriticalSection`, `EnterCriticalSection`, `LeaveCriticalSection`, `DeleteCriticalSection`, `InterlockedCompareExchange`, `Sleep` | Thread-safe log writes and first-hit flags; MinHook back-off |
| Code patching | `VirtualProtect`, `VirtualQuery`, `FlushInstructionCache`, `VirtualAlloc`, `VirtualFree` | Required by MinHook to install API hooks in our process |
| Thread management | `SuspendThread`, `ResumeThread`, `GetThreadContext`, `SetThreadContext`, `OpenThread`, `CreateToolhelp32Snapshot`, `Thread32First`, `Thread32Next` | Required by MinHook to safely patch code under other threads in our process |
| Exception/CRT plumbing | `IsDebuggerPresent`, `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter`, `TerminateProcess`, `RaiseException`, `GetCurrentProcess`, exception/unwind helpers | Brought in by MSVC's static C runtime (`/MT`); not used by our code |
| Heap/string/locale/console | `HeapAlloc`, `HeapFree`, `GetLastError`, `lstrlen*`, TLS/FLS slot helpers, console I/O, locale/codepage APIs, environment APIs | Used by MinHook and the static CRT plumbing |

The only *dynamic* DLL load our code does is `LoadLibraryExW` of the
absolute system-directory path resolved by `GetSystemDirectoryW()` —
normally `C:\Windows\System32\dxgi.dll` but correctly handles non-`C:`
Windows installs. See [`src/dxgi_exports.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dxgi_exports.cpp).
No other dynamic loads.

What you should **not** see (and won't):

- Anything from `ws2_32`, `wininet`, `winhttp`, `urlmon` (no networking)
- Anything from `advapi32.RegCreateKey*`, `Reg*Value*` (no registry writes)
- Anything from `kernel32.CreateProcess*`, `shell32.ShellExecute*` (no process launching)

### What gets written to disk

**By the DLL at runtime (in `<game>\retail\`):**
- `dxmd-thread-fix.log` — diagnostic log of the current run, opened (and
  truncated) once at DLL load, then appended to. **Not created at all
  if `LogLevel=0`.** If an old log exists from a previous run, we
  truncate it; we never delete it.

**By `install.ps1`:**
- `dxmd-thread-fix.ini` — copied from this repo into `retail\` if
  absent; never overwritten if you've customized it.
- `dxgi.dll` — copied into `retail\`.
- `dxgi.dll.bak-YYYYMMDD-HHMMSS-fff-<4 hex>` — created only if you pass `-Force`
  AND `retail\dxgi.dll` already exists AND it isn't ours AND there
  isn't an existing backup. Saves the prior (foreign-mod) DLL so
  `uninstall.ps1` can restore it. The millisecond + random suffix
  guards against name collision under concurrent install runs.
- `._dtf_writetest_<guid>.tmp` — a transient small file (4 bytes,
  the literal string `test`) written and immediately deleted to test
  that `retail\` is writable.

**By `uninstall.ps1`:**
- Removes the three files install.ps1 created (the DLL, INI, and log)
  if they're recognizably ours.
- Restores the oldest `dxgi.dll.bak-*` (the original pre-DTF state)
  back to `dxgi.dll`, then deletes all backups.

Nothing else. No `Documents/`, no `%APPDATA%`, no registry writes, no
network. Your saves, your `PSOCache.bin`, your Steam Cloud, your
achievements — all untouched throughout.

`install.ps1` and `uninstall.ps1` **read** Steam's registry install
path (`HKCU:\Software\Valve\Steam\SteamPath` and the WOW6432 LM hive)
for auto-detection of the game folder. They write nothing to the
registry.

### Reproducible-ish build

The full build is one PowerShell script:

```powershell
powershell.exe -ExecutionPolicy Bypass -File build.ps1
```

The output is `dist\dxgi.dll`. The script prints the SHA-256 at the
end. **Caveat:** MSVC embeds timestamps, GUIDs, and absolute paths
into the PE, so two builds on two machines won't be byte-identical
even from the same source — that's a known MSVC quirk, not a Trojan
indicator. What you *can* verify identically is:

- The exports table (`dumpbin /exports dxgi.dll`) — exactly 20 exports, exact ordinals matching System32 dxgi.
- The import table (`dumpbin /imports dxgi.dll`) — only kernel32.
- The function signatures of our hook detours (read `src/cpu_hooks.cpp`).

For each official release we publish on the GitHub release page:

- The git commit tag (`vX.Y.Z`).
- The release zip (`dxmd-thread-fix-v<VERSION>.zip`) and its SHA-256,
  plus the SHA-256 of `dxgi.dll` itself.
- A per-file SHA-256 manifest (`dxmd-thread-fix-v<VERSION>-SHA256SUMS.txt`)
  covering every non-manifest file inside the zip. The manifest is
  strict coreutils format, so you can verify with `sha256sum -c
  SHA256SUMS.txt` on Linux/macOS/Git-for-Windows. This same manifest
  also ships *inside* the zip as `SHA256SUMS.txt` so you can verify
  locally without re-downloading.
- The `dumpbin /headers`, `/exports`, and `/imports` output as text
  files (`dxmd-thread-fix-v<VERSION>-headers.txt`, `-exports.txt`,
  `-imports.txt`) so you can inspect the PE characteristics, export
  table, and import surface without needing dumpbin installed.

If a third party publishes a `dxgi.dll` *claiming* to be ours, hash it
against the GitHub release. If it doesn't match — don't run it.

### MinHook provenance

[MinHook][mh] is vendored unmodified in `third_party/minhook/`. See
[`third_party/minhook/PROVENANCE.md`](third_party/minhook/PROVENANCE.md)
for the upstream URL, the exact tag (`v1.3.3`), the tag's commit hash
(`9fbd087432700d73fc571118d6a9697a36443d88`), and the exhaustive list
of MinHook APIs our code calls.

### What we're *not* claiming

This DLL is **not** code-signed — we can't afford a code-signing
certificate, and self-signing is worse than nothing for trust. That
means Windows SmartScreen may warn you when you download the zip, and
some aggressive antivirus configurations may quarantine the DLL. See
[Troubleshooting → AV / SmartScreen](#av--smartscreen).

---

## What it does (and doesn't do)

### What it fixes

DXMD 1.19 (the final 2017 patch) crashes on CPUs with high logical
processor counts. The threshold is somewhere in the high teens. On a
32C/64T Threadripper (or anything bigger), DXMD typically crashes
within seconds — often before the title screen, often during the
first zone load, sometimes when you alt-tab. The bug is one of the
classic "this game was built before high-core CPUs were common"
failure modes.

This shim makes DXMD see a sane logical-processor count (default: 8)
regardless of your actual CPU. It does so by intercepting the six
Win32 APIs the game and its middleware (`bink2w64.dll`, `amd_ags64.dll`)
use to discover the CPU topology, and rewriting the answers to be
consistent with a smaller, single-group CPU.

The game then sizes its thread pool sanely, the crash stops happening,
and (as a side effect) the alt-tab crash also goes away — because
that's a downstream symptom of the same thread-pool sizing issue.

### What it does NOT fix

DXMD has at least one other well-known crash bug — **rapid menu
interaction** (inventory, looting, etc. clicked in fast sequence)
will eventually crash the game. **That bug is not addressed by this
mod.** It's a separate issue with a different root cause, and is
explicitly out of scope here.

This shim also doesn't:

- Improve graphics quality, add DLSS/FSR, fix HDR, etc. (See [Compatibility](#compatibility) — you can stack a graphics mod on top.)
- Modify the game's behaviour in any way other than what's described above.
- Touch saves, achievements, multiplayer, or DRM.
- Connect to the internet.

### What about other games?

This shim has been built and tested for DXMD only. The CPU-detection
crash pattern affects other games too (Dishonored 2, some Frostbite
titles, etc.), but I haven't tested or documented this fix against
them. **The DLL has a host-process check and will refuse to install
CPU hooks unless it's loaded inside `DXMD.exe`.** That's deliberate —
it prevents accidental activation in unrelated apps.

---

## Quick install

Three steps:

1. Download the latest release from
   [Releases](https://github.com/dwenograd/dxmd-thread-fix/releases)
   (the zip contains `dxgi.dll` and `dxmd-thread-fix.ini`; the release
   page lists the SHA-256 of the DLL).
2. **Right-click the zip → Properties → check "Unblock"** before
   extracting, otherwise Windows may block the extracted files
   (Mark-of-the-Web).
3. Drop `dxgi.dll` and `dxmd-thread-fix.ini` into the game's `retail\`
   subfolder, next to `DXMD.exe`. Typical Steam paths:
   - `C:\Program Files (x86)\Steam\steamapps\common\Deus Ex Mankind Divided\retail\`
   - `D:\Steam\steamapps\common\Deus Ex Mankind Divided\retail\`

That's it. Launch DXMD through Steam normally. Verify it engaged
([next section](#verifying-the-fix-is-active)).

### Scripted install (optional)

If you extracted the release zip or cloned the source repo, `install.ps1`
does it for you and can auto-detect the Steam install location. Open
PowerShell, `cd` into the folder where you extracted the zip (or where
you cloned the source repo), then run one of:

```powershell
# Stock Windows (PowerShell 5.1 ships with Windows):
cd path\to\extracted\dxmd-thread-fix
powershell.exe -ExecutionPolicy Bypass -File install.ps1
# Or with explicit game path:
powershell.exe -ExecutionPolicy Bypass -File install.ps1 -Game "C:\Games\Deus Ex Mankind Divided"

# Or, if you have PowerShell 7 installed:
pwsh -File install.ps1
```

The script:
- Recognizes an existing `dxgi.dll` as "ours" via its embedded
  VERSIONINFO ProductName (so upgrades between DTF versions are silent).
- Refuses to overwrite an existing `dxgi.dll` from a different mod
  unless you pass `-Force` (in which case it backs the prior one up
  ONCE — never overwriting an existing backup, so the original
  pre-DTF state stays preserved).
- Refuses to install if DXMD is currently running.
- Refuses to install if it can't write to `retail\` (e.g. game in
  Program Files without admin).
- Preserves your `dxmd-thread-fix.ini` if you've already customized it.

---

## Verifying the fix is active

After launching DXMD, check the log file:

```
<game folder>\retail\dxmd-thread-fix.log
```

(e.g. `C:\Program Files (x86)\Steam\steamapps\common\Deus Ex Mankind Divided\retail\dxmd-thread-fix.log`)

A working install looks like this:

```
[20:30:01.123] dxmd-thread-fix attach: loaded at module handle 00007FFD32100000
[20:30:01.124] config: LogicalProcessors=8 ClampAffinity=0 LogLevel=1
[20:30:01.124] Host process: <your Steam library>\steamapps\common\Deus Ex Mankind Divided\retail\DXMD.exe
[20:30:01.127] Real dxgi loaded: C:\WINDOWS\system32\dxgi.dll @ 00007FFD4CE00000
[20:30:01.127] dxgi exports resolved: 20 ok, 0 missing.
[20:30:01.127] Fake topology: 8 logical processors, 1 group(s)  (real: 64 in 1 group(s))
[20:30:01.180] CPU hook install complete: 6 hooks active (6 required, 0 optional).
[20:30:01.180]   Hooked GetSystemInfo @ 00007FFD514BD740 [required]
[20:30:01.180]   Hooked GetNativeSystemInfo @ 00007FFD514BFB20 [required]
... more "Hooked X" lines ...
[20:30:01.180] ============================================================
[20:30:01.180] FIX STATUS: ACTIVE  (reporting 8 logical processors to game)
[20:30:01.180] ============================================================
[20:30:01.191] GetSystemInfo: 64 -> 8
```

The line that matters: **`FIX STATUS: ACTIVE`**. The follow-up
`GetSystemInfo: 64 -> 8` line (or similar — your "real" number depends
on your CPU) shows the game actually called into our hook.

If you see `FIX STATUS: INACTIVE` or a `FATAL:` line, the fix didn't
install. Skip to [Troubleshooting](#troubleshooting).

If you don't see a log file at all, see
[Troubleshooting → No log file at all](#no-log-file-at-all).

---

## Uninstall

If you used `install.ps1`:

```powershell
powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1
# Or with PowerShell 7:
pwsh -File uninstall.ps1
```

It will:
- Identify `retail\dxgi.dll` as "ours" via its embedded VERSIONINFO
  ProductName (`dxmd-thread-fix`). If it's not ours, the script
  refuses to delete it — you'd have to pass `-DeleteForeignDxgi` to
  explicitly override (don't unless you know what you're doing).
- Restore the OLDEST backup (the original pre-DTF state — e.g., the
  ReShade `dxgi.dll` that was there before you installed DTF), then
  clean up all backup files.
- Remove `dxmd-thread-fix.ini` and `dxmd-thread-fix.log`.

If you installed manually, just delete:
- `<game>\retail\dxgi.dll` (only if it's ours; check with
  `(Get-Item dxgi.dll).VersionInfo.ProductName` — should say "dxmd-thread-fix")
- `<game>\retail\dxmd-thread-fix.ini`
- `<game>\retail\dxmd-thread-fix.log`

Your saves, shader cache, and game files are untouched throughout.

---

## Configuration

Edit `dxmd-thread-fix.ini` next to `dxgi.dll`. The defaults work for
almost everyone.

```ini
[ThreadFix]
LogicalProcessors=8     ; how many CPUs to report to the game (1-64)
ClampAffinity=0         ; 1 = also rewrite SetThreadAffinityMask calls
LogLevel=1              ; 0=silent (no log), 1=normal, 2=verbose
```

- **`LogicalProcessors`** — community-validated sweet spot is `8`. Some
  users report stability at `12`. The hard upper bound is `64` (a
  Windows processor-group ceiling); higher values get silently clamped.
  If your actual CPU has fewer logical processors than this, we report
  your real count (we never lie *upward*).

- **`ClampAffinity`** — leave at `0` (the default) unless you're
  specifically troubleshooting affinity-related crashes. When set to
  `1`, we additionally install a hook on `SetThreadAffinityMask` and
  clamp any requested affinity mask into the first
  `LogicalProcessors` bits, logging any clamping that happens. The
  default `0` value does NOT install this hook at all, so the log
  won't contain `SetThreadAffinityMask` lines unless you enable it.

- **LogLevel** — `0` disables logging entirely: no log file is created
  or updated for the run. An *old* log file from a previous run is
  not deleted (we don't touch it at all). `1` is the default and writes
  one line per hooked API plus the startup banner. `2` writes a line
  for every hooked call (very noisy; only useful when filing a bug).

---

## Compatibility

### Confirmed working

- **Steam version of DXMD** (the only place this has been tested).
- **Steam overlay** — fine. Steam hooks the D3D swapchain VTable after
  swapchain creation, which is orthogonal to our static-import proxy.
- **NVIDIA / AMD GPUs** — both. No driver-specific behavior.
- **Windows 10, Windows 11** — both.
- **Threadripper 3970X (32C/64T)** — primary test platform, fix
  developed against this hardware.

### Expected to work (untested)

- **Steam achievements** — we don't touch any Steam APIs, don't modify
  save data, don't intercept achievement-relevant code paths. Expected
  compatible, but not specifically verified.
- **Smaller CPUs (8C/16T, 12C/24T)** — should be a harmless no-op
  (we clamp our reported count downward to your real count).
- **Larger CPUs (96C/192T+)** — the topology hooks return a
  single-group, 8-CPU view, so should work.

### Known conflicts

| Other mod | Conflict | Workaround |
|---|---|---|
| ReShade (configured as `dxgi.dll` proxy) | Both want to be `dxgi.dll`. | Configure ReShade as `d3d11.dll` proxy instead. |
| Special K | Both proxy `dxgi.dll`. Special K's HDR retrofit specifically requires its dxgi proxy. | No good workaround currently — use one or the other. A future v1.1 may add a `winmm.dll` alt-proxy mode. |
| ENBSeries | Both proxy `dxgi.dll`. | Use only one. |

### Not supported

- **Microsoft Store / Xbox Game Pass** install of DXMD — the app
  sandbox blocks `dxgi.dll` proxying.
- **Pirated copies** — we can't help if your install is itself broken
  by a crack patcher.
- **DXMD demo / older patches** — only build `1.19.801.0` (the final
  2017 patch) has been tested. Earlier patches may differ in import
  table layout. The DLL itself does NOT runtime-check the DXMD build
  version, so it'll still try to install on other builds; the only
  enforced check is that the host EXE is named `DXMD.exe`.

### Supported platforms

| | Status |
|---|---|
| DXMD build | Tested on 1.19.801.0 (final 2017 patch). The DLL does NOT enforce this at runtime; other builds may work but are untested. |
| OS | Windows 10, Windows 11 — tested. Windows 7 SP1 / 8.1 — theoretically supported (the build targets Win7 SP1 minimum) but untested. |
| Architecture | x64 — the only one DXMD ships in |
| CPU | Any x86-64 CPU. The fix is most useful at 16+ logical processors but is harmless on smaller CPUs. |

---

## Troubleshooting

### No log file at all

If `dxmd-thread-fix.log` doesn't appear in `retail\` after launching
the game, our DLL didn't load (or didn't get far enough to open the
log). Most common causes:

1. **`LogLevel=0`** in the INI — we never create a log file in silent
   mode. (If you set it to 0 *after* having had logging on, an old
   log file may remain from a previous run; we don't delete it,
   but we won't update it either.) Change to `LogLevel=1` and relaunch.
2. **DLL not in `retail\`** — you put it in the wrong folder.
   `dxgi.dll` must be next to `DXMD.exe`, not next to the launcher or
   in the parent `Deus Ex Mankind Divided\` folder.
3. **DLL blocked by antivirus** — see next section.
4. **DLL blocked by Mark-of-the-Web** — see below.

### AV / SmartScreen blocked the DLL

Windows Defender, Norton, McAfee, Smart App Control on Windows 11, and
other security tools sometimes block or quarantine unsigned DLLs that
hook Windows APIs. Symptoms:

- DLL silently disappears from `retail\` after a few minutes.
- Or: game runs but the log file doesn't appear.
- Or: an AV notification.

Fixes (in order of preference):

1. **Verify the SHA-256** of the released `dxgi.dll` against the hash
   on the GitHub release page. If it matches, you have the genuine
   article — your AV is a false positive.
2. **Add an exception** for the `retail\` folder in your AV.
3. **For Smart App Control on Windows 11**: open Windows Security →
   App & browser control → Smart App Control → Off. (You can turn it
   back on after testing, then re-allow the DLL specifically.)

### Mark-of-the-Web blocks the PowerShell scripts

If you downloaded the zip from a browser, Windows tags it as "from the
internet". PowerShell may refuse to run the install/uninstall scripts.

Fix: right-click the zip → Properties → check "Unblock" → Apply →
*then* extract. Or run the scripts with explicit policy bypass:

```powershell
powershell.exe -ExecutionPolicy Bypass -File install.ps1
```

### `FIX STATUS: INACTIVE` in the log

The DLL loaded but couldn't install at least one of the 6 required
CPU-topology hooks. Possible causes:

- Another tool (AppVerifier, ETW instrumentation, an AV runtime
  shim) has already instrumented the same `kernel32` exports in a way
  MinHook can't patch.
- An unusual Windows version doesn't export one of the topology APIs
  we need. (Should be impossible on Win10+, but possible on heavily
  patched/custom systems.)

The log will name the specific API that failed. Please [report it](#reporting-bugs)
with the full log attached.

### `FATAL: real dxgi could not be loaded`

Our DLL loaded, but it couldn't open the real `C:\Windows\System32\dxgi.dll`
to forward to. Causes:

- AV blocking System32 dxgi loads (rare; major OS breakage).
- Wrong Windows version (very old / Wine / partial-OS environments).
- Disk error.

The DLL deliberately returns `FALSE` from its entry point in this case,
so the game shows a clean "DLL load failed" rather than crashing later.

### Game launches normally but my crash still happens

If the crash you're seeing is during **rapid menu interaction**
(inventory, looting), this mod does not fix that bug. See
[What it does NOT fix](#what-it-does-not-fix).

If the crash happens during something else (combat, save/load, zone
transition), please [report it](#reporting-bugs) with:
- Full `dxmd-thread-fix.log`
- Steam crash dump from `%LOCALAPPDATA%\CrashDumps\DXMD.exe.*.dmp`
- Your CPU model and Windows version
- What you were doing when it crashed
- Whether it crashes consistently or intermittently

### Steam "Verify integrity of game files"

Steam Verify ignores extra files in the game folder (it only checks
files Steam itself tracks). It will neither remove our `dxgi.dll`/INI/log
nor restore anything in their place. Safe to run with our shim installed.

### Game update through Steam

DXMD hasn't been updated since 2017, so this is unlikely. If it ever
happens and Steam adds its own `dxgi.dll` to `retail\`, ours will be
overwritten — just reinstall the mod.

---

## Theory of operation

### The bug we're fixing

`dumpbin /imports DXMD.exe` shows that the game calls
`kernel32!GetSystemInfo` directly. That API returns a `SYSTEM_INFO`
struct whose `dwNumberOfProcessors` field tells callers how many
logical processors the system has. DXMD uses that number to size its
internal worker-thread pool.

On a 32C/64T CPU, `dwNumberOfProcessors == 64`. DXMD (and its
middleware: `bink2w64.dll` and `amd_ags64.dll`, both of which also
import `GetSystemInfo`) spawn thread pools sized for 64 workers, and
something in that pipeline — most likely a fixed-size internal buffer
indexed by CPU index — overflows or races. The result is a crash, often
within seconds of launch.

The reason existing community workarounds (Process Lasso, `start /affinity`,
BES, Task Manager affinity) don't reliably fix this: they constrain
*where* the 64 threads can run, but the game still *spawns 64 threads*.
If the bug is in the spawning/sizing path (which the symptoms strongly
suggest), affinity workarounds don't help.

### The fix

Hook the CPU-detection APIs and lie about the count.

The challenge is **getting loaded into the game process early enough
that our hooks are live before the game calls `GetSystemInfo`**. DXMD
statically imports `dxgi.dll`, which means the Windows loader resolves
`dxgi.dll` (using standard DLL search order) before running DXMD's
entry point. By placing our `dxgi.dll` in DXMD's own directory
(`retail\`), we get loaded first — earlier than any of DXMD's own
threads or worker code runs.

This requires us to be a faithful drop-in replacement for `dxgi.dll`:
the loader will resolve all 20 of DXMD's dxgi imports through us, and
we have to forward each one correctly to the real `C:\Windows\System32\dxgi.dll`.
That's the `src/dxgi_stubs.asm` file — 20 single-instruction tail jumps,
one per dxgi export, each routed through a pointer that our DllMain
overwrites with the real System32 dxgi address.

Once our DllMain runs, we use [MinHook][mh] to install inline detours
on the 6 CPU-discovery APIs (`GetSystemInfo`, `GetNativeSystemInfo`,
`GetActiveProcessorCount`, `GetMaximumProcessorCount`,
`GetActiveProcessorGroupCount`, `GetMaximumProcessorGroupCount`). When
DXMD or its middleware calls any of them, our detour runs first and
returns numbers consistent with a fake "8 logical processors, 1 group"
topology.

### The subtle bit

The Windows loader's app-compatibility shim layer (`apphelp.dll`)
runs a compat-fixup pass on every loaded DLL **before** running the
DLL's entry point. For dxgi specifically, that pass calls some of
dxgi's compat exports (notably `SetAppCompatStringPointer`) to apply
OS-level fixups.

If our dxgi export stubs jumped through null pointers at that point,
the process would crash inside `apphelp.dll` before our DllMain ever
got a chance to run. We've solved this by initializing every stub's
target pointer at compile time to a no-op trap function defined in
`src/dtf_traps.cpp` — so the stubs always land on valid code. Apphelp
asks "did you handle this compat string?", our trap returns 0 ("yes,
all good"), and the loader proceeds to run our DllMain, which
overwrites the trap pointers with the real System32 dxgi addresses.

This is exactly the issue the first attempt at this mod tripped on,
and is documented at length in [`src/dtf_traps.cpp`](https://github.com/dwenograd/dxmd-thread-fix/blob/v1.0.0/src/dtf_traps.cpp).

### Why we hook 6 APIs, not just `GetSystemInfo`

Even though DXMD itself only imports `GetSystemInfo`, middleware
(`bink2w64.dll`, `amd_ags64.dll`, possibly Apex/PhysX) may dynamically
resolve other CPU-discovery APIs through `GetProcAddress`. Hooking
all 6 with a consistent fake topology means everyone in the process
gets the same lie — there's no code path that sees 8 from one API
and 64 from another.

The 6 hooks are treated as an **all-or-nothing required set**: if any
of them fails to install, we tear down the others and log
`FIX STATUS: INACTIVE` so the user can see clearly that the fix isn't
applied (rather than running the game with a half-applied fix that
might still crash).

---

## Building from source

### Prerequisites

- **Windows 10 or 11** (64-bit)
- **Visual Studio 2022** (Community is fine), with the
  **"Desktop development with C++"** workload installed. This provides
  the toolchain we need: `cl.exe`, `link.exe`, `ml64.exe`, `rc.exe`.
  Other compilers (GCC, Clang) are not supported.
- **PowerShell 5.1 or 7+** (5.1 ships with Windows).

### Build

```powershell
git clone https://github.com/dwenograd/dxmd-thread-fix.git
cd dxmd-thread-fix
powershell.exe -ExecutionPolicy Bypass -File build.ps1
# Or, if you have PowerShell 7 installed:
pwsh -File build.ps1
```

Output:
- `dist\dxgi.dll` — ~150 KB. The artifact you'd install or distribute.

(The Release build currently does NOT emit a PDB. If you want symbols
for debugging, edit `build.ps1` to add `/Zi` to the cl flags and
`/DEBUG` to the link flags.)

The script prints the SHA-256 of the built DLL at the end. Self-builds
are NOT expected to match the SHA-256 of official releases byte-for-byte —
MSVC embeds timestamps, GUIDs, and absolute paths into PE files that
differ between machines. What you CAN verify identically is:

- The export table (`dumpbin /exports dist\dxgi.dll`) — exactly 20
  names + ordinals matching `C:\Windows\System32\dxgi.dll`.
- The import table (`dumpbin /imports dist\dxgi.dll`) — only
  `KERNEL32.dll`.
- The embedded VERSIONINFO `ProductName` (`(Get-Item dist\dxgi.dll).VersionInfo.ProductName`)
  should be `dxmd-thread-fix`.

### Build flags

The Release build uses:

- `/O2 /Oi /GL /MT` — optimize for speed, enable intrinsics, link-time
  code gen, static CRT (so no MSVC runtime DLL dependency on the user's machine).
- `/GS /sdl` — stack-buffer security checks and additional SDL checks.
- `/W4 /EHsc /std:c++17` — high warning level, EH semantics, C++17.
- `/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT` — explicit security mitigations.
- **No `/guard:cf`** — Control Flow Guard is incompatible with MinHook's
  runtime code patching (CFG would reject the patched function pointers).

### Install (for testing)

```powershell
powershell.exe -ExecutionPolicy Bypass -File install.ps1
# Or with PowerShell 7:
pwsh -File install.ps1
```

### Uninstall

```powershell
powershell.exe -ExecutionPolicy Bypass -File uninstall.ps1
# Or with PowerShell 7:
pwsh -File uninstall.ps1
```

---

## Project layout

```
dxmd-thread-fix/
├── src/
│   ├── dllmain.cpp           Entry point + attach/detach sequence
│   ├── dtf_traps.cpp         Pre-resolve trap functions for the dxgi stubs
│   ├── dxgi_exports.cpp      pfn_FOO globals + load_system_dxgi_and_resolve()
│   ├── dxgi_exports.h
│   ├── dxgi_stubs.asm        20 tail-jump stubs (one per dxgi export)
│   ├── dxgi.def              Module-def file: exports by name AND ordinal
│   ├── cpu_hooks.cpp         MinHook detour install + the 6 hook bodies
│   ├── cpu_hooks.h
│   ├── topology.cpp          Single source of truth for the fake topology
│   ├── topology.h
│   ├── config.cpp            INI parsing via GetPrivateProfileIntW
│   ├── config.h
│   ├── log.cpp               Thread-safe append-only file logger
│   ├── log.h
│   └── version.rc            Embedded VERSIONINFO resource
├── third_party/
│   └── minhook/              Vendored MinHook 1.3.3 (BSD-2-Clause)
│       └── PROVENANCE.md     Upstream URL, tag, commit hash, API call list
├── build.ps1                 One-shot MSVC build script (produces dist/dxgi.dll)
├── package.ps1               Build + assemble the release zip into release/
├── install.ps1               Copies dxgi.dll + ini into game's retail\
├── uninstall.ps1             Reverses install
├── dxmd-thread-fix.ini       Default config
├── CHANGELOG.md              Release notes
├── LICENSE                   MIT
└── README.md                 This file

(generated; not tracked in git)
├── build/                    Intermediate objects
├── dist/                     Build output (dxgi.dll)
└── release/                  Packaged release artifacts (zip + manifests)
```

---

## Reporting bugs

GitHub Issues are the right place. Please include:

- Your CPU model (especially logical processor count)
- Your Windows version (run `winver` → first line)
- Your DXMD install source (Steam / Epic / other)
- The full contents of `dxmd-thread-fix.log`
- If the game crashed: the contents of `%LOCALAPPDATA%\CrashDumps\DXMD.exe.<pid>.dmp`
  (the file may be several MB; please upload it somewhere and link rather
  than pasting)
- Which other mods (if any) you have installed
- What you were doing when the crash happened
- Whether it crashes consistently or intermittently

If the issue is "the game crashes when I rapidly interact with menus" —
that's the menu-interaction bug, which is not addressed by this mod.
See [What it does NOT fix](#what-it-does-not-fix).

---

## License

[MIT](LICENSE).

This project vendors MinHook (BSD-2-Clause). See
[`third_party/minhook/LICENSE.txt`](third_party/minhook/LICENSE.txt).

---

## Acknowledgements

- **[Tsuda Kageyu](https://github.com/TsudaKageyu)** for [MinHook][mh] —
  this fix would have been at least 10× more code without it.
- The **Steam Community, /r/Deusex, and PCGamingWiki** threads that
  have been trading affinity workarounds for this crash since 2017.
- **Eidos Montréal** for one of the best immersive sims ever made,
  even if they shipped it with this bug. DXMD hasn't seen an update
  since 2017, which is why this fix exists.
