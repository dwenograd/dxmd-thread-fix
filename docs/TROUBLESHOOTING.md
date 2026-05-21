# Troubleshooting

## No log file at all

If `dxmd-thread-fix.log` doesn't appear in `retail\` after launching
the game, our DLL didn't load (or didn't get far enough to open the
log). Most common causes:

1. **DLL not in `retail\`** — you put it in the wrong folder.
   `dxgi.dll` must be next to `DXMD.exe`, not next to the launcher or
   in the parent `Deus Ex Mankind Divided\` folder.
2. **DLL blocked by antivirus** — see [AV / SmartScreen](#av--smartscreen).
3. **DLL blocked by Mark-of-the-Web** — see [Mark-of-the-Web](#mark-of-the-web-blocks-the-dll) below.

## AV / SmartScreen

Windows Defender, Norton, McAfee, Smart App Control on Windows 11,
and other security tools sometimes block or quarantine unsigned DLLs
that hook Windows APIs. Symptoms:

- DLL silently disappears from `retail\` after a few minutes.
- Or: game runs but the log file doesn't appear.
- Or: an AV notification.

Fixes (in order of preference):

1. **Verify the SHA-256** of the released `dxgi.dll` against the hash
   on the GitHub release page. If it matches, you have the genuine
   article — your AV is a false positive.
2. **Add an exception** for the `retail\` folder in your AV.
3. **For Smart App Control on Windows 11**: prefer adding a Defender
   allow rule for `dxgi.dll` (Windows Security → Virus & threat
   protection → Manage settings → Exclusions). **Do not** turn Smart
   App Control off as a workaround — on Windows 11, once SAC is
   turned off it cannot be re-enabled without resetting or
   reinstalling Windows. If you must disable a check, exclude this
   specific file by path after verifying its SHA-256 against the
   GitHub release.

## Mark-of-the-Web blocks the DLL

If you downloaded the zip from a browser, Windows tags it as "from
the internet". Some AV configurations refuse to load MOTW-tagged
DLLs into a process at all.

Fix: right-click the zip → Properties → check "Unblock" → Apply →
*then* extract. Or extract first, then right-click the extracted
`dxgi.dll` → Properties → "Unblock".

## `FIX STATUS: INACTIVE` in the log

The DLL loaded but couldn't install at least one of the 6 required
CPU-topology hooks. Possible causes:

- Another tool (AppVerifier, ETW instrumentation, an AV runtime
  shim) has already instrumented the same `kernel32` exports in a
  way MinHook can't patch.
- An unusual Windows version doesn't export one of the topology APIs
  we need. (Should be impossible on Win10+, but possible on heavily
  patched/custom systems.)

The log will name the specific API that failed. Please file a bug
report with the full log attached.

## `FATAL: real dxgi could not be loaded`

Our DLL loaded, but it couldn't open the real
`C:\Windows\System32\dxgi.dll` to forward to. Causes:

- AV blocking System32 dxgi loads (rare; major OS breakage).
- Wrong Windows version (very old / Wine / partial-OS environments).
- Disk error.

The DLL deliberately returns `FALSE` from its entry point in this
case, so the game shows a clean "DLL load failed" rather than
crashing later.

## Game launches normally but my crash still happens

If the crash you're seeing is during **rapid menu interaction**
(inventory, looting), this mod does not fix that bug — it's a
separate issue with a different root cause.

If the crash happens during something else (combat, save/load, zone
transition), please file a bug report with:

- Full `dxmd-thread-fix.log`
- Steam crash dump from `%LOCALAPPDATA%\CrashDumps\DXMD.exe.*.dmp`
- Your CPU model and Windows version
- What you were doing when it crashed
- Whether it crashes consistently or intermittently

## Steam "Verify integrity of game files"

Steam Verify ignores extra files in the game folder (it only checks
files Steam itself tracks). It will neither remove our `dxgi.dll`/log
nor restore anything in their place. Safe to run with our shim
installed.

## Game update through Steam

DXMD hasn't been updated since 2017, so this is unlikely. If it ever
happens and Steam adds its own `dxgi.dll` to `retail\`, ours will be
overwritten — just reinstall.
