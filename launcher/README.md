# SR2 Launcher

Madeira 0.2.0 built for Slime Rancher 2. It is Madeira's own app with six patches:

1. a fix for the slowdown measured in this phone's logs,
2. a counter that shows whether the fix worked,
3. Slime Rancher 2's settings built in, so nothing has to be set by hand,
4. its own name and build stamp, and the memory settings the game needs,
5. a fix that lets an IPA built by GitHub's macOS 27 runners start a game on iOS 26,
6. a fix in FEX, the x86 translator, for a lock that stopped the game for good once MelonLoader's .NET
   runtime started (builds 8 to 10).

Built by GitHub Actions ([workflow](../.github/workflows/sr2-launcher.yml)) and published on this
repository's [Releases](https://github.com/Rizje/slime-rancher-mp-ios/releases) page as
`SR2Launcher.ipa`.

## Install (on the iPhone)

1. In Safari, open the newest **SR2 Launcher, build N** on the
   [Releases](https://github.com/Rizje/slime-rancher-mp-ios/releases) page and download
   `SR2Launcher.ipa`.
2. Open it in **SideStore** (share the file to SideStore, or in SideStore tap **+** and pick it). Keep
   the app extensions when asked: one of them is Madeira's JIT helper.
3. It **replaces Madeira** (same bundle identifier, `com.willfaust.madeora`): the installed game,
   saves, Steam sign-in, JIT pairing and Memory+ all stay, and it uses no extra app slot. The Home
   Screen name becomes **SR2 Launcher**, and the app's folder in the Files app is renamed to match.
4. If the d3d11 bridge is still in the game folder, delete `d3d11.dll` next to `SlimeRancher2.exe`.
   The launcher removes `-force-d3d12` from the launch arguments by itself.
5. Open SR2 Launcher, enable JIT as usual, tap **Slime Rancher 2**, **Play**.

There is nothing to set. The first time the library loads, the launcher writes the game's settings
once, replacing whatever the game had (the old config goes to the log first). Anything changed
afterwards stays. **Reset to Recommended**, at the bottom of the game's settings, brings the
launcher's settings back.

## What it sets

The game's config (**Game details › This game's config**):

| Line | Why |
|---|---|
| `env.MADEIRA_EC_HOOK_TRACE = 0` | Madeira's standard line |
| `donor-pool-guard = 1` | The world loads with it (tested on this phone) |
| `env.FEX_VECTORTSOENABLED = 0` | The tested config; already FEX's default here, as 8 of Madeira's lists have it |
| `env.MADEIRA_FPS_GOV_MW = 2000`, `_MAX = 30`, `_MIN = 20` | FPS governor: holds the CPU near 2 W so iOS throttles later (tested: "better") |

That is the config of the tested runs, line for line, except `env.FEX_MEMCPYSETTSOENABLED = 0`: FEX's
log shows `memcpyset=false` without it, so it is already the default, and Madeira's own checks forbid
it in a list.

Build 3 (list version 1) also set `env.MADEIRA_PROBES = light` and `env.MADEIRA_LEAN_DIAG = 1`, as
diagnostics-only switches. They are not: the first never starts Madeira's thread sampler, and the
second skips its stuck-store check, which completes a store stuck across a 16 KB page. The tested
runs had that check. From build 4 the list is version 2 without them; the launcher writes it once
more over version 1.

The game's switches: **Resolution** 960×540, **MetalFX upscaling** 2×, **Display** Fit,
**FPS limit** 30, **Launch arguments** empty. This is the display setup of the tested runs. Known
issue, unchanged: the game's own Options menu shows white at 960×540. To change the game's options,
set Resolution to 1280×720 for that session.

Memory (**Settings › Memory & sync**), written once and only where `madeira.cfg` has no value of its
own, on a device with less than 10 GB: **Swap tier** 4 GB, **Swap coverage** "1 MB+ and overflow"
(`wide`), **Donor pool** 2 GB. These are the settings the world loaded with on this phone. A phone
that already has them (this one does) is left alone, and so is `eco = 1` and everything else in
`madeira.cfg`.

## What the patches do

