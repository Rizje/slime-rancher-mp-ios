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

## What was ruled out, and why

| Lever | Result |
|---|---|
| Direct3D 12 through the [d3d11 bridge](../tools/d3d11-bridge/README.md) | Runs, at 9–15 FPS against 28–30 on Direct3D 11: madeira-d3d12 spends 65–165 ms a frame replaying commands |
| FEX TSO emulation off (`FEX_TSOENABLED=0`) | Not usable: PlayPort's Unity test game hangs entering a new game |
| Vector TSO off (`FEX_VECTORTSOENABLED=0`) | About 4–5% of CPU work (PlayPort); already in this game's config |
| LRCPC2 TSO loads and stores | About 2–5% (PlayPort); a FEX build feature, not a setting |
| FEX disk cache | Faster starts, no change to frame pacing (PlayPort) |
| FPS governor (`MADEIRA_FPS_GOV_*`) | Holds about 2 W by capping the frame rate at 20–30; keeps the clock up, at the cost of frames |

## What can change it

1. **Keep the phone cool.** Every point above where the clock stays at 3 GHz or more runs at
   24–30 FPS. An active (Peltier) clip-on cooler works on the cause directly. No controlled test
   of one on this phone was found, so the gain has to be measured: the log's `[thermal]` lines and
   the clock show whether it holds. Free steps that cut heat: no case, no charging while playing,
   lower screen brightness, and starting with the phone at `thermal=nominal`.
2. **Less work per frame** (the planned MelonLoader performance mod): less CPU power for the same
   frame rate, so the phone heats more slowly and the cut comes later or not at all.
3. **More sustained power:** an iPad Pro with an M-series chip.
