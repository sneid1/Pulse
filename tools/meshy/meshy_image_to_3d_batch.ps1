# meshy_image_to_3d_batch.ps1 - Batch Meshy image-to-3D helper. ASCII only.
#
# Converts local PNG/JPG concept images into Meshy GLB tasks using the
# /openapi/v1/image-to-3d endpoint. Each image gets its own output folder:
#   <out>/<image-stem>/model_glb.glb
#   <out>/<image-stem>/task_response.json
#   <out>/<image-stem>/preview*.png
#
# Key resolution order: $env:MESHY_API_KEY, then tools/meshy/key.txt.

[CmdletBinding()]
param(
  [string]$InputDir = "New Models/Asset kits/base_assets",
  [string]$OutDir = "New Models/Asset kits/base_assets/meshy_generated_models",
  [string]$Pattern = "*.png",
  [ValidateSet('meshy-5','meshy-6','latest')]
  [string]$Model = 'meshy-6',
  [ValidateSet('standard','lowpoly')]
  [string]$ModelType = 'standard',
  [int]$TargetPolycount = 30000,
  [ValidateSet('triangle','quad')]
  [string]$Topology = 'triangle',
  [switch]$NoTexture,
  [switch]$NoPbr,
  [switch]$NoRemesh,
  [switch]$NoAutoSize,
  [switch]$NoImageEnhancement,
  [switch]$Force,
  [int]$PollSeconds = 10,
  [int]$TimeoutSec = 3600
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$base = 'https://api.meshy.ai/openapi'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $scriptDir '..\..')

function Resolve-RepoPath([string]$path) {
  if ([IO.Path]::IsPathRooted($path)) { return (Resolve-Path -LiteralPath $path).Path }
  return (Resolve-Path -LiteralPath (Join-Path $repoRoot $path)).Path
}

function Get-Key {
  if ($env:MESHY_API_KEY) { return $env:MESHY_API_KEY.Trim() }
  $kf = Join-Path $scriptDir 'key.txt'
  if (Test-Path -LiteralPath $kf) { return ((Get-Content -LiteralPath $kf -Raw).Trim()) }
  throw 'No Meshy API key. Set MESHY_API_KEY or create tools/meshy/key.txt'
}

function Get-Mime([string]$path) {
  $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
  switch ($ext) {
    '.jpg'  { return 'image/jpeg' }
    '.jpeg' { return 'image/jpeg' }
    '.png'  { return 'image/png' }
    default { throw "Unsupported image extension: $ext" }
  }
}

function Save-Json($obj, [string]$path) {
  $dir = Split-Path -Parent $path
  if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
  }
  $obj | ConvertTo-Json -Depth 64 | Set-Content -LiteralPath $path -Encoding UTF8
}

function Save-Url([string]$url, [string]$out) {
  if (-not $url) { return }
  $dir = Split-Path -Parent $out
  if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
  }
  Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
}

function Download-TaskAssets($task, [string]$assetDir) {
  $glb = $task.model_urls.glb
  if (-not $glb) { throw "Task $($task.id) has no GLB URL" }
  Save-Url $glb (Join-Path $assetDir 'model_glb.glb')
  Save-Url $task.thumbnail_url (Join-Path $assetDir 'preview.png')
  Save-Url $task.alpha_thumbnail_url (Join-Path $assetDir 'preview_alpha.png')
  if ($task.thumbnail_urls) {
    Save-Url $task.thumbnail_urls.front (Join-Path $assetDir 'preview_front.png')
    Save-Url $task.thumbnail_urls.right (Join-Path $assetDir 'preview_right.png')
    Save-Url $task.thumbnail_urls.back  (Join-Path $assetDir 'preview_back.png')
    Save-Url $task.thumbnail_urls.left  (Join-Path $assetDir 'preview_left.png')
  }
}

