Slime Rancher 2: MelonLoader pack for SR2 Launcher
==================================================

This folder holds MelonLoader @MELONLOADER@ (winhttp.dll and the MelonLoader folder) and Microsoft's .NET @DOTNET@ Desktop Runtime for Windows x64 (the dotnet folder),
which MelonLoader runs on. MelonLoader 0.7.3 reads a .NET runtime from <game>/dotnet, so
nothing has to be installed inside Wine.

Install (Files app on the iPhone):

1. Move everything in this folder (winhttp.dll, MelonLoader, dotnet) into
   SR2 Launcher > wine > drive_c > Program Files (x86) > Steam > steamapps > common >
   Slime Rancher 2, next to SlimeRancher2.exe. (This text file can stay behind.)
2. In SR2 Launcher: Slime Rancher 2 > Game details > Advanced > This game's config,
   add this line at the end and tap Done:

       env.WINEDLLOVERRIDES = winhttp=n,b

3. Play. The first start with MelonLoader takes much longer than usual: MelonLoader
   reads the game's code and writes its own copies of it, which needs the internet.
   Leave it running until the game's menu appears.

Undo: delete winhttp.dll from the game folder (the other files then do nothing).

winhttp.dll is MelonLoader's version.dll under another of its proxy names. In SR2 Launcher (and
Madeira) Wine's own system DLLs import version.dll, so a version.dll proxy starts too early and
the game closes before its menu; only the game itself imports winhttp.dll.

Licences: MelonLoader is Apache-2.0 (MelonLoader-LICENSE.md); .NET is MIT
(dotnet/LICENSE.txt, dotnet/ThirdPartyNotices.txt).
