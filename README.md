# Rumble Roses XX Native Recompilation

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange)](#status)
[![Platform: Windows](https://img.shields.io/badge/platform-Windows-blue)](#local-build)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Game data: not included](https://img.shields.io/badge/game%20data-not%20included-brightgreen)](#game-content-policy)

An independent, work-in-progress PC recompilation of *Rumble Roses XX*. This public repository contains the handwritten hybrid port host and build glue. A user-friendly launcher is planned for later; there is **no playable release or installer** today.

## Status

The local development build has rendered recognizable in-engine scenes with audio and controller input. Save/load is not verified: selecting Options → Data Save currently makes the menu unresponsive. Clean shutdown and sustained gameplay are also unverified. The public source is an experimental development snapshot, not a packaged game.

## Game content policy

No disc image, `default.xex`, DLC, extracted asset, generated PowerPC translation, switch table, shader, key, or proprietary SDK file is included. Contributors must supply their own legally obtained game copy and perform extraction and code generation locally. Do not upload game content or game-derived dumps to this repository, its issues, CI, or releases. The launcher must eventually accept user-selected media and keep import local; it must never download or bundle the game.

## Local build

This is a developer build, not a one-click setup. It currently targets Windows with Visual Studio 2022, clang-cl, CMake, and Ninja. You will also need a locally built [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) with the Xenos D3D12 GPU plugin, and locally generated XenonRecomp output under `generated/ppc`. The optional single-function boundary units used by the current development build must likewise be generated locally; they are not redistributable.

From an x64 Visual Studio Developer PowerShell, after preparing those private inputs:

```powershell
cmake -S hybrid -B build/hybrid -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DREXSDK_SOURCE="$PWD/build/rexglue-sdk" `
  -DREXGPU_SOURCE="$PWD/build/rexglue-d3d12" `
  -DRRXX_PPC_DIR="$PWD/generated/ppc"
cmake --build build/hybrid --target rrxx_windowed --parallel 2
```

The private game files and generated code are intentionally absent, so cloning this repository alone will not produce a runnable game. `config/rrxx_hybrid.toml` is an example of the local XenonRecomp configuration; its paths and the separate switch-table file must be supplied locally. The optional `RRXX_FADE_BOUNDARY_INPUT` and `RRXX_NEW_GAME_BOUNDARY_INPUT` CMake paths accept locally generated boundary units needed for the observed development progression. Do not commit their inputs or build outputs.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Useful areas include original launcher UI, accessible import flows, save/profile compatibility, and synthetic tests that require no copyrighted game files. The project-authored code is under [MIT](LICENSE). This project is unofficial and is not affiliated with Konami or Microsoft; game names and marks belong to their owners.
