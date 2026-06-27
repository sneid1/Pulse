-- Build the editable REAPER source session from pre-rendered source_*.wav files.

local info = debug.getinfo(1, "S")
local script_path = info.source:gsub("^@", "")
local script_dir = script_path:match("^(.*)[/\\]") or "."
local repo_root = script_dir .. "/../.."
local audio_dir = repo_root .. "/assets/audio"
local music_dir = audio_dir .. "/music"
local source_dir = audio_dir .. "/reaper_source"
local project_path = source_dir .. "/PULSE_adaptive_music.rpp"

local sr = 48000
local bpm = 140.0
local bars = 16
local beat = 60.0 / bpm
local bar = beat * 4.0
local loop_len = bars * bar

local stems = {}
local biomes = { "foundry", "furnace", "reliquary" }
local layers = { "bed", "bass", "drums", "pressure", "boss", "overpulse" }
local colors = {
  foundry = {  89, 142, 210 },
  furnace = { 232, 120,  72 },
  reliquary = { 150, 125, 220 },
  hub = { 196, 105, 220 },
  stinger = { 225,  80,  80 },
}
local layer_vol = { bed = 0.92, bass = 0.88, drums = 0.84, pressure = 0.82, boss = 0.78, overpulse = 0.80 }
for _, biome in ipairs(biomes) do
  for _, layer in ipairs(layers) do
    local short = biome .. "_" .. layer
    stems[#stems + 1] = { track = short, src = "source_" .. short .. ".wav", len = loop_len, vol = layer_vol[layer], pan = 0.00, color = colors[biome] }
  end
end
stems[#stems + 1] = { track = "hub_bed", src = "source_hub_bed.wav", len = loop_len, vol = 0.74, pan = 0.00, color = colors.hub }
local stingers = {
  { "room_clear", 1.05 },
  { "reward", 1.18 },
  { "boss_intro", 2.25 },
  { "overpulse", 1.35 },
  { "run_win", 2.45 },
  { "run_lose", 1.85 },
  { "sector_foundry", 1.18 },
  { "sector_furnace", 1.28 },
  { "sector_reliquary", 1.34 },
}
for _, stinger in ipairs(stingers) do
  local name = stinger[1]
  local seconds = stinger[2]
  local short = "stinger_" .. name
  stems[#stems + 1] = { track = short, src = "source_" .. short .. ".wav", len = seconds, vol = 0.86, pan = 0.00, color = colors.stinger }
end

reaper.RecursiveCreateDirectory(source_dir, 0)
reaper.RecursiveCreateDirectory(music_dir, 0)
reaper.PreventUIRefresh(1)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE", sr, true)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
reaper.GetSetProjectInfo(0, "PROJECT_TIMEBASE", 1, true)
reaper.SetTempoTimeSigMarker(0, -1, 0.0, -1, -1, bpm, 4, 4, false)
reaper.GetSetProjectNotes(0, true, "PULSE adaptive music v3 source. Source clips are produced by tools/audio/pulse_music_producer.py, then arranged here as three biome stem banks, overpulse layers, hub bed, and music-bus stingers.")

for i, stem in ipairs(stems) do
  reaper.InsertTrackAtIndex(i - 1, false)
  local tr = reaper.GetTrack(0, i - 1)
  reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", stem.track, true)
  reaper.SetMediaTrackInfo_Value(tr, "D_VOL", stem.vol)
  reaper.SetMediaTrackInfo_Value(tr, "D_PAN", stem.pan)
  reaper.SetTrackColor(tr, reaper.ColorToNative(stem.color[1], stem.color[2], stem.color[3]) | 0x1000000)
  reaper.SetOnlyTrackSelected(tr)
  reaper.SetEditCurPos(0.0, false, false)
  reaper.InsertMedia(source_dir .. "/" .. stem.src, 0)
  local item = reaper.GetTrackMediaItem(tr, 0)
  if item then
    reaper.SetMediaItemInfo_Value(item, "D_POSITION", 0.0)
    reaper.SetMediaItemInfo_Value(item, "D_LENGTH", stem.len)
    reaper.SetMediaItemInfo_Value(item, "B_LOOPSRC", 0)
  end
end

reaper.AddProjectMarker2(0, true, 0.0, loop_len, "PULSE_16_BAR_LOOP", -1, 0)
for b = 0, bars - 1 do
  reaper.AddProjectMarker2(0, false, b * bar, 0.0, "bar_" .. tostring(b + 1), -1, 0)
end

reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", loop_len, true)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 128, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", sr, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", music_dir, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "$track", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "wave", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT2", "", true)

reaper.Main_SaveProjectEx(0, project_path, 8)
reaper.PreventUIRefresh(-1)
reaper.Main_OnCommand(40004, 0)
