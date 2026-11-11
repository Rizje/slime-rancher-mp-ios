# MelonLoader for Slime Rancher 2 in SR2 Launcher

One zip with everything MelonLoader needs, arranged as it goes into the game folder. It's built by
[`sr2-melonloader-pack.yml`](../../.github/workflows/sr2-melonloader-pack.yml) from official
downloads and published on [Releases](https://github.com/Rizje/slime-rancher-mp-ios/releases) as
**MelonLoader 0.7.3 pack for Slime Rancher 2**.

| In the zip | From |
|---|---|
| `version.dll`, `MelonLoader/` | [MelonLoader v0.7.3](https://github.com/LavaGang/MelonLoader/releases/tag/v0.7.3), `MelonLoader.x64.zip` (Apache-2.0) |
| `dotnet/` | Microsoft's .NET 6.0.36 Runtime and Desktop Runtime for Windows x64, SHA-512 checked against Microsoft's release metadata (MIT) |

0.7.3 is the version Ranching Together needs, so the multiplayer mod can go on top later. For
IL2CPP games like this one MelonLoader runs on .NET 6. On Windows it installs that runtime itself;
0.7.3 can instead use a portable runtime in `<game>/dotnet`, so nothing gets installed inside Wine.

## Install (on the iPhone)

1. In Safari, download `SR2-MelonLoader-0.7.3-pack.zip` from the newest
   **MelonLoader 0.7.3 pack** on the Releases page. In Files, tap it to unzip.
2. Open the new folder **Slime Rancher 2 MelonLoader**. Select `version.dll`, `MelonLoader` and
   `dotnet`, tap **Move**, and move them into
   **SR2 Launcher › wine › drive_c › Program Files (x86) › Steam › steamapps › common ›
   Slime Rancher 2**, next to `SlimeRancher2.exe`.
3. In SR2 Launcher: **Slime Rancher 2 › Game details › Advanced › This game's config**, add this
   line at the end, tap **Done**:

   ```
   env.WINEDLLOVERRIDES = version=n,b
   ```

4. Play. The first start with MelonLoader is much slower than usual: MelonLoader reads the game's
   code and writes its own copies of it, and downloads parts of that from the internet. Leave it
   until the menu appears; later starts are close to normal.

It worked when the game folder has `MelonLoader/Latest.log` and a `Mods` folder after that first
start.

To undo, delete `version.dll` from the game folder. Without it the other files do nothing.

## If it fails

Send the Get Help files and `MelonLoader/Latest.log` from the game folder (Files app: long-press,
Share).
