param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)

$ErrorActionPreference = "Stop"

function Join-RepoPath {
    param([string]$RelativePath)
    return Join-Path $RepoRoot $RelativePath
}

function Test-AnyToken {
    param(
        [string]$Text,
        [string[]]$Tokens
    )
    foreach ($token in $Tokens) {
        if ($Text.Contains($token)) { return $true }
    }
    return $false
}

function Copy-IfChanged {
    param(
        [System.IO.FileInfo]$Source,
        [string]$Destination
    )
    $destDir = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destDir)) {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }

    $shouldCopy = $true
    if (Test-Path -LiteralPath $Destination) {
        $destInfo = Get-Item -LiteralPath $Destination
        $shouldCopy = ($destInfo.Length -ne $Source.Length) -or ($destInfo.LastWriteTimeUtc -lt $Source.LastWriteTimeUtc)
    }

    if ($shouldCopy) {
        Copy-Item -LiteralPath $Source.FullName -Destination $Destination -Force
        return $true
    }
    return $false
}

function Get-RelativeFilePath {
    param(
        [string]$BasePath,
        [string]$ChildPath
    )

    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    if (-not $baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar.ToString())) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $childFull = [System.IO.Path]::GetFullPath($ChildPath)
    $baseUri = New-Object System.Uri($baseFull)
    $childUri = New-Object System.Uri($childFull)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($childUri).ToString()).Replace("/", "\")
}

function Sync-DirectoryFiles {
    param(
        [string]$SourceRoot,
        [string]$DestinationRoot
    )

    if (-not (Test-Path -LiteralPath $SourceRoot)) {
        Write-Host "[assets] missing source: $SourceRoot"
        return @{ Seen = 0; Copied = 0 }
    }

    $seenLocal = 0
    $copiedLocal = 0
    New-Item -ItemType Directory -Force -Path $DestinationRoot | Out-Null

    Get-ChildItem -LiteralPath $SourceRoot -Recurse -File | ForEach-Object {
        $seenLocal++
        $rel = Get-RelativeFilePath $SourceRoot $_.FullName
        $dest = Join-Path $DestinationRoot $rel
        if (Copy-IfChanged $_ $dest) { $copiedLocal++ }
    }

    return @{ Seen = $seenLocal; Copied = $copiedLocal }
}

function Get-RoundMetadata {
    param([string]$CsvPath)

    $byId = @{}
    $byName = @{}
    if ([string]::IsNullOrWhiteSpace($CsvPath)) {
        return @{ ById = $byId; ByName = $byName }
    }
    if (-not (Test-Path -LiteralPath $CsvPath)) {
        return @{ ById = $byId; ByName = $byName }
    }

    foreach ($row in (Import-Csv -LiteralPath $CsvPath)) {
        if ($row.id) { $byId[$row.id] = $row }
        if ($row.name) { $byName[$row.name.ToLowerInvariant()] = $row }
    }
    return @{ ById = $byId; ByName = $byName }
}

function Get-AssetMetadata {
    param(
        [string]$AssetDirName,
        [hashtable]$Metadata
    )

    $id = ""
    $name = $AssetDirName
    if ($AssetDirName -match "^(\d+)[_-](.+)$") {
        $id = $Matches[1]
        $name = $Matches[2]
    }

    if ($id -and $Metadata.ById.ContainsKey($id)) { return $Metadata.ById[$id] }
    $key = $name.ToLowerInvariant()
    if ($Metadata.ByName.ContainsKey($key)) { return $Metadata.ByName[$key] }
    return $null
}

