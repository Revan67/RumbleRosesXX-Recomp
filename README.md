# Rumble Roses XX Native Recompilation Research

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange)](#project-status)
[![Platform: Windows](https://img.shields.io/badge/platform-Windows-blue)](#toolchain)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Game data: not included](https://img.shields.io/badge/game%20data-not%20included-brightgreen)](LEGAL.md)

An educational, work-in-progress native PC recompilation of the Xbox 360 version of Rumble Roses XX. XenonRecomp translates the original PowerPC program into C++, while the project runtime supplies the host memory, kernel, filesystem, input, timing, and graphics interfaces expected by that code.

The original CPU/runtime baseline builds and reaches an internal frame loop. An isolated ReXGlue/Xenos hybrid opens a native window and renders recognizable in-engine scenes with controller input and audio. Local tests confirmed two saves and one load. On October 25, 2026, a user reached and played a match with the native nonlocal-return repair and experimental safe shader loads enabled. This is a first-playable checkpoint, not a stable release: full-match completion, clean shutdown and sustained gameplay have not been established.

See the [first playable checkpoint](docs/FIRST_PLAYABLE_CHECKPOINT.md) and [developer build guide](BUILDING.md). A user-supplied-ISO launcher is planned but not yet implemented. Repository participation and any future distribution are governed by [LEGAL.md](LEGAL.md).

Private research, raw traces and machine-specific guidance stay local. Published findings distinguish observed implementation behavior from unverified platform assumptions. See [contribution guidance](CONTRIBUTING.md).

The first playable run used a locally modified ReXGlue GPU plugin with experimental safe relative constant loads. That SDK modification is not included in this source snapshot; a stock dependency build must not be assumed equivalent to the tested build.

## Legal requirements

This repository does not include the game executable, disc image, extracted assets, generated switch-table data, shaders, or XenonRecomp-generated game code. You must provide your own legally obtained copy of the game and generate those files locally. Do not distribute copyrighted game data with builds or source archives. Describing the project as educational does not itself create a copyright or anti-circumvention exception.

## Toolchain

- Visual Studio 2022 with Desktop development with C++ and LLVM/clang-cl
- CMake 3.20 or newer
- Ninja
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), cloned recursively into `tools/XenonRecomp`
- [extract-xiso](https://github.com/XboxDev/extract-xiso), when ISO extraction is required

The Xenia source tree may optionally be cloned into `tools/xenia-reference` for implementation research. It is not part of the build.

## Local layout

Private and generated directories are intentionally ignored:

```text
game/extracted/default.xex   legally supplied input
generated/ppc/               local XenonRecomp output
build/                       local CMake/Ninja output
```

The recompilation configuration is `config/rrxx.toml`. Generate `config/rrxx_switch_tables.toml` locally with XenonAnalyse, then generate the PPC sources from the `config` directory using the command-line form documented by XenonRecomp.

## Build

Run these commands from a Visual Studio Developer PowerShell configured for x64:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl `
  -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --target rrxx_host -- -j2
```

The build deliberately limits large recompilation units to two concurrent compiler jobs and one linker job to avoid exhausting system memory.

## Project status

- XenonRecomp translation units compile and link.
- Guest memory and core runtime services operate.
- The game reaches a sustained internal frame loop.
- A native Windows window and XInput controller path operate.
- The isolated hybrid renders in-engine scenes with audio and controller input.
- Two local saves and one load were confirmed; general save reliability is not verified.
- First playable match confirmed locally on October 25, 2026; earlier CPU and GPU failures remain relevant regression cases.
- The launcher, installer, and playable release are not available yet.

This is experimental research software and is not currently a playable release. Rumble Roses, Xbox, Microsoft, Konami, and their associated names and marks belong to their respective owners. This project is unofficial and is not endorsed by or affiliated with them.
