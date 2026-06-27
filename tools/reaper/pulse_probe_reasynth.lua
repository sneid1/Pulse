local log = io.open(reaper.GetResourcePath() .. "/pulse_reasynth_probe.txt", "w")
local function line(s)
  if log then log:write(s .. "\n") end
end

reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
local fx = reaper.TrackFX_AddByName(track, "ReaSynth", false, -1)
line("fx=" .. tostring(fx))
if fx >= 0 then
  local n = reaper.TrackFX_GetNumParams(track, fx)
  line("params=" .. tostring(n))
  for i = 0, n - 1 do
    local _, name = reaper.TrackFX_GetParamName(track, fx, i, "")
    local value = reaper.TrackFX_GetParam(track, fx, i)
    line(tostring(i) .. "\t" .. name .. "\t" .. tostring(value))
  end
end
if log then log:close() end
reaper.Main_OnCommand(40004, 0)
