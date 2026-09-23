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

This is a developer build, not a one-click setup. It targets Windows with Visual Studio 2022, clang-cl, CMake, and Ninja. The complete [bring-your-own-game build guide](BUILDING.md) pins the open-source tool versions and covers local analysis, generation, boundaries, and the windowed build. Cloning the repository alone cannot produce a runnable game.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Useful areas include original launcher UI, accessible import flows, save/profile compatibility, and synthetic tests that require no copyrighted game files. The project-authored code is under [MIT](LICENSE). This project is unofficial and is not affiliated with Konami or Microsoft; game names and marks belong to their owners.
