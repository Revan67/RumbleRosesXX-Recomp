# Contributing

Please open an issue before a substantial change so the interface and scope can be agreed on. Share synthetic reproductions and small factual observations, not game dumps.

- Submit only code and assets you wrote or can license compatibly with MIT. Keep third-party attribution and notices.
- Never submit disc images, XEX/XEXP files, title updates, DLC, extracted game assets, generated translations, switch tables, shaders, keys, certificates, proprietary SDK material, or logs/screenshots containing them.
- Keep game media, generated code, local profiles, logs, and build products in ignored local paths. CI and tests must use synthetic fixtures and must not require a copy of the game.
- Avoid personal paths, identities, credentials, and machine identifiers in commits and diagnostic output.
- Keep resource use bounded. The project uses two compiler jobs by default to avoid excessive memory pressure.
- Document what a change actually verifies. Rendering a scene is not proof of gameplay, save/load, or clean shutdown.
- Do not add downloaders for copyrighted game content or package imported media with a release.

The future launcher should accept user-selected media, validate it locally, show progress and errors, support re-import and cache removal, and keep all imported data outside source and release packages. Original UI work is welcome; do not reuse game artwork or another port's branding.
