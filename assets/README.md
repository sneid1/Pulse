# assets/

Runtime game assets for PULSE.

This directory is intentionally kept in line with the asset tree that
`Package.bat` copies into `dist/assets`. If an asset is required to build, run, or
package the game, it belongs here. If it is a raw download, authoring source,
cache, preview, backup, capture, or obsolete export, it should stay ignored and
untracked.

Large binary assets are stored through Git LFS. After cloning:

```bat
git lfs pull
```

## Runtime Layout

- `audio/` - shipped music stems, ambience, weapon banks, enemy tells, UI, and
  player-feedback SFX.
- `bumstrum/` - authored first-person weapon rigs used by `config/pulse.weapons`.
- `external/` - selected runtime third-party material and sci-fi kit assets.
- `fonts/` - UI font data.
- `icons/` - Windows/game icons.
- `meshy/` - runtime enemy, boss, and relic/slice assets.
- `models/` - small static fallback meshes.
- `packs/pulse_environment/` - promoted arena environment assets.
- `shaders/` - HLSL shader sources compiled by the engine.
- `textures/` - core runtime texture data.

## Source Assets

Authoring inputs are kept out of the committed runtime set unless the package
script needs them directly. Local-only folders such as raw audio bundles, Blender
workspaces, source Meshy output, reference downloads, captures, and generated
previews are covered by `.gitignore`.

When adding assets, use `Package.bat` as the source of truth: promote only the
files that the game opens or the package step copies into `dist/assets`, and keep
the rest local.
