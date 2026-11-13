# Importing an external ANR FG runtime

The ANR Vulkan host is proposed separately in [PR #2958](https://github.com/NotRexed/Hookshot/pull/2958). This helper
complements that host: it imports a package the user obtained from Apex into the host's `anr-fg` directory. It does
not add ANR support to an older Hookshot build, download anything, execute an upstream installer or install an
OptiScaler proxy.

## Installation

1. Obtain `anr-fg-V.B15-addon.zip` from the [official FG V.B15 release](https://github.com/apex-compute/ANR/releases/tag/FG-V.B15)
   and review the publisher's terms there. The full ZIP is accepted too; the addon is smaller.
2. Close the game and run this from the Hookshot source/tools folder:

   ```powershell
   python -I scripts/anr_runtime.py --package "C:\Downloads\anr-fg-V.B15-addon.zip" --game "C:\Games\Hookshot"
   ```

3. Use an ANR-enabled build and select **ANR FG** in **Frame generation by**. The host proposed in #2958 may also
   choose ANR at Auto once its files are installed; choose Built-in to keep using Hookshot's own interpolation.

The importer validates the entire ZIP against Apex's published SHA-256, checks the provider DLL separately, verifies
the required model/kernel files, and publishes a complete installation with one directory rename. A failed import
does not publish a partial `anr-fg` directory. Existing installations and settings are never replaced; rename or
remove your own old runtime folder explicitly before importing another copy.

A DLL alone is insufficient. ANR also needs the model programs, adapter and kernel packs, JSON tables and `nr_fg.ini`
from the same release. Only that provider directory is imported. `dxgi.dll`, `winmm.dll`, `OptiScaler.ini` and upstream
installation scripts are not installed or run.

Upstream configuration is preserved byte for byte by default. For a diagnostic portable-kernel setup, add
`--portable`: it changes only `path = portable` and preserves the original as `nr_fg.ini.upstream`. The installation
manifest `hookshot_anr.json` records the release, package/provider hashes and this explicit choice. No game options
are changed by the helper.

Only FG V.B15 full/addon packages are accepted. A future release needs an explicit checksum update and another test;
the helper does not follow a moving "latest" download. It uses Python's standard library and performs no network
requests. The runtime remains external to Hookshot's source, Builder bundles and CI artifacts.

## Distribution decision, checked 2026-11-13

The [ANR README](https://github.com/apex-compute/ANR) lists FG as a beta, SR/RR as upcoming, and ANR's engine source as
coming soon. The reviewed repository, release text and full/addon packages do not provide an explicit redistribution
license for ANR's provider and models. The full package's GPL notice identifies **OptiScaler**; it does not establish
the ANR engine/models' terms. The repository has no top-level ANR license.

For now, distribute the importer source only and have users obtain the official package themselves. Do not bundle or
mirror ANR binaries/models, and do not invent a license-acceptance checkbox: there is no published ANR license text
in the reviewed materials for it to present. Keeping an optional runtime external is not itself a conclusion about
compatibility with Hookshot's GPL obligations.

The preferred future Builder flow is an optional direct download from an official, versioned release, with integrity
checks and the acceptance/notices required by the publisher's actual terms. Before implementing that flow, obtain
clear terms covering automated downloads, independent hosts, runtime/model use and required notices. Bundling ANR
inside Hookshot releases or Builder resources also needs an explicit redistribution grant and maintainer review of
GPL compatibility. The fact that a download is public or that source is planned does not answer those questions.

Until those terms are clear, a Builder action to **import a user-downloaded package** is the appropriate integration
point for this helper; this change adds the command-line helper and does not change the Builder UI or download flow.

## Verification

`tests/test_anr_runtime.py` uses generated fixtures with no game, ANR binaries or network access. It checks package
integrity, completeness, path traversal, Windows reparse points, duplicate paths, Unicode destination paths,
configuration preservation, explicit portable selection and refusal to overwrite an existing installation.

The official V.B15 full and addon packages were also imported locally on Windows 11 build 26300, with their ZIP
hashes checked against the release's `SHA256SUMS.txt`. No game or provider is executed by this test. Runtime rendering
and performance are the host integration's separate tests.
