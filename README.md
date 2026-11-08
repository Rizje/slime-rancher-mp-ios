# Slime Rancher 2 multiplayer on iOS

Goal: play Slime Rancher 2 with multiplayer entirely on iPhones, on the same Wi-Fi, with no PC.

## Approach

There is no official iOS version of Slime Rancher 2. The only way to run it on the device is
[Madeira](https://github.com/willfaust/Madeira), which runs the unmodified Windows build on the
iPhone. It does this with Wine (Windows compatibility), FEX-Emu (x86-to-ARM translation, which needs
JIT) and DXMT (Direct3D 11 to Metal).

Multiplayer comes from the [Ranching Together](https://github.com/pyeight/SlimeRancher2Multiplayer)
mod (GPL-3.0). It is a MelonLoader mod that talks over plain UDP, so iPhones running the game in
Madeira can connect to each other over the same Wi-Fi.

Nobody has reported running Slime Rancher 2 on Madeira yet, so the first step is a test on one phone.

## Status

| Phase | What | State |
|---|---|---|
| 1 | Test on one phone: does the plain game run well enough? | Next: [docs/phase-1-on-device-test.md](docs/phase-1-on-device-test.md) |
| 2 | Add MelonLoader and Ranching Together on one phone | After phase 1 passes |
| 3 | Two phones, same Wi-Fi, one hosts and one joins | After phase 2 passes |
| 4 | Polish: touch layout, settings preset, setup guide, fixes sent to Madeira | Later |

## Ground rules

- Every player owns their own copy of Slime Rancher 2 on Steam.
- Never commit game files, saves or anything copied out of the game to this repository.
- Mods stay non-commercial, as Monomi Park's Fan Content Policy requires.
