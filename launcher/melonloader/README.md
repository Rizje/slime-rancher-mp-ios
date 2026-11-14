# MelonLoader for Slime Rancher 2 in SR2 Launcher

One zip with everything MelonLoader needs, arranged as it goes into the game folder. It's built by
[`sr2-melonloader-pack.yml`](../../.github/workflows/sr2-melonloader-pack.yml) from official
downloads and published on [Releases](https://github.com/Rizje/slime-rancher-mp-ios/releases) as
**MelonLoader 0.7.3 pack for Slime Rancher 2**.

| In the zip | From |
|---|---|
| `winhttp.dll`, `MelonLoader/` | [MelonLoader v0.7.3](https://github.com/LavaGang/MelonLoader/releases/tag/v0.7.3), `MelonLoader.x64.zip` (Apache-2.0); `winhttp.dll` is its `version.dll`, renamed (below) |
| `dotnet/` | Microsoft's .NET 6.0.36 Runtime and Desktop Runtime for Windows x64, SHA-512 checked against Microsoft's release metadata (MIT) |

0.7.3 is the version Ranching Together needs, so the multiplayer mod can go on top later. For
IL2CPP games like this one MelonLoader runs on .NET 6. On Windows it installs that runtime itself;
0.7.3 can instead use a portable runtime in `<game>/dotnet`, so nothing gets installed inside Wine.

## Install (on the iPhone)

Needs SR2 Launcher **build 10** or later: build 6 stood at "Waiting for its window" for good once
MelonLoader started its .NET runtime ([launcher/README.md](../README.md), patch 0006).

1. In Safari, download `SR2-MelonLoader-0.7.3-pack.zip` from the newest
   **MelonLoader 0.7.3 pack** on the Releases page. In Files, tap it to unzip.
2. Open the new folder **Slime Rancher 2 MelonLoader**. Select `winhttp.dll`, `MelonLoader` and
   `dotnet`, tap **Move**, and move them into
   **SR2 Launcher › wine › drive_c › Program Files (x86) › Steam › steamapps › common ›
   Slime Rancher 2**, next to `SlimeRancher2.exe`.
3. In SR2 Launcher: **Slime Rancher 2 › Game details › Advanced › This game's config**, add this
   line at the end, tap **Done**:

   ```
   env.WINEDLLOVERRIDES = winhttp=n,b
   ```

4. Play. The first start with MelonLoader is much slower than usual: MelonLoader reads the game's
   code and writes its own copies of it, and downloads parts of that from the internet. Leave it
   until the menu appears; later starts are close to normal.

It worked when the game folder has `MelonLoader/Latest.log` and a `Mods` folder after that first
start.

To undo, delete `winhttp.dll` from the game folder. Without it the other files do nothing.

## Why `winhttp.dll` and not `version.dll`

MelonLoader loads through a proxy DLL in the game folder that Windows loads in place of a system
one. Its usual name, `version.dll`, does not work in SR2 Launcher or Madeira 0.2.0. Madeira's own
system DLLs import `version.dll` directly (`shell32`, `setupapi` and others, which upstream Wine
loads later), and nearly every core DLL reaches one of them: `advapi32`, `user32`, `ole32` and
`gdi32` all do. So Wine has to start a `version.dll` proxy in the middle of starting those, before
`msvcrt` (the C runtime) is ready. MelonLoader's start-up calls `advapi32!LookupPrivilegeValueW`,
which uses `msvcrt`'s `_wcsicmp`, and the game closed with an access violation in
`msvcrt_get_thread_data` (log: `"version.dll" failed to initialize, aborting`). No Madeira system
DLL imports `winhttp.dll`, and Unity's player does, so under that name MelonLoader starts after
the DLLs it needs. MelonLoader 0.7.3 picks its exports by its file name; `winhttp` is one of its
proxy names (`ProxyMap.cs`).

Already copied the files as `version.dll`? Rename it to `winhttp.dll` (long-press, Rename) and
change the config line to `winhttp=n,b`.

## If it fails

Send the Get Help files and `MelonLoader/Latest.log` from the game folder (Files app: long-press,
Share).

What has been found so far, in order:

| Build | What happened | Cause and fix |
|---|---|---|
| 6, pack 2 (`version.dll`) | The game closed at once: `"version.dll" failed to initialize` | MelonLoader started before `msvcrt`; the pack's DLL is `winhttp.dll` since pack 3 (above) |
| 6, pack 3 | "Waiting for its window" for good; `Latest.log` ends after the two lines that pick the .NET runtime | A lock in FEX taken twice by the game thread when .NET's code churn freed a dead code buffer; launcher patch 0006, build 8 |
| 8, pack 3 | The same screen; the log shows the fix working and the thread then stopping on the next lock of the same kind | Patch 0006 now skips the whole nested re-translation; build 9 |
| 9, pack 3 | The same screen after about 10 s of .NET; the game thread waits on FEX's code buffer list held by a blocked thread | Patch 0006 holds that list only while nothing can wait; build 10 |
| 9, with `DOTNET_TieredCompilation = 0` and `DOTNET_EnableWriteXorExecute = 0` | "Madeira Dock stopped" before a window | Those two lines made it worse: leave them out |
