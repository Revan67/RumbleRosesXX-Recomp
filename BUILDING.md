# Local developer build (Windows x64)

This guide builds from a contributor's own lawful game copy. It does not download, include, or redistribute the game. Build products and generated game code remain ignored and local. A first playable match was observed in a local build on October 25, 2026; this remains experimental, not a packaged playable release. See the limitations below before comparing a stock dependency build to that checkpoint.

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
foreach ($patch in Get-ChildItem patches/rexglue-sdk -Filter '*.patch' | Sort-Object Name) {
  git -C build/rexglue-sdk apply --check $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "SDK patch check failed: $($patch.Name)" }
  git -C build/rexglue-sdk apply $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "SDK patch failed: $($patch.Name)" }
}
Push-Location build/rexglue-sdk
try {
  cmake --preset win-amd64
  cmake --build --preset win-amd64-release --target rexruntime rexgpu-xenos --parallel 2
} finally { Pop-Location }
```

The SDK should produce libraries under `build/rexglue-sdk/out/win-amd64`, DLLs there or under its `Release` subdirectory, plus the generated header `out/build/win-amd64/include/rex/version.h`. ReXGlue and its GPU plugin remain separate dependencies under their own license and notices; they are not copied into this repository.

If a clean SDK places `rexruntime.lib` and `fmt.lib` in `out/win-amd64/Release`,
set `REXSDK_LIB` to that directory in the host configure command. For an isolated
host build, point `REXSDK_SOURCE`, `REXSDK_BUILD`, `REXSDK_LIB`, and
`REXSDK_RUNTIME_DIR` at the same SDK build and use a separate host output
directory. Do not mix import libraries from one build with DLLs from another.

When overriding compiler locations in PowerShell, pass each entire CMake cache
assignment as one argument, for example `"-DCMAKE_CXX_COMPILER=$clangPath"`.
Do not build a nested shell command with embedded compiler-path quotes: a path
containing spaces can otherwise become a broken `C:/Program` cache entry. If a
configure attempt already produced that malformed cache, preserve it for
diagnosis and configure a separate build directory with `-B`; do not reuse the
damaged compiler-detection files or delete a known-good build.

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

The current host requires locally generated New Game, save-menu, and dialog boundaries. The fade boundary is needed for the observed progression. Find the units containing those implementations without copying them into tracked source:

```powershell
$boundaryUnits = Get-ChildItem build/hybrid-ppc -Filter 'ppc_recomp.*.cpp' -File
$fade = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_82062200) {' -List | Select-Object -First 1).Path
$newGame = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_8214E948) {' -List | Select-Object -First 1).Path
$saveMenu = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_82129680) {' -List | Select-Object -First 1).Path
$dialog = ($boundaryUnits | Select-String -SimpleMatch 'PPC_FUNC_IMPL(__imp__sub_821D1810) {' -List | Select-Object -First 1).Path
if (!$fade -or !$newGame -or !$saveMenu -or !$dialog) { throw 'Required local boundary unit missing; check the generated output.' }

cmake -S hybrid -B build/hybrid -G Ninja `
  -DCMAKE_CXX_COMPILER=clang-cl `
  -DREXSDK_SOURCE="$PWD/build/rexglue-sdk" `
  -DREXSDK_BUILD="$PWD/build/rexglue-sdk/out/build/win-amd64" `
  -DRRXX_PPC_DIR="$PWD/generated/ppc" `
  -DRRXX_TRANSLATED_MEMORY=ON `
  -DRRXX_FADE_BOUNDARY_INPUT="$fade" `
  -DRRXX_NEW_GAME_BOUNDARY_INPUT="$newGame" `
  -DRRXX_SAVE_MENU_BOUNDARY_INPUT="$saveMenu" `
  -DRRXX_DIALOG_BOUNDARY_INPUT="$dialog"
cmake --build build/hybrid --target rrxx_windowed --parallel 2
```

If CMake selects a non-Windows `ld` instead of the Visual Studio linker, add `-DCMAKE_LINKER="$((Get-Command lld-link).Source)"` to the configure command. The linker map and DLLs in `build/hybrid/` are local build products, not release assets. Do not upload them until binary redistribution and third-party license obligations have been reviewed.

## 5. Run only as a bounded local experiment

The executable accepts an extracted-game directory and a writable local-state directory. Local testing has saved twice and loaded once, and one bounded match was played with additional local corrections and a modified GPU plugin. Crash recovery, full-match completion and orderly shutdown are not proven. Keep experiments time- and resource-bounded, and retain all local logs, saves, and captures outside Git. The planned launcher and user-facing ISO import flow do not exist yet.

For a short menu-only smoke test, run `./tools/test_windowed_boot.ps1 -ObservationSeconds 30 -MemoryLimitGiB 2 -LogLimitMiB 16` from this repository's root. The extended-test defaults are 8 GiB working set and 64 MiB of text logs; both limits can be lowered explicitly. Logs stay under ignored `build/hybrid/`. The timer defaults to 120 seconds and accepts up to 1800 seconds. These process limits cannot prevent a GPU timeout. Interactive GPU-risk testing requires an available user; do not repeatedly provoke device loss.

## 6. First-playable boundary corrections and limits

The handwritten preparation tools produce private overlays from your own local
input. Run with PowerShell 7 (the scripts use modern .NET path APIs):

```powershell
./tools/prepare_flag_boundary.ps1
./tools/prepare_nonlocal_boundaries.ps1
```

The switch preparation additionally requires a private mapped executable image at
`game/rrxx_image.bin`, loaded at guest base 0x82000000, matching the translated XEX.
That artifact is not an ISO or an arbitrary XEX file; do not rename either to
satisfy this requirement. Its creation is not automated in this snapshot.
When that validated local image is available, run:

```powershell
./tools/prepare_switch_boundaries.ps1
cmake -S hybrid -B build/hybrid `
  -DRRXX_SWITCH_BOUNDARY_DIR="$PWD/build/research/switch-boundaries" `
  -DRRXX_FLAG_BOUNDARY_SOURCE="$PWD/build/research/flag-boundary/sub_8236ABE8.cpp" `
  -DRRXX_NONLOCAL_BOUNDARY_DIR="$PWD/build/research/nonlocal-boundaries"
cmake --build build/hybrid --target rrxx_windowed rrxx_nonlocal_test --parallel 2
./build/hybrid/rrxx_nonlocal_test.exe
```

The nonlocal bridge executes the original guest register restoration, then
unwinds native frames to the matching active save continuation. It stops on an
unknown target rather than returning through invalid native frames. This is a
host compatibility repair, not a claim of complete Xbox context equivalence.

Runtime DLLs are synchronized every windowed build. If SDK DLLs are in a
`Release` subdirectory, set `REXSDK_RUNTIME_DIR` to that directory.

**Reproducibility limit:** the successful local match also used the experimental
SDK shader change `dxbc_safe_relative_constants`. It is now preserved in the
[pinned SDK patch package](patches/rexglue-sdk/README.md), applied in step 2.
The patches reproduce all 16 modified source blobs from the pinned Git tree.
A separate clean SDK Release compilation from that package now passes with
Clang 19.1.5 and two jobs, using existing dependency sources. A separate host
build using those DLLs passed offline tests and was used for user-confirmed
save/load, match play and a later restart/load check. This is narrow route
coverage, not full-game correctness or a turnkey independent installation test.
Stock SDK builds without the patches do not supply this option.

The GPU draw journal and GPU validation option are included in the SDK patch
package. Full guest tracing also requires the host's trace-enabled build option;
launcher flags alone do not add instrumentation to a stock dependency. Never
upload captured payloads to GitHub.

With the patched SDK and all local overlays built, the checkpoint route uses:

```powershell
./tools/test_windowed_boot.ps1 -ObservationSeconds 120 -GpuDebug -SafeRelativeConstants -ContextTrace
```

Arrange an attended test; desktop disruption remains possible. This command
does not itself prove arrival in gameplay or clean termination.

For the local clean-SDK validation build, add `-CleanSdkCandidate` to explicitly
select `build/research/host-clean-sdk-validation`. It verifies the adjacent DLLs
against the isolated SDK build record and keeps saves, caches and logs in the
candidate directory. It does not copy existing saves. Add `-PreflightOnly` to
check paths, identities and limits without starting the game. Omitting the
candidate switch preserves the existing default build selection.

Keep `-CleanSdkCandidate` when reopening that candidate's existing saves; omit
only `-FreshState`. Omitting the candidate switch selects the older build and
its different state directory. The candidate option is a local development
convenience and requires the corresponding isolated build and local SDK identity
record; it is not a public packaged installation.
