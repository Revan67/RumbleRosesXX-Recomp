# Local developer build (Windows x64)

This guide builds from a contributor's own lawful game copy. It does not download, include, or redistribute the game. Build products and generated game code remain ignored and local. The result is an experimental runtime, not a playable release; saves and clean shutdown are unverified.

Run commands from this repository's root in an **x64 Visual Studio 2022 Developer PowerShell**. Install Visual Studio C++ and LLVM/clang-cl components, CMake 3.25+, Ninja, and Git. Verify `clang-cl --version`, `cmake --version`, and `ninja --version` before starting. Keep generated-code builds to two jobs to limit memory pressure.

## 1. Prepare private input

Extract your own supported Rumble Roses XX disc locally, then place the extracted tree at `game/extracted/` with `game/extracted/default.xex`. The disc image, XEX, DLC, and extracted assets must never be committed, uploaded to CI, or attached to an issue. [extract-xiso](https://github.com/XboxDev/extract-xiso) is one possible local extraction tool; follow its own documentation and applicable law. No title update or DLC is part of this build recipe.

## 2. Build the open-source dependencies

The following revisions match the development stack used for this snapshot. The checkouts stay in ignored paths.

```powershell
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git tools/XenonRecomp
git -C tools/XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
git -C tools/XenonRecomp submodule update --init --recursive
cmake -S tools/XenonRecomp -B tools/XenonRecomp/build -G Ninja `
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build tools/XenonRecomp/build --target XenonAnalyse XenonRecomp --parallel 2

git clone --recursive https://github.com/rexglue/rexglue-sdk.git build/rexglue-sdk
git -C build/rexglue-sdk checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git -C build/rexglue-sdk submodule update --init --recursive
Push-Location build/rexglue-sdk
try {
  cmake --preset win-amd64
  cmake --build --preset win-amd64-release --target rexruntime rexgpu-xenos --parallel 2
} finally { Pop-Location }
```

The SDK should produce `build/rexglue-sdk/out/win-amd64/rexruntime.lib`, `rexruntime.dll`, `rexgpu-xenos.dll`, and `fmt.lib`, plus the generated header `out/build/win-amd64/include/rex/version.h`. ReXGlue and its GPU plugin remain separate dependencies under their own license and notices; they are not copied into this repository.

## 3. Analyze and translate your game locally

XenonRecomp's [documented CLI](https://github.com/hedge-dev/XenonRecomp#usage) takes a TOML config and `ppc_context.h`; XenonAnalyse writes a local switch-table TOML. Both configurations here resolve their relative paths from `config/`. The switch table and generated code are ignored because they are derived from your game copy.

```powershell
New-Item -ItemType Directory -Force generated/ppc,build/hybrid-ppc | Out-Null
& tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse.exe `
  game/extracted/default.xex config/rrxx_switch_tables.toml
& tools/XenonRecomp/build/XenonRecomp/XenonRecomp.exe `
  config/rrxx.toml tools/XenonRecomp/XenonUtils/ppc_context.h
& tools/XenonRecomp/build/XenonRecomp/XenonRecomp.exe `
  config/rrxx_hybrid.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

Check that `generated/ppc/ppc_func_mapping.cpp`, `generated/ppc/ppc_recomp_shared.h`, and `build/hybrid-ppc/ppc_recomp.*.cpp` exist before building. XenonAnalyse is not guaranteed to discover every jump table for every game revision; if translation fails, record the tool revision and a non-copyrighted error summary, not the game dump or generated source. These configs are for the locally tested release, not arbitrary title updates.

## 4. Configure and compile the host

The current host requires the locally generated New Game boundary, and the fade boundary is needed for the observed progression. Find the units containing those implementations without copying them into tracked source:

```powershell
$boundaryUnits = Get-ChildItem build/hybrid-ppc -Filter 'ppc_recomp.*.cpp' -File
$fade = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_82062200) {' -List | Select-Object -First 1).Path
$newGame = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_8214E948) {' -List | Select-Object -First 1).Path
if (!$fade -or !$newGame) { throw 'Required local boundary unit missing; check the generated output.' }

cmake -S hybrid -B build/hybrid -G Ninja `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DREXSDK_SOURCE="$PWD/build/rexglue-sdk" `
  -DREXSDK_BUILD="$PWD/build/rexglue-sdk/out/build/win-amd64" `
  -DRRXX_PPC_DIR="$PWD/generated/ppc" `
  -DRRXX_FADE_BOUNDARY_INPUT="$fade" `
  -DRRXX_NEW_GAME_BOUNDARY_INPUT="$newGame"
cmake --build build/hybrid --target rrxx_windowed --parallel 2
```

If CMake selects a non-Windows `ld` instead of the Visual Studio linker, add `-DCMAKE_LINKER="$((Get-Command lld-link).Source)"` to the configure command. The linker map and DLLs in `build/hybrid/` are local build products, not release assets. Do not upload them until binary redistribution and third-party license obligations have been reviewed.

## 5. Run only as a bounded local experiment

The executable accepts an extracted-game directory and a writable local-state directory. For now, do not treat it as a normal game installation: save/load, crash recovery, and orderly shutdown are not proven. Keep experiments time- and resource-bounded, and retain all local logs and captures outside Git. The planned launcher and user-facing ISO import flow do not exist yet.
