# Native ARM64 SteamVR runtime search

For the October 7 update and current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

Native tracking, composition and graphics remain the intended headset path.
The VM's x86-64 SteamVR through FEX is a compatibility baseline. It does not
establish Steam Frame's compositor architecture or headset performance.
Valve documents Proton/FEX for Windows x86 games and forwarding graphics calls
to native libraries in its [Frame compatibility guide](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility?l=english).

On September 11, 2026, the following checks found no obtainable complete ARM64
SteamVR runtime. This is a bounded search result, not proof that none exists.

| Source | Verified result |
|---|---|
| [Valve OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) | Linux/Android ARM64 API loaders are present. The complete tree contains no `vrclient.so`, `vrserver` or `vrcompositor`. |
| [Holo ARM64 preview](https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview) | The public `mash-20251118.3` package databases contain 254 core and 4,306 extra packages; no SteamVR/Deckard package match was found. |
| [SteamOS package index](https://steamdeck-packages.steamos.cloud/archlinux-mirror/) | The inspected Holo main/release repositories expose x86-64 directories. The 178-package Holo main database has no SteamVR/Deckard match. |
| [Public recovery index](https://steamdeck-images.steamos.cloud/recovery/) | Steam Deck images are listed; no Frame recovery image was found. |
| Public GitHub/GitLab searches and unofficial reports | API-loader mirrors and historical ARM Lighthouse references did not provide a complete runtime. |
| Restricted sources | The Holo package root returns HTTP 401. Anonymous access to Lepton app 3029110/depot 3029111 is denied; an earlier SteamVR DLC 4128110 attempt was also denied. No content was obtained from these restricted sources. |

An [independent Lepton inspection](https://utzcoz.github.io/2026/09/03/steam-frame-lepton-architecture.html)
reports `deckard-steamvr-main`/`deckard-steamvr-rel` package names and an Android
`/data/steamvr/runtime/bin/androidarm64/vrclient.so` path. The author explicitly
says the runtime binary was unavailable for inspection. These are leads; the
underlying scripts and runtime were not independently obtained here.

The existing ARM64 Steam client, Proton, OpenVR SDK loaders, Monado and ALVR
are distinct components. None supplies the missing Valve ARM64 runtime merely
by being ARM64. The current ARM64 Wine OpenXR bridge cannot load the installed
x86-64 `vrclient.so`; the ELF evidence is in
`output/steamvr-runtime-architectures/report.json`.

Exact URLs, response statuses, package metadata, hashes and search limits are
preserved in `output/steamvr-arm64-research-v1/report.json`. A newly obtainable
runtime should first be inspected for architecture, matching client/server ABI,
dependencies and provenance, then tested in an isolated VM session. The current
validated x86 runtime and its evidence must remain available for comparison.
