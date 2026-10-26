# Legal and Repository Policy

This policy is deliberately conservative. It reduces avoidable risk but is not legal advice and cannot guarantee that a particular reverse-engineering or distribution activity is lawful. Laws differ by jurisdiction. Obtain advice from a qualified intellectual-property attorney before making the repository public or distributing binaries.

## Scope

This repository is for independently written interoperability research and host-runtime code. It must not be used as a delivery mechanism for Rumble Roses XX, Xbox 360 system software, or any other third-party copyrighted material.

Calling the project educational, noncommercial, or a preservation effort does not automatically make copying, circumvention, or distribution lawful.

## Material that must never be committed or released

- Disc images, XEX/XEXP files, title updates, DLC, extracted archives, executable sections, or game assets
- XenonRecomp-generated translations of the game executable
- Generated jump/switch tables or bulk address/function databases derived from the executable
- Extracted or translated game shaders, shader caches, textures, models, animation, audio, video, fonts, text, or localization data
- Xbox keys, certificates, credentials, device secrets, decryption material, or instructions whose purpose is defeating access controls
- Proprietary SDK headers, libraries, symbols, documentation, or leaked source code
- Crash dumps, traces, memory dumps, logs, screenshots, or test packages that embed substantial game data
- Installers or archives that bundle any of the above

## Permitted repository content

- Independently authored host-runtime and compatibility code
- Build scripts and configuration that contain no third-party expressive content
- Small factual constants and API declarations only when necessary for interoperability and legally reviewed if there is doubt
- Documentation describing architecture, testing, and how users supply their own lawful input locally
- Unmodified third-party open-source dependencies used in compliance with their licenses, preferably referenced by URL and pinned revision

## Development rules

1. Contributors must use a lawfully obtained copy and retain it only in ignored local paths.
2. Extraction, analysis, code generation, and shader conversion occur locally; their outputs remain untracked.
3. Do not upload game files to issues, pull requests, CI artifacts, caches, logs, or release attachments.
4. Do not paste decompiled game logic or substantial generated output into handwritten source as a workaround.
5. Record implementation behavior in original words. Prefer clean-room-style interface notes that describe inputs, outputs, and observable behavior without copying implementation expression.
6. Publish only reviewed, independently authored source and approved open-source material. Keep confidential development evidence local; a public source repository does not authorize publishing game-derived payloads.
7. Do not publish binaries, installers, patches containing copyrighted bytes, or automated data-acquisition tooling without a separate legal and security review.
8. Stop and escalate any uncertain material before committing it. Removal after publication may not remove copies, forks, caches, or prior clones.

## Contribution certification

By contributing, a contributor represents that the contribution is their original work or is used under a compatible license; that they have the right to submit it; and that it contains none of the prohibited material above. No contributor should submit confidential employer information or material obtained under an NDA.

## Licensing status

Independently authored project code is available under the MIT license in LICENSE. Third-party dependencies retain their respective licenses and notices; MIT does not relicense them or any game content. This repository claims no ownership of the game, Xbox 360 software, trademarks, or other third-party works.

## Project identity

Rumble Roses, Xbox, Microsoft, Konami, and all associated names, marks, characters, and assets are the property of their respective owners. This project is unofficial, is not endorsed by them, and should not use official logos or presentation that implies affiliation.
