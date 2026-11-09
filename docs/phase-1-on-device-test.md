# Phase 1: on-device test

Device: iPhone 16 Pro Max (A18 Pro, 8 GB RAM), latest iOS 27, SideStore already installed.

The test answers one question: **does Slime Rancher 2 run well enough in Madeira on this phone?**
Do parts A to C first. Only add mods (part D) once the plain game passes.

Write down your results in the table at the end and bring them back, along with Madeira's logs if
anything fails.

## A. Install Madeira

1. In Safari, open <https://github.com/willfaust/Madeira/releases> and download the latest `.ipa`.
   Only use this official page: copycat "PlayPort"/Madeira downloads exist.
2. Open the file in **SideStore** (not your paid signing certificate, see the note below).
   If SideStore asks about app extensions, choose to **keep** them: Madeira's JIT helper is an
   extension.
3. Open Madeira and go to **Settings → JIT → JIT setup → In-app** (iOS 27 lets the phone pair with
   itself, no computer needed):
   1. Tap **Start pairing** and allow Local Network access.
   2. In iOS **Settings → Privacy & Security → Developer Mode**, tap **Pair with Madeira** and enter
      the code Madeira shows.
   3. Back in Madeira, connect **LocalDevVPN**, tap **Check setup**, then **Enable JIT**.
4. Madeira's **Settings** should show green checks for **JIT** and **Memory+** and say
   **Ready to play**.