function Get-AssetRoute {
    param(
        [string]$AssetDirName,
        $MetadataRow
    )

    $bits = @($AssetDirName)
    if ($MetadataRow) {
        foreach ($field in @("category", "name", "target_use", "display_name")) {
            if ($MetadataRow.$field) { $bits += $MetadataRow.$field }
        }
    }
    $key = (($bits -join " ").ToLowerInvariant() -replace "-", "_")

    if (Test-AnyToken $key @("door_side", "door side", "machinery_stack")) {
        return @{ Bucket = "common"; Role = "door_side" }
    }
    if (Test-AnyToken $key @("door_threshold", "threshold")) {
        return @{ Bucket = "common"; Role = "door_threshold" }
    }
    if (Test-AnyToken $key @("door_lintel", "overhead_lintel", " lintel")) {
        return @{ Bucket = "common"; Role = "door_lintel" }
    }
    if (Test-AnyToken $key @("wall_seam", "seam_cover")) {
        return @{ Bucket = "common"; Role = "wall_seam" }
    }
    if (Test-AnyToken $key @("wall_alcove", "maintenance_alcove", " alcove")) {
        return @{ Bucket = "common"; Role = "wall_alcove" }
    }
    if (Test-AnyToken $key @("base_trim", "wall_base_trim")) {
        return @{ Bucket = "common"; Role = "base_trim" }
    }
    if (Test-AnyToken $key @("floor_access_hatch", "access_hatch")) {
        return @{ Bucket = "common"; Role = "floor_hatch" }
    }
    if (Test-AnyToken $key @("ceiling_service_spine", "ceiling_spine", "service_spine")) {
        return @{ Bucket = "common"; Role = "ceiling_spine" }
    }
    if (Test-AnyToken $key @("ceiling_duct", "duct_cluster", "cable_drop")) {
        return @{ Bucket = "common"; Role = "ceiling_duct" }
    }
    if (Test-AnyToken $key @("underside_support", "deck_support", "support_truss")) {
        return @{ Bucket = "common"; Role = "deck_support" }
    }
    if (Test-AnyToken $key @("stair_finisher", "stair_side", "stair_cheek")) {
        return @{ Bucket = "common"; Role = "stair_finisher" }
    }

    $bucket = "shared"
    if (Test-AnyToken $key @("foundry", "coolant", "magnetic_cargo", "conveyor")) {
        $bucket = "foundry"
    } elseif (Test-AnyToken $key @("furnace", "slag", "molten", "ash_", "refractory", "crucible", "heat_curtain")) {
        $bucket = "furnace"
    } elseif (Test-AnyToken $key @("reliquary", "relic", "ceramic", "ivory", "hymn", "saint", "censer", "guardian", "glyph")) {
        $bucket = "reliquary"
    }

    $role = "anchors"
    if (Test-AnyToken $key @("floor", "sigil", "offering", "trough", "hazard", "marker", "decal", "grate", "socket")) {
        $role = "floor_details"
    } elseif (Test-AnyToken $key @("console", "tower", "heat_exchanger", "shrine", "coffer", "column", "buttress", "pedestal", "pylon", "beacon", "crucible", "reactor", "large_cylinder", "pod")) {
        $role = "focals"
    } elseif (Test-AnyToken $key @("wall", "panel", "scrubber", "censer", "tablet", "monitor", "terminal", "display", "keypad", "light_bar", "vertical_panel", "pipe_bundle", "vent")) {
        $role = "wall_details"
    }

    return @{ Bucket = $bucket; Role = $role }
}

$packRoot = Join-RepoPath "assets\packs\pulse_environment\meshy"
$manifestRoot = Join-RepoPath "assets\packs\pulse_environment\manifests"
New-Item -ItemType Directory -Force -Path $packRoot | Out-Null
New-Item -ItemType Directory -Force -Path $manifestRoot | Out-Null

$quaterniusRoot = Join-RepoPath "assets\packs\pulse_environment\quaternius"
$quaterniusSources = @(
    @{
        Name = "Modular SciFi MegaKit[Pro]"
        Source = Join-RepoPath "assets\quaternius\Modular SciFi MegaKit[Pro]"
        Dest = Join-Path $quaterniusRoot "Modular SciFi MegaKit[Pro]"
    },
    @{
        Name = "Sci-Fi Essentials Kit[Pro]"
        Source = Join-RepoPath "assets\quaternius\Sci-Fi Essentials Kit[Pro]"
        Dest = Join-Path $quaterniusRoot "Sci-Fi Essentials Kit[Pro]"
    },
    @{
        Name = "Animated Mech Pack"
        Source = Join-RepoPath "assets\quaternius\Animated Mech Pack"
        Dest = Join-Path $quaterniusRoot "Animated Mech Pack"
    },
    @{
        Name = "enemies_relicpunk"
        Source = Join-RepoPath "assets\quaternius\enemies_relicpunk"
        Dest = Join-Path $quaterniusRoot "enemies_relicpunk"
    }
)