| Patch | Change | Turn it off |
|---|---|---|
| [0001](patches/0001-ntdll-no-region-query-for-shared-data-reads.patch) | Reads of Windows' shared clock page (`KUSER_SHARED_DATA`) no longer ask the kernel about the memory behind them. That query walked iOS's whole memory map, 100–130 times a second, on the single thread that serves every game thread's faults: 47 % of a core on average and over 90 % late in a run, with the game waiting on it | `env.MADEIRA_USD_PROBE = 1` |
| [0002](patches/0002-ntdll-exception-thread-census.patch) | One `[exc-census]` log line every 5 s: how busy that thread is | `env.MADEIRA_EXC_CENSUS = 0` |
| [0003](patches/0003-app-slime-rancher-2-settings-built-in.patch) | The settings above, recognised by the game's Steam App ID, folder or program | Edit them, or delete the config lines |
| [0004](patches/0004-app-sr2-launcher-name-defaults-and-build-stamp.patch) | The name, the memory defaults, the build stamp in the log and Get Help, a line in Setup Guide › About; the app built as one executable, as Madeira's own 0.2.0 release is | — |
| [0005](patches/0005-ntdll-no-pipe2-on-ios.patch) | Wine makes its server pipes with `pipe()` on iOS, never `pipe2()` (below) | — |
| [0006](patches/0006-fex-patches-code-buffer-list-walk-re-entry.patch) | Adds `build/fex-patches/0002-…` to Madeira's own FEX patches, built into `xtajit64.dll`: a code invalidation that comes back to the thread already walking FEX's code buffer list no longer waits on that thread's own lock (below) | — |

The `env.` switches go in the game's config like any other line. Why 0001 is safe: the window onto
that page is set up once, onto memory Madeira keeps mapped for the whole session, so a read that
lies wholly inside the page and lands exactly there cannot reach unmapped memory, which is all the
query guards against. Writes, other addresses and anything not matching exactly still take the
query. [tests/usd_probe_test.c](tests/usd_probe_test.c) checks those boundaries.

## Check that it worked

Play about five minutes in the world, then send the Get Help files. In the log:

| Line | Expected |
|---|---|
| `[build]` | `0.2.0 · SR2 Launcher 1 · …` |
| `ml989: running image` | `…/App.app/Madeira` (not `Madeira.debug.dylib`) |
| `[recommended] Slime Rancher 2:` | `slime-rancher-2 v2 written (reset)`, after the config it replaced |
| `[probes]` and `[lean-diag]` | absent (the thread sampler and its stuck-store check run, as in the tested runs) |
| `[subfloor] sr2 KUSER_SHARED_DATA reads skip the backing probe` | present |
| `[subfloor] ml956 serviced … usd-probe-skips=N` | N rising |
| `[exc-census] sr2 …` | `% of a core` a few percent (it was 47–96 %) |
| `wine-x18-exc` in `[xp-t]` | the same drop |
| FPS after the first 100 s | above the 14–20 of Madeira 0.2.0; the aim is 30 |

What it cannot promise: the fix removes the largest cost measured once iOS throttles the processor,
and the game's own threads need less than one core there, so 30 FPS is within reach, but only the
phone can show it. If something else is left, `[exc-census]` and `[xp-t]` name it.

## Builds 3 to 5 closed the app as the game started; build 6 removes the cause

Builds 3 to 5 closed the whole app as Wine connected to its server, before the game ran. The log
ended with `wineserver crashed`, and iOS's crash report for build 5 showed where:

```
EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0
do_sigsegv  <-  server_pipe+44  <-  init_thread_pipe+68  <-  server_init_process+1212  <-  __wine_main
```

`server_pipe+44` is the return from its call to `pipe2()`. Wine's build configuration (`config.h`)
is written by Wine's `configure` on the build Mac. GitHub's `xcode-27` runner runs macOS 27, which
has `pipe2`, so the configuration said the system has it. iOS 26 does not, and the call, linked
weakly, jumped to address 0 the first time Wine made a pipe. Madeira's own 0.2.0 IPA was configured
on a Mac without `pipe2`, so it never calls it.

Patch 0005 makes `server_pipe` use `pipe()` on iOS, as that build does. `check.sh` fails if any
`pipe2` call is built for iOS, and the workflow refuses an IPA that links weakly to any system
function outside a short list known to be on iOS 26 (in build 5, `pipe2` was the only one). The earlier
guesses, the two diagnostics switches and Xcode's debug dylib, were not the cause.

## Build 6 stood still once MelonLoader started .NET; builds 8 to 10 remove the cause

