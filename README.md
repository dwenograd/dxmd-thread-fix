# dxmd-thread-fix

> A tiny `dxgi.dll` shim that fixes the long-standing high-core-CPU
> crash in **Deus Ex: Mankind Divided** (build 1.19, the final 2017
> patch). It also fixes the alt-tab crash as a side effect.
>
> Drop one file next to `DXMD.exe`, launch the game. That's it.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](docs/COMPATIBILITY.md)
[![Game version: 1.19 (final)](https://img.shields.io/badge/DXMD-1.19%20%28final%29-orange.svg)](docs/COMPATIBILITY.md)

---

## Up front: this is an AI-generated fix

This is an AI-generated fix for one of the biggest pain points of
running DXMD on a modern system. The entire patch, end to end, is
AI-generated — well above what I could write by hand. After 10 years
of everyone living with this bug, this seemed worth putting together
as a final fix for getting the game playable forever.

Trusting a random DLL off the internet is sketchy. The source is one
~620-line C++ file ([`src/dxmd_thread_fix.cpp`](src/dxmd_thread_fix.cpp))
plus an assembly file, a module-def file, and a version resource. See
[`docs/TRUST.md`](docs/TRUST.md) for auditing notes, what the DLL is
and isn't allowed to do, and how to verify the binary against its
source.

---

## Quick install

1. Download the latest `dxmd-thread-fix-v<VERSION>.zip` from
   [Releases](https://github.com/dwenograd/dxmd-thread-fix/releases).
2. Right-click the zip → **Properties** → check **Unblock** → Apply
   (otherwise Windows may block the extracted DLL via
   Mark-of-the-Web).
3. Extract, and drop `dxgi.dll` into the game's `retail\` subfolder,
   next to `DXMD.exe`. Typical Steam paths:
   - `C:\Program Files (x86)\Steam\steamapps\common\Deus Ex Mankind Divided\retail\`
   - `D:\Steam\steamapps\common\Deus Ex Mankind Divided\retail\`

Launch DXMD through Steam normally.

## Verifying the fix is active

After launching DXMD, look for the log file:

```
<game>\retail\dxmd-thread-fix.log
```

A working install contains a line that says:

```
FIX STATUS: ACTIVE  (reporting 8 logical processors to game)
```

If you see `FIX STATUS: INACTIVE`, `FATAL:`, or no log file at all,
see [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md).

## Uninstall

Delete `<game>\retail\dxgi.dll` and `<game>\retail\dxmd-thread-fix.log`.
That's it.

(If you previously installed v1.0.0, you can also delete the leftover
`dxmd-thread-fix.ini` — v1.1.0 ignores it.)

---

## What it fixes (and doesn't)

**Fixes:** the crash on CPUs with high logical-processor counts. The
threshold is somewhere in the high teens; on a 32C/64T Threadripper
DXMD typically crashes within seconds. The shim tells the game it's
running on an 8-CPU machine regardless of your actual CPU, by
intercepting the six Win32 APIs the game and its middleware
(`bink2w64.dll`, `amd_ags64.dll`) use to discover CPU topology. The
alt-tab crash goes away as a side effect.

**Does NOT fix:** the **rapid menu interaction** crash (inventory /
looting clicked too fast). Different bug, different root cause, out
of scope.

**Other games:** the DLL refuses to install its CPU hooks unless
loaded inside `DXMD.exe`. Even if you renamed the host EXE, this
shim is tuned for DXMD only and has not been validated against other
titles.

---

## More detail

- **[`docs/DESIGN.md`](docs/DESIGN.md)** — architectural narrative. How
  the proxy works, why the trap functions exist, why six API hooks,
  why a three-pass MinHook install. Start here if you're reading the
  source.
- **[`docs/TRUST.md`](docs/TRUST.md)** — auditing, imports, what gets
  written to disk, MinHook provenance, reproducible-ish build.
- **[`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md)** — confirmed
  working configurations, known conflicts (other dxgi.dll-proxying
  mods), and what's not supported.
- **[`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)** — AV /
  SmartScreen, Mark-of-the-Web, `FIX STATUS: INACTIVE`, `FATAL:`
  diagnoses.
- **[`docs/BUILDING.md`](docs/BUILDING.md)** — build from source,
  customize the hardcoded defaults, project layout, packaging a
  release.
- **[`CHANGELOG.md`](CHANGELOG.md)** — version history.

## Reporting bugs

GitHub Issues are the right place. Please include:

- Your CPU model (especially logical processor count)
- Your Windows version (run `winver` → first line)
- Your DXMD install source (Steam / Epic / other)
- The full contents of `dxmd-thread-fix.log`
- If the game crashed: the contents of
  `%LOCALAPPDATA%\CrashDumps\DXMD.exe.<pid>.dmp` (the file may be
  several MB; please upload it somewhere and link rather than
  pasting)
- Which other mods (if any) you have installed
- What you were doing when the crash happened
- Whether it crashes consistently or intermittently

If the issue is "the game crashes when I rapidly interact with menus"
— that's the menu-interaction bug, which is not addressed by this
mod.

## License

[MIT](LICENSE).

This project vendors [MinHook](https://github.com/TsudaKageyu/minhook)
(BSD-2-Clause). See
[`third_party/minhook/LICENSE.txt`](third_party/minhook/LICENSE.txt).

## Acknowledgements

- **[Tsuda Kageyu](https://github.com/TsudaKageyu)** for
  [MinHook](https://github.com/TsudaKageyu/minhook) — this fix would
  have been at least 10× more code without it.
- The **Steam Community, /r/Deusex, and PCGamingWiki** threads that
  have been trading affinity workarounds for this crash since 2017.
- **Eidos Montréal** for one of the best immersive sims ever made,
  even if they shipped it with this bug. DXMD hasn't seen an update
  since 2017, which is why this fix exists.
