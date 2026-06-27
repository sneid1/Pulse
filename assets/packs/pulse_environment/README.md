# Pulse Environment Runtime Pack

This folder is the single runtime source of truth for arena environment art used by the game.

Authoring/import sources can live elsewhere while they are being generated, but the game should
load environment and arena enemy assets from this pack:

- `quaternius/` - Quaternius runtime kits used by the arena shell and enemy rigs.
- `meshy/common/` - shared generated props with fixed integration slots.
- `meshy/foundry/`, `meshy/furnace/`, `meshy/reliquary/` - biome-specific generated props.
- `meshy/shared/` - generated props allowed in every biome.
- `base_assets/` - promoted base Meshy environment GLBs and references used as structural
  dressing accents by the runtime.
- `manifests/` - copied source manifests/status files for traceability.

Promote generated assets into this pack with:

```powershell
.\tools\assets\Sync-PulseEnvironmentAssets.ps1
```

The game runtime should not reference `New Models/Asset kits/...` directly.