With MelonLoader installed ([melonloader/README.md](melonloader/README.md)) build 6 showed
"Waiting for its window" for as long as it was left (29 minutes), and MelonLoader's `Latest.log` ended
after its two first lines, the ones that pick the .NET runtime. The game log showed where: the game
thread was parked on a lock inside FEX, Madeira's x86 translator, with two frames of the same
function on its stack, and the two .NET threads waited behind it. The line right before the stop:

```
[inval-nested] #1 invalidate 0x336c2d000: this thread already holds the code invalidation lock
```

FEX keeps a list of its code buffers (one per thread) and walks it under a plain lock when code
must be re-translated, which .NET's compiler causes constantly. While the game thread walked that
list, a Wine thread exited, and the walk became the last holder of that thread's buffer and
destroyed it. Destroying it frees the buffer's memory; Wine reports every free back to FEX as a
range to re-translate; and that report arrived on the same thread, which took the list lock a second
time and waited on itself. Build 8 made FEX note which thread holds that lock and skip a report arriving on it; the game then
stood on the next lock along the same path (the lookup cache the outer re-translation was clearing).
Patch 0006 as of build 9 goes one level up: a report that arrives while the thread already holds
FEX's code-invalidation lock is FEX's own doing (memory it frees or reprotects under that hold), never
the game's code, so the whole nested re-translation is skipped (`[inval-nested-skip]` in the log).
Build 9 then stopped once more: the game thread waited on FEX's list of code buffers while another
thread held it and was itself waiting. Build 10 holds that list only for the few instructions that copy
or append to it, never while anything can wait; the slow parts (clearing caches, freeing memory,
growing the list) happen after it is released. [tests/codebuf_lock](tests/codebuf_lock/run.sh) runs
the translator's real list functions under those conditions: build 9's hung in 10 of 30 runs, build
10's finish every run (also under ThreadSanitizer). `check.sh` runs it on every build. The patch lives in `build/fex-patches/`, the folder
Madeira's build uses for its own FEX changes, so `xtajit64.dll` is rebuilt with it; `check.sh`
fetches Madeira's FEX pin and proves the patches apply, and the workflow refuses an IPA whose
`xtajit64.dll` lacks the patch's log tags.

## If it crashes

Send these, so the cause can be read rather than guessed:

1. Open SR2 Launcher again and use **Get Help**: it collects the log of the last game session, the
   game's config and `madeira.cfg`.
2. iOS's own crash report: **Settings › Privacy & Security › Analytics & Improvements › Analytics
   Data**, the newest entry starting with `Madeira` (share it from the top-right button).

Until then, Madeira's own 0.2.0 IPA (below) plays as before.

## Undo

Install Madeira's own 0.2.0 IPA from <https://github.com/willfaust/Madeira/releases> the same way.
It replaces SR2 Launcher; the game, saves and sign-in stay. The game keeps the launcher's settings
until they are changed or reset.

## Build it

The workflow builds on every push to `main` or a `claude/` branch that changes `launcher/` or the
workflow. It is Madeira's own `build.yml` at v0.2.0 with Madeira checked out at the commit in
[UPSTREAM](UPSTREAM) and the patches applied by [apply.sh](apply.sh) as one commit. The first run
builds everything (LLVM, FEX, Wine, the runtime components; several hours on GitHub's macOS
runners, free for a public repository); later runs reuse those caches and rebuild only the app.

- **checks** runs [check.sh](check.sh). The release waits for it.
- **app** builds the IPA and checks it with [verify-ipa.sh](verify-ipa.sh): bundle identifier,
  name, build stamp, entitlements, and the patches' code in the binary.
- **release** publishes `SR2Launcher.ipa` as the prerelease `sr2-launcher-<run number>`.

Options (**Actions › SR2 Launcher IPA › Run workflow**, once the workflow is on the default branch):
**stock** builds Madeira without the patches, to tell a build problem from a patch problem; it is
not published. A push whose commit message contains `[sr2 stock]` does the same. **bundle_id**
builds a separate app next to Madeira; it then needs the game copied in again and Memory+ set up
for the new App ID, and it uses three more App IDs.

Checks without a Mac (git, Python 3, a C compiler):

```bash
bash launcher/check.sh
```

It applies the patches to a fresh copy of Madeira, runs the boundary test, checks every setting
against the keys Madeira reads, and runs Madeira's own host checks that read a changed file on stock
and patched copies (`--all` runs all of them). A check that passes on stock must pass patched.

## Licence

GPL-3.0-or-later, as Madeira: [COPYING.md](COPYING.md).
