# First playable checkpoint — 2026-10-25

Evidence status: OBSERVED. Classification: IMPLEMENTATION / TEST RESULT.

The user reached and played a match in the local Windows hybrid build, then
reported an apparent observation timeout without a visible crash. Reviewed
runtime output confirms eight native nonlocal transfers resumed their intended
continuation, with subsequent rendering activity and no recorded CPU fault or
GPU device loss. Safe relative shader constant loads were explicitly enabled.

This is a bounded local milestone, not full-game validation. Full-match
completion, long-session stability, orderly shutdown, rendering fidelity and
general save reliability remain unverified. The shader experiment is not proof
that every GPU failure is fixed. Original parser-error inputs remain unresolved.

## Build identity and conditions

- Windowed executable SHA-256:
  `DC07D81727616227856FF21CE8312B525504514476C1949072786E673CB84C01`.
- GPU plugin SHA-256:
  `67B01543E3FDB076D2520FDE37D62ED79550409AB398495A04D1FA2C8CBB6E8D`.
- Local switch, flag-continuation and native nonlocal boundary overlays enabled.
- SDK safe-relative-constant experiment enabled; not part of the stock SDK.
- Two-job builds; bounded interactive testing with resource safeguards.

The tested executable and dependency DLLs are preserved only in ignored local
checkpoint storage. Game media, generated implementations, captures, saves and
raw research evidence are not part of the source checkpoint or public release.

This document is a sanitized milestone summary. Detailed local provenance is
maintained separately; a source checkout alone does not reproduce the modified
SDK or provide any game content.
