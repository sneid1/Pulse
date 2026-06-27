# meshy_gen.ps1 - Pulse Meshy AI generation helper. ASCII only (see CLAUDE.md).
#
# Drives the Meshy text-to-3d OpenAPI: submit a preview (mesh-only) task, poll to
# completion, and download the GLB. Optionally chains a refine (texture) pass, which
# is only needed when a mesh must be rigged (rigging wants a textured humanoid).
#
# For Pulse the look comes from the engine (shading bands, ink outlines, the locked
# master-material library in config/pulse.style), NOT from Meshy textures. So most
# assets are PREVIEW-ONLY and their generated textures are discarded on import.
#
# Key resolution order: $env:MESHY_API_KEY, then tools/meshy/key.txt (gitignored).
#
# Examples:
#   .\meshy_gen.ps1 -Command balance
#   .\meshy_gen.ps1 -Command generate -Prompt "brutalist monolith" -Out ..\..\assets\meshy\raw\monolith.glb
#   .\meshy_gen.ps1 -Command status   -TaskId <id>
#   .\meshy_gen.ps1 -Command download -TaskId <id> -Out out.glb

[CmdletBinding()]
param(
  [ValidateSet('generate','refine','balance','status','download')]
  [string]$Command = 'generate',
  [string]$Prompt = '',
  [string]$Out = '',
  [string]$Model = 'meshy-5',
  [int]$Polycount = 8000,
  [ValidateSet('triangle','quad')]
  [string]$Topology = 'triangle',
  [switch]$NoRemesh,
  [switch]$Refine,
  [switch]$Rig,                 # characters: after preview+refine, auto-rig (humanoid) + download walk/run
  [double]$HeightMeters = 1.8,
  [string]$TaskId = '',
  [int]$TimeoutSec = 900
)

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$base = 'https://api.meshy.ai/openapi'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

function Get-Key {
  if ($env:MESHY_API_KEY) { return $env:MESHY_API_KEY.Trim() }
  $kf = Join-Path $scriptDir 'key.txt'
  if (Test-Path $kf) { return ((Get-Content $kf -Raw).Trim()) }
  throw 'No Meshy API key. Set MESHY_API_KEY or create tools/meshy/key.txt'
}

$headers = @{ Authorization = ("Bearer " + (Get-Key)) }

function Poll-Task([string]$id) {
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  $last = -1
  while ($true) {
    $t = Invoke-RestMethod -Method Get -Uri "$base/v2/text-to-3d/$id" -Headers $headers
    if ($t.progress -ne $last) {
      Write-Host ("  [{0,3}%] {1}" -f $t.progress, $t.status)
      $last = $t.progress
    }
    if ($t.status -eq 'SUCCEEDED') { return $t }
    if ($t.status -eq 'FAILED' -or $t.status -eq 'CANCELED') {
      $msg = ''
      if ($t.task_error) { $msg = $t.task_error.message }
      throw ("Task $id $($t.status): $msg")
    }
    if ((Get-Date) -gt $deadline) { throw "Task $id timed out after $TimeoutSec s" }
    Start-Sleep -Seconds 5
  }
}

function Save-Url([string]$url, [string]$out) {
  if (-not $url) { throw 'No URL to download' }
  $dir = Split-Path -Parent $out
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  # Pre-signed CDN link; do NOT send the API auth header to it.
  Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
  Write-Host ("Saved -> {0}" -f $out)
}

function Save-Glb($task, [string]$out) {
  if (-not $out) { throw 'Specify -Out <path.glb>' }
  $url = $task.model_urls.glb
  if (-not $url) { throw 'No GLB url in task result' }
  Save-Url $url $out
  Write-Host ("  (credits used this task: {0})" -f $task.consumed_credits)
}

# Poll a rigging task (different endpoint + result shape from text-to-3d).
function Poll-RigTask([string]$id) {
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  $last = -1
  while ($true) {
    $t = Invoke-RestMethod -Method Get -Uri "$base/v1/rigging/$id" -Headers $headers
    if ($t.progress -ne $last) { Write-Host ("  rig [{0,3}%] {1}" -f $t.progress, $t.status); $last = $t.progress }
    if ($t.status -eq 'SUCCEEDED') { return $t }
    if ($t.status -eq 'FAILED' -or $t.status -eq 'CANCELED') {
      $msg = ''; if ($t.task_error) { $msg = $t.task_error.message }
      throw ("Rig task $id $($t.status): $msg")
    }
    if ((Get-Date) -gt $deadline) { throw "Rig task $id timed out after $TimeoutSec s" }
    Start-Sleep -Seconds 6
  }
}

