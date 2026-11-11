# Licence

SR2 Launcher is a modified version of [Madeira](https://github.com/willfaust/Madeira), which is
licensed under the GNU General Public License, version 3 or later. Madeira's terms are in its
`LICENSE` (also `COPYING`), `LICENSE-EXCEPTION.md`, `LICENSES/` and `THIRD-PARTY-NOTICES.md`, and the
app shows its licence notices itself.

The modifications, the patches in [`patches/`](patches), are released under the same terms,
GPL-3.0-or-later. So are the other files in this folder and
[`.github/workflows/sr2-launcher.yml`](../.github/workflows/sr2-launcher.yml).

## Corresponding source of an SR2 Launcher IPA

An IPA built by `.github/workflows/sr2-launcher.yml` is made from:

1. Madeira at the commit named in [`UPSTREAM`](UPSTREAM) (tag `v0.2.0`), with its git submodules at
   the commits that commit pins (`FEX`, `dxmt`, `madeira-dock`, `wine`), all public on GitHub;
2. the patches in [`patches/`](patches), applied by [`apply.sh`](apply.sh);
3. the build scripts in Madeira's tree, run as the workflow runs them.

The release notes of each prerelease name the commit of this repository it was built from, which
fixes all three.

Changes: each patch is dated and its message says what it changes and why;
[README.md](README.md) sums them up.

The IPA contains no game files. Slime Rancher 2 is installed separately, by each player, from their
own Steam account.
