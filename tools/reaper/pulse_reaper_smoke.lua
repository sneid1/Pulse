local out = reaper.GetResourcePath() .. "/pulse_reaper_smoke.txt"
local f = io.open(out, "w")
if f then
  f:write("REAPER Lua smoke ok\n")
  f:write("resource=" .. reaper.GetResourcePath() .. "\n")
  f:close()
end
reaper.Main_OnCommand(40004, 0)