5. **If Memory+ is not green** (likely with SideStore and a free Apple ID): SideStore asks for the
   Increased Memory Limit but doesn't turn it on for free accounts
   ([SideStore#1616](https://github.com/SideStore/SideStore/issues/1616)). Without it, an 8 GB
   iPhone gives the app only about 3.3 GB, far too little for Slime Rancher 2. Fix it once:
   1. Sideload [GetMoreRam](https://github.com/hugeBlack/GetMoreRam) and sign in with the same
      Apple ID SideStore uses.
   2. On its **App IDs** page, tap **Refresh**, tap Madeira's App ID, then
      **Add Increased Memory Limit**.
   3. Reinstall Madeira from SideStore (it upgrades in place) and check that Memory+ is green.
   4. Delete GetMoreRam afterwards to free the app slot. SideStore's weekly refresh keeps the fix.

> **Why SideStore and not the paid certificate:** JIT only works if Madeira is signed as
> debuggable. That needs a development certificate, which SideStore uses with your Apple ID.
> Paid certificate services usually sign with distribution certificates, which can't be
> debuggable. Madeira shows a signing error if this is wrong.

## B. Install Slime Rancher 2

1. In Madeira's library, tap **Sign in to Steam** and sign in.
2. Install **Slime Rancher 2** (it must be the Steam version).
3. Give it time: the download is several GB.

**Faster: copy the game from your PC.** Madeira's downloader checks files already on disk against
Steam's checksums and keeps every part that matches, so it only verifies instead of downloading.

1. On the PC, make sure Steam has Slime Rancher 2 fully updated, with **no mods** in the folder.
   It's in `Steam\steamapps\common\` (right-click the game in Steam → Manage → Browse local files).
2. Copy that whole folder to the phone, using either:
   - a **USB-C flash drive or SSD** (exFAT). Plug it into the iPhone and use the Files app. This is
     the fastest option.
   - a **shared folder**: share it on the PC, then on the iPhone use Files → **⋯** →
     **Connect to Server** → `smb://<PC's IP>`.
3. In Madeira, tap **Install** on Slime Rancher 2, then **pause** it as soon as it starts. This
   creates the game's folder with the exact name Madeira expects.
4. In Files, copy the game's contents into
   **Madeira › wine › drive_c › Program Files (x86) › Steam › steamapps › common ›** that folder,
   replacing what's there.
5. Resume the install in Madeira. It should check the files for a few minutes, download only
   what's different, then finish. Then **Repair installed files** isn't needed.

## C. Run the plain game (no mods)

Open the game's details page and set:

| Setting | Value | Why |
|---|---|---|
| Resolution | This screen's shape, the 480-line option | MetalFX 1.5× brings it to about 720p |
| MetalFX upscaling | 1.5× | Big GPU saving |
| FPS limit | 30 | Steadier, cooler, lower battery drain |
| Pause threads fully | Automatic | Turns itself on for Unity games like this one; prevents a known crash |
| Performance overlay | On | Shows FPS, memory and thermal state |

Then press **Play** and check, in order:

1. **Boot:** does it reach the main menu?
2. **Render:** does the world look right (no black screen, missing textures or flicker)?
3. **In-game settings:** set the game's graphics to the lowest preset.
4. **Frame rate:** note the FPS on the ranch and in a busy area (lots of slimes).
5. **Memory:** note the memory figure on the overlay. This phone allows about 6 GB per app.
6. **Stability:** play 20 minutes. Does it crash, get killed, or slow down as the phone heats up?
7. **Controls:** in the touch controls menu, choose the built-in **Xbox controller** layout. Can you
   move, look, vacuum, shoot and jump?

**If it gets killed for memory:** in Madeira, **Settings → Memory & sync**, set the swap tier to
2 GB, restart Madeira and try again.

**If it fails before the menu:** stop and collect Madeira's logs. That's a Madeira bug to report,
and nothing in parts D or later will help.

**Plan B for part C only:** if the game won't render or runs badly in Madeira, try the same test
in [Playport](https://github.com/playportdev/playport) (official repo only). It's a fork of Madeira
with newer Wine and FEX and a different graphics path (Vulkan by default, DXMT as an option), so
it may behave differently. Its release app can't set the DLL override MelonLoader needs, so it's
only for checking the plain game. A free Apple ID allows 3 sideloaded apps at once, so you may
need to remove one to install it.

## D. Add the mods (only if part C passed)

Ranching Together needs three things, all versions matching exactly:
**MelonLoader 0.7.3**, **Starlight 4.0.3** and **Ranching Together**. It also requires Slime
Rancher 2 version **1.3.0**.

1. In Safari, download:
   - MelonLoader: the **x64 zip** from <https://github.com/LavaGang/MelonLoader/releases> (v0.7.3)
   - Starlight: `Starlight.dll` from <https://github.com/ThatFinnDev/Starlight/releases> (v4.0.3)
   - Ranching Together: from <https://www.nexusmods.com/slimerancher2/mods/118> or its Discord
2. In the **Files** app, unzip MelonLoader. Then open the game's folder at
   **Madeira › wine › drive_c › Program Files (x86) › Steam › steamapps › common ›**
   the Slime Rancher 2 folder.
3. Copy MelonLoader's contents into the game folder, so `version.dll` and the `MelonLoader` folder
   sit next to the game's `.exe`.
4. Make a `Mods` folder there if there isn't one, and put `Starlight.dll` and the Ranching Together
   `.dll` in it.
5. In Madeira, open the game's details page → **Advanced → This game's config** and add this line:

   ```
   env.WINEDLLOVERRIDES = version=n,b
   ```

   This tells Wine to load MelonLoader's `version.dll` from the game folder.
6. Press **Play**. The first launch with MelonLoader is slow: it prepares its files on the phone
   and downloads some of them, so it needs internet. Let it finish.
7. Pass: a **Multiplayer** button appears on the main menu.

## E. Two phones on the same Wi-Fi (phase 3, needs a second device)

Each phone needs its own Madeira install, its own Steam copy of Slime Rancher 2, and the same mod
versions.

1. On both phones, allow Madeira in **Settings → Privacy & Security → Local Network**.
2. **Host:** find the phone's IP in **Settings → Wi-Fi → (i)**. In the game, open Multiplayer →
   Host → **Manual**, enter that IP, and use **1919** for the ports. Skip **Automatic**: it relies
   on UPnP, which iOS is likely to block.
3. **Joiner:** load an **empty save** first, because joining replaces it with the host's world.
   Then Multiplayer → Join, and enter the host's IP and port 1919, or the join code the host shows.
4. The host keeps Madeira open the whole session.

Guest, school and hotel Wi-Fi often block devices from talking to each other. Use home Wi-Fi.

## Results to bring back

| Check | Result |
|---|---|
| Madeira installed, JIT + Memory+ green | |
| Game reaches main menu | |
| World renders correctly | |
| FPS on the ranch | |
| FPS in a busy area | |
| Peak memory on the overlay | |
| 20 minutes without a crash or kill | |
| Touch controls usable | |
| (Part D) Multiplayer button appears | |
