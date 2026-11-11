# Performance findings: what limits Slime Rancher 2 on the iPhone

Measured from Madeira 0.2.0 (11-03) logs on an iPhone 16 Pro Max (A18 Pro, iOS 26.2.1), 2026-11-09
and 2026-11-10, at 960×540 with MetalFX 1.5× and the game on its lowest settings.

## The limit is heat, through the CPU clock

Madeira logs the performance cores' clock, the process's CPU power and the frame rate every few
seconds. Across five runs, the frame rate follows the performance cores' clock:

| Performance cores | Frame rate |
|---|---|
| 3.0–3.7 GHz (menu and world) | 24–30 FPS |
| 1.0–2.2 GHz (after iOS cuts the CPU) | 14–20 FPS |

The cut follows the phone's thermal state. In the 10:24 run on 2026-11-09, without the FPS
governor, Madeira logged `[thermal] nominal -> fair at 27 FPS` 74 s after the first frame. Twenty
seconds later the performance cores averaged 1.0 GHz, the process's CPU power fell from about
1.7 W to 0.4–0.8 W, and the game ran at 14–20 FPS. When the clock came back to 3.0 GHz for a
moment, it ran at 29 FPS.

PlayPort measured the same mechanism: once thermal pressure rises, iOS lowers its CPMS thermal
power budgets and the process loses the performance cores
([PlayPort, Hollow Knight power budget](https://github.com/playportdev/playport/blob/main/docs/evidence/2026-09-26-hk-power-budget.md)).

The GPU is not the limit: 16–26% busy on Direct3D 11.

## Where the processor time goes

Madeira's per-thread census (`[xp-t]`) with the thread names it logs (`[thr-name]`), for the
10:24 run. Shares are of one core; "clamped" is the world after iOS cut the CPU (100–210 s).

| Thread | Menu, full clock | World, clamped |
|---|---|---|
| `wine-x18-exc` (Madeira: repairs register x18, which iOS clears) | 17% | 47% |
| Game main thread | 15% | 30% |
| `Job.Worker` 0–4 (Unity job system) | 5% | 29% |
| `UnityGfxDeviceWorker` (render thread) | 5% | 13% |
| Three unnamed Madeira threads | 14% | 30% |
| `dxmt-encode-thr` (Direct3D 11 to Metal) | 3% | 7% |

In the clamped world only about half of the processor time is the game's own threads; the
largest single consumer is Madeira's x18 repair thread. Unity starts 5 job workers and 16
background workers, sized for a desktop PC.

## The root cause of the throttled slowdown: one kernel query

The census above was the clue: in the clamped world the game's own threads need less than one core,
yet the frame rate halves. The game is waiting, not short of processor time.

`wine-x18-exc` is Madeira's Mach exception thread. It serves every fault of every game thread, one
at a time, while the faulting thread waits for its reply (Madeira's own comment calls it "a hard
serialised ceiling on the whole app"). Over the 10:24 run it used 47 % of a core on average and
91–96 % late in the run, at a steady fault rate. Three of ten stack dumps
(`[thread-stacks]`) caught it in the same place:

```
ios_subfloor_service -> ios_subfloor_backing_probe -> mach_vm_region_recurse
```

That path runs on every read of Windows' shared clock page (`KUSER_SHARED_DATA`, guest
`0x7ffe0320`/`0x7ffe0328`), which the game reads 100–130 times a second
(`[subfloor] ml956 serviced …`). The page is mapped more than once, and for such an object the
kernel answers that query by walking every entry of the task's memory map (Apple's
`osfmk/mach/vm_region.h` has `VM_REGION_INFO_FLAGS_NO_ALIASED`, "(perf)", to skip exactly that).
The map grows as the game runs, so the thread's cost grew with it: 17 % to 91 % in one run.

The query only guards against touching memory with nothing behind it. Madeira registers the window
onto that page once, onto `user_shared_data`, which stays mapped and readable for the whole
session, so for a read wholly inside the page that lands exactly there the query cannot find
anything wrong.

The other slow region queries in Madeira's fault and memory code (about 70 call sites audited) are
not on a per-fault path: start-up, censuses, the pool warmer, image loading, process exit, a
fallback after five refused allocations, diagnostics and 32-bit I/O. The pool warmer's sweep runs
every 10 s; `MADEIRA_LEAN_DIAG = 1` makes it once a minute.

## SR2 Launcher

[SR2 Launcher](../launcher/README.md) is Madeira 0.2.0 with four patches, built by GitHub Actions on
Madeira's own CI image: reads of that page skip the query (switch: `env.MADEIRA_USD_PROBE = 1`
restores it), an `[exc-census]` line every 5 s shows the exception thread's load, and the game's
tested settings are built in, with `MADEIRA_PROBES = light` and `MADEIRA_LEAN_DIAG = 1` to stop
diagnostics that pause or sweep while the game runs. It installs over Madeira and keeps the game.

It removes the largest measured cost in the throttled state. Whether that is enough for 30 FPS there
can only be measured on the phone; the launcher's README lists what the log should show.

Unity's job workers stay as they are for now: `-job-worker-count 2` (or `job-worker-count=2` in
`boot.config`) is the next lever once the launcher's numbers show what is left.

## What was ruled out, and why

| Lever | Result |
|---|---|
| Direct3D 12 through the [d3d11 bridge](../tools/d3d11-bridge/README.md) | Runs, at 9–15 FPS against 28–30 on Direct3D 11: madeira-d3d12 spends 65–165 ms a frame replaying commands |
| FEX TSO emulation off (`FEX_TSOENABLED=0`) | Not usable: PlayPort's Unity test game hangs entering a new game |
| Vector TSO off (`FEX_VECTORTSOENABLED=0`) | About 4–5% of CPU work (PlayPort); already FEX's default here (`vector=false` in the log), and in this game's config |
| LRCPC2 TSO loads and stores | About 2–5% (PlayPort); a FEX build feature, not a setting |
| FEX disk cache | Faster starts, no change to frame pacing (PlayPort) |
| FPS governor (`MADEIRA_FPS_GOV_*`) | Holds about 2 W by capping the frame rate at 20–30; keeps the clock up, at the cost of frames |

## What else can change it

In software, after SR2 Launcher's numbers are in:

1. **Unity's job workers:** `-job-worker-count 2` cuts the workers' own time and their wake-ups,
   which go through Madeira's sync and fault paths.
2. **Less work per frame** (the planned MelonLoader performance mod): less CPU power for the same
   frame rate, so the phone heats more slowly and the cut comes later or not at all.

Outside the software, for completeness: every point above where the clock stays at 3 GHz or more
runs at 24–30 FPS, so less heat (no case, not charging while playing, lower brightness, an active
cooler) or more sustained power (an iPad Pro with an M-series chip) also helps. This project
does not rely on either.