$rounds = @(
    @{
        Name = "round_1"
        InputRoot = Join-RepoPath "New Models\Asset kits\round_1_inputs"
        MeshRootCandidates = @(
            (Join-RepoPath "New Models\Asset kits\round_1_inputs\codex_hq_refs\meshy_generated_models"),
            (Join-RepoPath "New Models\Asset kits\round_1_inputs\meshy_generated_models")
        )
        MetadataCsv = Join-RepoPath "New Models\Asset kits\round_1_inputs\round1_image_regen_status.csv"
        ExtraManifestFiles = @(
            (Join-RepoPath "New Models\Asset kits\round_1_inputs\codex_hq_refs\meshy_generated_models\_meshy_generation_summary.csv")
        )
    },
    @{
        Name = "round_2"
        InputRoot = Join-RepoPath "New Models\Asset kits\round_2_inputs"
        MeshRootCandidates = @((Join-RepoPath "New Models\Asset kits\round_2_inputs\meshy_generated_models"))
        MetadataCsv = ""
        ExtraManifestFiles = @()
    },
    @{
        Name = "round_3"
        InputRoot = Join-RepoPath "New Models\Asset kits\round_3_inputs"
        MeshRootCandidates = @((Join-RepoPath "New Models\Asset kits\round_3_inputs\meshy_generated_models"))
        MetadataCsv = ""
        ExtraManifestFiles = @()
    }
)

$copied = 0
$seen = 0

foreach ($source in $quaterniusSources) {
    $result = Sync-DirectoryFiles $source.Source $source.Dest
    $seen += $result.Seen
    $copied += $result.Copied
    Write-Host "[assets] quaternius/$($source.Name): routed $($result.Seen) files"
}

foreach ($round in $rounds) {
    $metadata = Get-RoundMetadata $round.MetadataCsv
    $roundManifestDir = Join-Path $manifestRoot $round.Name
    New-Item -ItemType Directory -Force -Path $roundManifestDir | Out-Null

    if (Test-Path -LiteralPath $round.InputRoot) {
        Get-ChildItem -LiteralPath $round.InputRoot -File |
            Where-Object { $_.Extension -in @(".csv", ".json", ".jsonl", ".md") } |
            ForEach-Object {
                if (Copy-IfChanged $_ (Join-Path $roundManifestDir $_.Name)) { $script:copied++ }
            }
    }

    foreach ($extraManifest in @($round.ExtraManifestFiles)) {
        if ($extraManifest -and (Test-Path -LiteralPath $extraManifest)) {
            $extraManifestInfo = Get-Item -LiteralPath $extraManifest
            if (Copy-IfChanged $extraManifestInfo (Join-Path $roundManifestDir $extraManifestInfo.Name)) { $script:copied++ }
        }
    }

    $meshRoot = $null
    foreach ($candidate in @($round.MeshRootCandidates)) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            $meshRoot = $candidate
            break
        }
    }

    if (-not $meshRoot) {
        Write-Host "[assets] $($round.Name): no mesh root yet"
        continue
    }

    $models = Get-ChildItem -LiteralPath $meshRoot -Recurse -File -Filter "model_glb.glb"
    foreach ($model in $models) {
        $seen++
        $assetDirName = $model.Directory.Name
        $meta = Get-AssetMetadata $assetDirName $metadata
        $route = Get-AssetRoute $assetDirName $meta
        $destAssetDir = Join-Path $packRoot (Join-Path $route.Bucket (Join-Path $route.Role ("{0}_{1}" -f $round.Name, $assetDirName)))
        if (Copy-IfChanged $model (Join-Path $destAssetDir "model_glb.glb")) { $copied++ }

        Get-ChildItem -LiteralPath $model.Directory.FullName -File |
            Where-Object { $_.Name -like "texture_*.png" } |
            ForEach-Object {
                if (Copy-IfChanged $_ (Join-Path $destAssetDir $_.Name)) { $script:copied++ }
            }
    }

    Write-Host "[assets] $($round.Name): routed $($models.Count) generated GLB assets"
}

Write-Host "[assets] pulse_environment pack ready: $seen assets inspected, $copied files copied/updated"