$headers = @{ Authorization = ("Bearer " + (Get-Key)) }
$inputRoot = Resolve-RepoPath $InputDir
if ([IO.Path]::IsPathRooted($OutDir)) {
  $outputRoot = $OutDir
} else {
  $outputRoot = Join-Path $repoRoot $OutDir
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null

$images = Get-ChildItem -LiteralPath $inputRoot -File -Include $Pattern
if (-not $images) {
  $images = Get-ChildItem -LiteralPath $inputRoot -File | Where-Object {
    $_.Extension.ToLowerInvariant() -in @('.png','.jpg','.jpeg') -and $_.Name -like $Pattern
  }
}
$images = $images | Sort-Object Name
if (-not $images) { throw "No images matched '$Pattern' in $inputRoot" }

$pending = @()
$submitted = 0
$skipped = 0

foreach ($img in $images) {
  $stem = [IO.Path]::GetFileNameWithoutExtension($img.Name)
  $assetDir = Join-Path $outputRoot $stem
  $glbPath = Join-Path $assetDir 'model_glb.glb'
  $responsePath = Join-Path $assetDir 'task_response.json'
  $submissionPath = Join-Path $assetDir 'submission.json'

  if ((-not $Force) -and (Test-Path -LiteralPath $glbPath) -and (Test-Path -LiteralPath $responsePath)) {
    Write-Host ("SKIP complete: {0}" -f $stem)
    $skipped++
    continue
  }

  New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
  Copy-Item -LiteralPath $img.FullName -Destination (Join-Path $assetDir ('source' + $img.Extension.ToLowerInvariant())) -Force

  if ((-not $Force) -and (Test-Path -LiteralPath $submissionPath)) {
    $existing = Get-Content -LiteralPath $submissionPath -Raw | ConvertFrom-Json
    if ($existing.task_id) {
      Write-Host ("RESUME {0} task={1}" -f $stem, $existing.task_id)
      $pending += [pscustomobject]@{ Id = $existing.task_id; Stem = $stem; Dir = $assetDir; Response = $responsePath }
      continue
    }
  }

  $bytes = [IO.File]::ReadAllBytes($img.FullName)
  $dataUri = "data:$(Get-Mime $img.FullName);base64,$([Convert]::ToBase64String($bytes))"
  $body = @{
    image_url = $dataUri
    model_type = $ModelType
    ai_model = $Model
    should_texture = (-not $NoTexture)
    enable_pbr = ((-not $NoTexture) -and (-not $NoPbr))
    hd_texture = $false
    should_remesh = (-not $NoRemesh)
    topology = $Topology
    target_polycount = $TargetPolycount
    image_enhancement = (-not $NoImageEnhancement)
    remove_lighting = $true
    target_formats = @('glb')
    auto_size = (-not $NoAutoSize)
    origin_at = 'bottom'
    alpha_thumbnail = $true
    multi_view_thumbnails = $true
  }
  if ($ModelType -eq 'lowpoly') {
    $body.Remove('topology')
    $body.Remove('target_polycount')
    $body.Remove('should_remesh')
  }
  if ($Model -eq 'meshy-5') {
    $body.Remove('hd_texture')
    $body.Remove('image_enhancement')
    $body.Remove('remove_lighting')
  }

  Write-Host ("SUBMIT {0}" -f $stem)
  $result = Invoke-RestMethod -Method Post -Uri "$base/v1/image-to-3d" -Headers $headers -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 16)
  if (-not $result.result) { throw "No task id returned for $stem" }
  $submission = @{
    image = $img.Name
    stem = $stem
    task_id = $result.result
    submitted_at = (Get-Date).ToString('o')
    request = @{
      model_type = $body.model_type
      ai_model = $body.ai_model
      should_texture = $body.should_texture
      enable_pbr = $body.enable_pbr
      should_remesh = $body.should_remesh
      topology = $body.topology
      target_polycount = $body.target_polycount
      target_formats = $body.target_formats
      auto_size = $body.auto_size
      origin_at = $body.origin_at
    }
  }
  Save-Json $submission (Join-Path $assetDir 'submission.json')
  $pending += [pscustomobject]@{ Id = $result.result; Stem = $stem; Dir = $assetDir; Response = $responsePath }
  $submitted++
}

if (-not $pending) {
  Write-Host ("Done. submitted=0 skipped={0}" -f $skipped)
  exit 0
}

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$completed = 0
$failed = 0
$lastProgress = @{}

while ($pending.Count -gt 0) {
  $remaining = @()
  foreach ($job in $pending) {
    $task = Invoke-RestMethod -Method Get -Uri "$base/v1/image-to-3d/$($job.Id)" -Headers $headers
    $key = $job.Id
    $progress = [int]($task.progress)
    $status = [string]($task.status)
    if ((-not $lastProgress.ContainsKey($key)) -or $lastProgress[$key] -ne "$status/$progress") {
      Write-Host ("  {0}: {1}% {2}" -f $job.Stem, $progress, $status)
      $lastProgress[$key] = "$status/$progress"
    }

    if ($status -eq 'SUCCEEDED') {
      Save-Json $task $job.Response
      Download-TaskAssets $task $job.Dir
      $completed++
      Write-Host ("DONE {0} credits={1}" -f $job.Stem, $task.consumed_credits)
      continue
    }

    if ($status -eq 'FAILED' -or $status -eq 'CANCELED') {
      Save-Json $task $job.Response
      $failed++
      $msg = ''
      if ($task.task_error) { $msg = $task.task_error.message }
      Write-Warning ("FAILED {0}: {1} {2}" -f $job.Stem, $status, $msg)
      continue
    }

    $remaining += $job
  }

  $pending = $remaining
  if ($pending.Count -eq 0) { break }
  if ((Get-Date) -gt $deadline) {
    throw "Timed out after $TimeoutSec seconds with $($pending.Count) task(s) still pending"
  }
  Start-Sleep -Seconds $PollSeconds
}

Write-Host ("Done. submitted={0} skipped={1} completed={2} failed={3}" -f $submitted, $skipped, $completed, $failed)
if ($failed -gt 0) { exit 2 }