switch ($Command) {
  'balance' {
    $b = Invoke-RestMethod -Method Get -Uri "$base/v1/balance" -Headers $headers
    Write-Host ("Meshy balance: {0} credits" -f $b.balance)
  }
  'status' {
    if (-not $TaskId) { throw 'Specify -TaskId' }
    $t = Invoke-RestMethod -Method Get -Uri "$base/v2/text-to-3d/$TaskId" -Headers $headers
    Write-Host ("status={0} progress={1}% credits={2}" -f $t.status, $t.progress, $t.consumed_credits)
  }
  'download' {
    if (-not $TaskId) { throw 'Specify -TaskId' }
    $t = Invoke-RestMethod -Method Get -Uri "$base/v2/text-to-3d/$TaskId" -Headers $headers
    Save-Glb $t $Out
  }
  'refine' {
    # Texture an EXISTING preview task (-TaskId = preview id) without re-rolling the mesh.
    if (-not $TaskId) { throw 'Specify -TaskId <preview id>' }
    $rb = @{ mode = 'refine'; preview_task_id = $TaskId; enable_pbr = $true; remove_lighting = $true } | ConvertTo-Json
    Write-Host ("Submitting refine (PBR texture) for preview {0}..." -f $TaskId)
    $rr = Invoke-RestMethod -Method Post -Uri "$base/v2/text-to-3d" -Headers $headers -ContentType 'application/json' -Body $rb
    $rid = $rr.result
    Write-Host ("refine task: {0}" -f $rid)
    $t = Poll-Task $rid
    Save-Glb $t $Out
  }
  'generate' {
    if (-not $Prompt) { throw 'Specify -Prompt "..."' }
    $body = @{
      mode             = 'preview'
      prompt           = $Prompt
      ai_model         = $Model
      should_remesh    = (-not $NoRemesh)
      topology         = $Topology
      target_polycount = $Polycount
    } | ConvertTo-Json
    Write-Host "Submitting preview task ($Model, $Polycount tris, $Topology)..."
    $r = Invoke-RestMethod -Method Post -Uri "$base/v2/text-to-3d" -Headers $headers -ContentType 'application/json' -Body $body
    $id = $r.result
    Write-Host ("preview task: {0}" -f $id)
    $t = Poll-Task $id
    $texId = $id
    if ($Refine -or $Rig) {
      # Rigging needs a textured mesh, so -Rig implies a refine pass first.
      # PBR texture; remove_lighting (meshy-6 only) strips baked lighting from the albedo so the
      # metalness/roughness read cleanly. meshy-5 rejects remove_lighting, so only set it for 6.
      # NOTE: do not name this $refine - PowerShell is case-insensitive and it collides with the
      # -Refine switch parameter (assigning a hashtable to a SwitchParameter throws).
      $refineBody = @{ mode = 'refine'; preview_task_id = $id; enable_pbr = $true }
      if ($Model -eq 'meshy-6') { $refineBody['remove_lighting'] = $true }
      $rb = $refineBody | ConvertTo-Json
      Write-Host "Submitting refine task..."
      $rr = Invoke-RestMethod -Method Post -Uri "$base/v2/text-to-3d" -Headers $headers -ContentType 'application/json' -Body $rb
      $rid = $rr.result
      Write-Host ("refine task: {0}" -f $rid)
      $t = Poll-Task $rid
      $texId = $rid
    }
    if ($Rig) {
      try {
        $rgb = @{ input_task_id = $texId; height_meters = $HeightMeters } | ConvertTo-Json
        Write-Host ("Submitting rigging task (humanoid, height {0}m)..." -f $HeightMeters)
        $rg = Invoke-RestMethod -Method Post -Uri "$base/v1/rigging" -Headers $headers -ContentType 'application/json' -Body $rgb
        $rgId = $rg.result
        Write-Host ("rig task: {0}" -f $rgId)
        $rt = Poll-RigTask $rgId
        Save-Url $rt.result.rigged_character_glb_url $Out
        $walk = $rt.result.basic_animations.walking_glb_url
        if ($walk) { Save-Url $walk ([IO.Path]::ChangeExtension($Out, $null) + "_walk.glb") }
        $run = $rt.result.basic_animations.running_glb_url
        if ($run)  { Save-Url $run  ([IO.Path]::ChangeExtension($Out, $null) + "_run.glb") }
        Write-Host ("Rigged character done (rig credits: {0})" -f $rt.consumed_credits)
      } catch {
        # Pose-estimation can fail on non-standard humanoids (422). Keep the textured mesh
        # so the silhouette is still usable as a static prop / re-rig candidate.
        Write-Host ("RIGGING FAILED ({0}); saving the static textured mesh instead." -f $_.Exception.Message)
        Save-Glb $t $Out
      }
    } else {
      Save-Glb $t $Out
    }
  }
}
