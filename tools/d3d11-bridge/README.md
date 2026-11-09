# d3d11 bridge: Direct3D 12 for Slime Rancher 2 in Madeira

A replacement `d3d11.dll` that goes in Slime Rancher 2's game folder. It lets the game's
Direct3D 12 renderer start in Madeira instead of falling back to Direct3D 11.

## Why

With `-force-d3d12`, Unity creates its Direct3D 12 device on Madeira's D3D12 layer, then asks
`d3d11.dll` for a "D3D11On12" device. Madeira's `d3d11.dll` (DXMT) doesn't implement that, so the
game's log shows:

```
d3d12: failed to create D3D11On12 device (0x80004001).
kGfxThreadingModeSplitJobs is not supported on Direct3D 11. Reverting to kGfxThreadingModeClientWorkerJobs instead.
```

and the game runs on Direct3D 11. There, Unity can't use the game's "split" graphics jobs, which
spread rendering work across more processor cores.

## What it does

- Passes `D3D11CreateDevice` and the other ordinary calls straight to the real `d3d11.dll`.
- Answers `D3D11On12CreateDevice` with a plain Direct3D 11 device when the real one can't. Unity
  only uses that device briefly while it starts.
- Guards the Direct3D 12 device's `CheckFeatureSupport`: a capability question Madeira's D3D12
  layer doesn't know yet gets "not supported" instead of `E_INVALIDARG`. Unity 6 treats that error
  as fatal for `D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT`. Queries where the error is a real answer
  (formats, shader model, root signature version, feature level list) are passed through
  unchanged.
- Never writes to Madeira's memory. For the guard it copies the device's function table into the
  bridge, swaps `CheckFeatureSupport` in the copy, and points the device object at the copy.
  Version 2 patched the table in place instead. Madeira runs madeira-d3d12 from a copy in its JIT
  pool, and changing that page's protection (`VirtualProtect`) left the whole table reading as
  zeros. The game then crashed calling `Release` through it (address 0).
- Finds the real `d3d11.dll` the way Madeira's loader does: `system32` first, then
  `C:\windows\sysx64`. In Madeira, `system32` holds an ARM64 build an x64 game can't load, and the
  usable one is in `sysx64`.
- Writes what it did to `C:\sr2-d3d12-bridge.log` (at most 300 lines).

The idea, a plain D3D11 device for Unity's D3D11On12 call plus a feature-query guard, was first
shown working for a Unity 6 game on Apple's D3DMetal. This is a separate implementation for
Madeira.

## Install, check and undo

1. Copy `d3d11.dll` into the game folder, next to `SlimeRancher2.exe`:
   **Madeira › wine › drive_c › Program Files (x86) › Steam › steamapps › common › Slime Rancher 2**
2. Keep `-force-d3d12` in the game's **Launch arguments** in Madeira.
3. Play. The first launch converts the game's Direct3D 12 shaders, so loading is slower and the
   first minutes stutter. Madeira keeps the converted shaders, so judge speed on the second launch.

It worked if `C:\sr2-d3d12-bridge.log` shows `standalone D3D11 device for D3D11On12CreateDevice
-> 0`, and the game's log no longer shows `failed to create D3D11On12 device` or `Reverting to
kGfxThreadingModeClientWorkerJobs`.

To undo, delete `d3d11.dll` from the game folder. Nothing else is changed.

`env.SR2_BRIDGE_GUARD = 0` in the game's config turns the feature-query guard off and keeps the
rest, to tell a guard problem from a D3D12 problem.

## Build and test

```sh
sh build.sh          # MinGW-w64: apt-get install gcc-mingw-w64-x86-64
sh test/run.sh       # Wine 64-bit: apt-get install wine64
sh test/run.sh wine  # same checks against Wine's own d3d11.dll
```

`test/run.sh` loads the bridge in Wine with Madeira's folder layout. A fake D3D12 device built like
madeira-d3d12's stands in for it: one object answering up to `ID3D12Device8`, with a 79-slot table
in a read-only page. `test/fake_d3d11.c` stands in for DXMT. The test checks:

- that the D3D12 layer's table is never written, which version 2 fails;
- the device that comes back, the dropped debug flag, and the retry when only 12_x feature levels
  are requested;
- which feature queries the guard changes and which it leaves alone;
- balanced references, and the `SR2_BRIDGE_GUARD=0` switch.

The build is reproducible: `--no-insert-timestamp` leaves no build time in the file.
