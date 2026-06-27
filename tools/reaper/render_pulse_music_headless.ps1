param(
    [string]$PythonProducerPath = "$PSScriptRoot\..\audio\pulse_music_producer.py",
    [string]$ReaperPath = "",
    [string]$ScriptPath = "",
    [int]$TimeoutSeconds = 0
)

$ErrorActionPreference = 'Stop'
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$audioDir = Join-Path $repoRoot 'assets\audio'
$musicDir = Join-Path $audioDir 'music'
$sourceDir = Join-Path $audioDir 'reaper_source'
$projectPath = Join-Path $sourceDir 'PULSE_adaptive_music.rpp'
$biomes = @('foundry', 'furnace', 'reliquary')
$stems = @('bed', 'bass', 'drums', 'pressure', 'boss', 'overpulse')
$stingers = @('room_clear', 'reward', 'boss_intro', 'overpulse', 'run_win', 'run_lose', 'sector_foundry', 'sector_furnace', 'sector_reliquary')
$expectedNames = @()
foreach ($biome in $biomes) {
    foreach ($stem in $stems) {
        $expectedNames += "$biome`_$stem.wav"
    }
}
$expectedNames += 'hub_bed.wav'
foreach ($stinger in $stingers) {
    $expectedNames += "stinger_$stinger.wav"
}
$expected = $expectedNames | ForEach-Object { Join-Path $musicDir $_ }
$expectedSource = $expectedNames | ForEach-Object { Join-Path $sourceDir ("source_" + $_) }

if (-not (Test-Path -LiteralPath $PythonProducerPath)) {
    throw "Python producer not found: $PythonProducerPath"
}

$producer = (Resolve-Path -LiteralPath $PythonProducerPath).Path
python $producer
if ($LASTEXITCODE -ne 0) {
    throw "Python music producer failed with exit code $LASTEXITCODE"
}

foreach ($file in ($expected + $expectedSource)) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Missing exported music file: $file"
    }
    if ((Get-Item -LiteralPath $file).Length -le 44) {
        throw "Exported music file is empty: $file"
    }
}

if (-not (Test-Path -LiteralPath $projectPath)) {
    throw "Missing REAPER source project: $projectPath"
}
$projectText = Get-Content -Raw -LiteralPath $projectPath
if ($projectText -notmatch 'PULSE_16_BAR_LOOP') {
    throw "REAPER source project did not include the 16-bar loop marker: $projectPath"
}
if ($projectText -match 'PULSE_8_BAR_LOOP') {
    throw "REAPER source project still contains the old 8-bar marker: $projectPath"
}
if ($projectText -notmatch 'PULSE adaptive music v3 source') {
    throw "REAPER source project did not include the v3 source note: $projectPath"
}

Write-Host "Headless music export complete. REAPER was not launched."
Get-ChildItem -LiteralPath ($expected + $expectedSource + @($projectPath)) | Select-Object Name, Length, LastWriteTime
