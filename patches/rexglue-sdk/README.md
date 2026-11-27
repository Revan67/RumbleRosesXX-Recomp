# ReXGlue SDK local checkpoint patches

Upstream: https://github.com/rexglue/rexglue-sdk

Exact base: `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`.
Apply these patches in numeric order to a clean checkout of that revision.

These are source changes used by the local first-playable configuration,
not game data or a replacement SDK distribution. Upstream-derived material
retains its BSD license and notices in [LICENSE.rexglue](LICENSE.rexglue).
The repository's MIT license does not replace those terms. No dependency
binaries, game shaders, captures, guest instructions or generated game bodies
are supplied here.

- `0001`: CPU fault observation; logs register state and module-relative fault
  location without changing access-violation handling results.
- `0002`: GPU device-loss observation, bounded indexed-draw journal and optional
  GPU-based validation. The journal is opt-in via `RRXX_DRAW_JOURNAL`; retain
  its output locally. Diagnostics can affect timing and resource use.
- `0003`: experimental `dxbc_safe_relative_constants` option, default off and
  fixed for each translator lifetime. Relative float constant reads outside
  the allocated constant range return zero. Valid indices are not clamped.
  This changes generated host shader behavior; it is not established Xbox
  hardware behavior or a complete model of all constant-register semantics.

## Validation boundary

OBSERVED / IMPLEMENTATION / TEST RESULT: all 16 affected source blobs were
reconstructed exactly from the pinned upstream Git tree using these three
patches in an isolated index. The active SDK checkout and its index were not
changed. `tools/export_sdk_checkpoint.ps1` repeats this check against the local
checkpoint sources and emits an ignored provenance manifest.

The source changes had previously built in the local SDK and were present in
the user-confirmed playable run. A separate clean Release build from this patch
package now passes for `rexruntime` and `rexgpu-xenos` with Clang 19.1.5 and two
jobs; a second build reports no work. Both DLLs were inspected statically as
x64. Existing dependency sources were reused, not previous build objects.
The new DLLs were subsequently linked/deployed only into a separate host
candidate, not over the preserved working build. The user confirmed save/load
and match play there, then confirmed a saved state loaded after restart.
Full-game, visual correctness and orderly-shutdown validation remain incomplete.
The host's CPU return repair is also required; shader bounds checking alone is
not a demonstrated explanation for every earlier GPU failure.

Submodule changes are excluded. Fifteen local `libmspack` files were verified
byte-identical to their original symlink targets; those Windows materialization
differences are not independent algorithm patches.
