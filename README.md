# MiniSkirt

An SKSE plugin for Skyrim SE / AE / VR that shortens the long skirts, dresses and robes worn by female NPCs into miniskirts at runtime, and fills in the legs below the new hem from the NPC's own body. One DLL works on SE, AE and VR (CommonLibSSE NG, runtime detection).

The mod itself, its requirements and the installation steps are on the Nexus Mods page. This repository is the source code.

## Building

Requirements:
- Visual Studio 2022 with "Desktop development with C++" (MSVC, CMake, Ninja)
- [vcpkg](https://github.com/microsoft/vcpkg) (a clone; set `VCPKG_ROOT` to it)
- .NET SDK 8 or later (for `espgen`, which writes `MiniSkirt.esp`)
- PowerShell 7 (`pwsh`) for `package.ps1`

```
git clone --recursive https://github.com/pentarow55/MiniSkirt
cd MiniSkirt
copy local.example.ps1 local.ps1   # then set VCPKG_ROOT in local.ps1
pwsh -File package.ps1 -Draft      # release build + zip in dist\
```

| Script | Preset | Output | Notes |
|---|---|---|---|
| `package.ps1` | `release` | `build-release\`, `dist\MiniSkirt-<version>.zip` | SE/AE/VR DLL without dev tools. This is what is released |
| `build-multi.ps1` | `multi` | `build-multi\` | SE/AE/VR DLL with dev tools |
| `build.ps1` | `vr` | `build\` | VR-only DLL with dev tools; copies to `MINISKIRT_DEPLOY_DIR` if set |

CommonLibSSE NG is a git submodule (`extern/CommonLibVR`, pinned to v9.0.0). If you cloned without `--recursive`, run `git submodule update --init --recursive`.

Other tools:
- `tools/mcm_i18n.py` writes the MCM page (`assets/MCM/Config/MiniSkirt/config.json`) and the translation files.
- `tools/gen_notices.py` writes the license notices in `assets/SKSE/Plugins/MiniSkirt_Licenses/` (run after a `build-multi.ps1` build).

## License

GPL-3.0-or-later (see `LICENSE`). CommonLibSSE NG is GPL-3.0-or-later with the Modding Exception and the GPL-3.0 Linking Exception. Third-party notices are in `assets/SKSE/Plugins/MiniSkirt_Licenses/`.
