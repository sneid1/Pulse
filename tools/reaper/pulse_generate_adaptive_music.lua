-- PULSE adaptive music DAW source/render script.
-- Run with:
--   reaper.exe -newinst -nosplash -new tools/reaper/pulse_generate_adaptive_music.lua

local info = debug.getinfo(1, "S")
local script_path = info.source:gsub("^@", "")
local script_dir = script_path:match("^(.*)[/\\]") or "."
local repo_root = script_dir .. "/../.."
local audio_dir = repo_root .. "/assets/audio"
local source_dir = audio_dir .. "/reaper_source"
local project_path = source_dir .. "/PULSE_adaptive_music.rpp"
local log_path = source_dir .. "/PULSE_adaptive_music_render_log.txt"

local sr = 48000
local bpm = 140.0
local bars = 8
local beat = 60.0 / bpm
local six = beat * 0.25
local bar = beat * 4.0
local loop_len = bars * bar
local frames = math.floor(loop_len * sr)
local pi = math.pi

local stems = {
  { id = "bed",      track = "music_bed",      src = "source_bed.wav",      vol = 0.92, pan = 0.00, color = {  89, 142, 210 } },
  { id = "bass",     track = "music_bass",     src = "source_bass.wav",     vol = 0.88, pan = 0.00, color = {  64, 190, 145 } },
  { id = "drums",    track = "music_drums",    src = "source_drums.wav",    vol = 0.84, pan = 0.00, color = { 232, 160,  72 } },
  { id = "pressure", track = "music_pressure", src = "source_pressure.wav", vol = 0.82, pan = 0.00, color = { 196, 105, 220 } },
  { id = "boss",     track = "music_boss",     src = "source_boss.wav",     vol = 0.78, pan = 0.00, color = { 225,  80,  80 } },
}

local roots = { 55.00, 55.00, 43.65, 49.00, 55.00, 65.41, 49.00, 41.20 }
local chord_degrees = {
  { 0, 3, 7, 10 }, { 0, 3, 7, 12 }, { 0, 4, 7, 10 }, { 0, 4, 7, 10 },
  { 0, 3, 7, 10 }, { 0, 4, 7, 12 }, { 0, 4, 7, 10 }, { 0, 4, 7, 10 },
}
local REST = 99
local bass_notes = {
  { REST,0,0,3, REST,0,7,0, REST,0,3,0, REST,7,5,-2 },
  { REST,0,3,0, REST,7,0,10, REST,0,3,7, REST,12,10,7 },
  { REST,0,0,4, REST,0,7,0, REST,0,4,7, REST,10,7,5 },
  { REST,0,2,4, REST,7,4,2, REST,0,7,10, REST,12,10,7 },
  { REST,0,0,3, REST,7,0,3, REST,10,7,3, REST,12,10,7 },
  { REST,0,4,7, REST,12,7,4, REST,0,4,7, REST,12,10,7 },
  { REST,0,2,4, REST,7,4,2, REST,0,7,10, REST,14,12,10 },
  { REST,0,4,7, REST,10,7,4, REST,0,4,7, 10,7,4,-1 },
}
local lead_notes = {
  { REST,0,REST,3, 7,REST,10,7, REST,12,10,7, REST,3,5,REST },
  { REST,0,REST,3, 7,REST,10,12, REST,15,12,10, 7,REST,5,3 },
  { REST,7,REST,5, 4,REST,7,12, REST,10,7,5, 4,REST,5,7 },
  { REST,0,REST,2, 7,REST,10,12, 14,REST,12,10, 7,5,3,2 },
  { REST,12,REST,10, 7,REST,3,5, REST,7,10,12, REST,15,12,10 },
  { REST,7,REST,4, 0,REST,4,7, REST,12,11,7, 4,REST,7,12 },
  { REST,14,REST,12, 10,REST,7,4, REST,7,10,12, 14,REST,12,10 },
  { REST,12,10,7, 4,REST,0,4, 7,10,12,10, 7,4,3,-1 },
}
local boss_notes = {
  { 0,REST,6,REST, 7,REST,3,REST, 0,REST,-1,REST, 3,REST,6,7 },
  { 0,REST,6,REST, 10,REST,7,REST, 3,REST,0,REST, 7,REST,10,12 },
  { 0,REST,5,REST, 7,REST,4,REST, 0,REST,-2,REST, 4,REST,7,10 },
  { 0,REST,6,REST, 7,REST,10,REST, 12,REST,10,7, 6,3,1,0 },
  { 12,REST,10,REST, 7,REST,6,REST, 3,REST,0,REST, 6,REST,7,10 },
  { 0,REST,4,REST, 7,REST,11,REST, 12,REST,11,REST, 7,REST,4,0 },
  { 0,REST,6,REST, 10,REST,12,REST, 14,REST,12,10, 7,6,3,1 },
  { 0,4,7,10, 12,10,7,4, 0,4,7,10, 12,10,7,-1 },
}

local function clamp(x, lo, hi)
  if x < lo then return lo end
  if x > hi then return hi end
  return x
end

local function softclip(x, drive)
  local y = clamp(x * drive, -12.0, 12.0)
  local e = math.exp(2.0 * y)
  return (e - 1.0) / (e + 1.0)
end

local function env(t, speed)
  if t < 0.0 then return 0.0 end
  return math.exp(-t * speed)
end

local function sine(t, hz)
  return math.sin(2.0 * pi * hz * t)
end

local function saw(t, hz)
  local p = (t * hz) % 1.0
  return p * 2.0 - 1.0
end

local function pulse(t, hz, width)
  local p = (t * hz) % 1.0
  if p < width then return 1.0 end
  return -1.0
end

local function semitone(st)
  return 2.0 ^ (st / 12.0)
end

local function fast_noise(n)
  local x = (n * 1103515245 + 12345) % 2147483648
  return (x / 1073741824.0) - 1.0
end

local function brighten_noise(sample, step, song_bar, salt)
  local a = fast_noise(sample * 19 + step * 113 + song_bar * 271 + salt)
  local b = fast_noise((sample - 1) * 19 + step * 113 + song_bar * 271 + salt)
  return a - 0.72 * b
end

local function add_pan(acc, value, pan)
  local p = clamp(pan, -1.0, 1.0)
  local lg = math.sqrt(0.5 * (1.0 - p))
  local rg = math.sqrt(0.5 * (1.0 + p))
  acc.l = acc.l + value * lg
  acc.r = acc.r + value * rg
end

local function hit_at(phase, at, speed)
  if phase < at then return 0.0 end
  return math.exp(-(phase - at) * speed)
end

local function frame_context(i)
  local t = i / sr
  local phrase_time = t % loop_len
  local bar_phase = phrase_time % bar
  local beat_phase = phrase_time % beat
  local six_phase = phrase_time % six
  local step = math.floor(bar_phase / six) % 16
  local song_bar = math.floor(phrase_time / bar) % bars
  local motif_bar = song_bar % 8
  local section = math.floor(song_bar / 8)
  local root = roots[motif_bar + 1]
  local pump = 0.22 + 0.78 * (1.0 - env(beat_phase, 7.0))
  local bar_lift = 0.0
  if (motif_bar == 3 or motif_bar == 7) and bar_phase > bar * 0.5 then
    bar_lift = (bar_phase - bar * 0.5) / (bar * 0.5)
  end
  local phrase_in = clamp(phrase_time / beat, 0.0, 1.0)
  local phrase_out = clamp((loop_len - phrase_time) / (beat * 0.75), 0.0, 1.0)
  return {
    t = t,
    sample = i,
    phrase_time = phrase_time,
    bar_phase = bar_phase,
    beat_phase = beat_phase,
    six_phase = six_phase,
    step = step,
    song_bar = song_bar,
    motif_bar = motif_bar,
    section = section,
    root = root,
    pump = pump,
    bar_lift = bar_lift,
    loop_guard = math.min(phrase_in, phrase_out),
    chaos = 0.5 * (fast_noise(step * 101 + song_bar * 131 + 7) + 1.0),
  }
end

local function chord_hz(ctx, voice, octave)
  return ctx.root * octave * semitone(chord_degrees[ctx.motif_bar + 1][voice] or 0)
end

local function render_bed(ctx)
  local acc = { l = 0.0, r = 0.0 }
  local kenv = env(ctx.beat_phase, 11.5)
  local kpitch = 40.0 + 165.0 * env(ctx.beat_phase, 36.0)
  local kick = softclip(sine(ctx.beat_phase, kpitch) * 1.58, 2.8) * kenv
    + brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 3) * env(ctx.beat_phase, 300.0) * 0.045
  add_pan(acc, kick * 0.95, 0.0)

  local off_pulse = hit_at(ctx.beat_phase, beat * 0.50, 7.5)
  local section_lift = ctx.section == 1 and 1.10 or 0.94
  local pad_env = (0.40 + 0.34 * ctx.pump + 0.17 * off_pulse) * (0.96 - 0.08 * ctx.bar_lift) * ctx.loop_guard * section_lift
  local pad_l, pad_r = 0.0, 0.0
  for v = 1, 4 do
    local h = chord_hz(ctx, v, v == 1 and 0.50 or 1.00)
    local breath = 0.82 + 0.18 * sine(ctx.t, 0.071 + 0.013 * v)
    pad_l = pad_l + softclip(saw(ctx.t, h * (0.996 - 0.0015 * v)) * 0.46 + sine(ctx.t, h * 0.5) * 0.18, 1.45) * breath
    pad_r = pad_r + softclip(saw(ctx.t, h * (1.004 + 0.0012 * v)) * 0.44 + sine(ctx.t, h * 0.5) * 0.18, 1.45) * breath
  end
  add_pan(acc, softclip(pad_l * 0.070, 1.4) * pad_env, -0.34)
  add_pan(acc, softclip(pad_r * 0.070, 1.4) * pad_env, 0.34)
  if ctx.section == 1 and (ctx.step == 2 or ctx.step == 6 or ctx.step == 10 or ctx.step == 14) then
    local air = sine(ctx.t, chord_hz(ctx, 4, 8.0) * 1.003) * env(ctx.six_phase, 12.0) * ctx.loop_guard
    add_pan(acc, air * 0.024, (ctx.step == 6 or ctx.step == 14) and 0.42 or -0.42)
  end
  add_pan(acc, sine(ctx.t, 55.0 * 0.25) * (0.075 + 0.045 * ctx.pump) * ctx.loop_guard, 0.0)
  return acc.l, acc.r
end

local function render_bass(ctx)
  local acc = { l = 0.0, r = 0.0 }
  local note = bass_notes[ctx.motif_bar + 1][ctx.step + 1]
  if note ~= REST then
    if ctx.section == 1 and (ctx.step == 6 or ctx.step == 14) then
      note = note + 12
    end
    local benv = env(ctx.six_phase, 8.0) * (ctx.six_phase < six * 0.88 and 1.0 or 0.0)
    local accent = (ctx.step == 1 or ctx.step == 9) and 1.16 or ((ctx.step == 14 or ctx.step == 15) and 1.04 or 0.88)
    if ctx.section == 1 then accent = accent * 1.08 end
    local cut = 0.62 + 0.38 * env(ctx.six_phase, 24.0)
    local bf = ctx.root * 2.0 * semitone(note)
    local acid = softclip(
      saw(ctx.t, bf * 0.997) * 0.46
      + pulse(ctx.t, bf * 1.003, 0.36) * 0.28
      + sine(ctx.t, bf * 0.5) * 0.60,
      3.7 + 1.7 * cut)
    local sub = sine(ctx.t, ctx.root * 0.5) * 0.30
    local click = brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 31) * env(ctx.six_phase, 145.0) * 0.026
    add_pan(acc, (acid * 0.43 + sub + click) * benv * ctx.pump * accent, 0.0)
    add_pan(acc, acid * benv * ctx.pump * 0.050 * accent, (ctx.step % 2 == 1) and 0.10 or -0.08)
  end
  return acc.l, acc.r
end

local function render_drums(ctx)
  local acc = { l = 0.0, r = 0.0 }
  local open_hat = ctx.step == 2 or ctx.step == 6 or ctx.step == 10 or ctx.step == 14
  local henv = env(ctx.six_phase, open_hat and 18.0 or ((ctx.step % 2 == 1) and 52.0 or 105.0))
  local hacc = open_hat and 1.05 or ((ctx.step % 2 == 1) and 0.62 or 0.42)
  local hat = (brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 17) * 0.30
    + sine(ctx.t, open_hat and 8200.0 or 10400.0) * 0.050) * henv * hacc
  add_pan(acc, hat * 0.088, (ctx.step % 2 == 1) and -0.52 or 0.50)

  if ctx.step == 4 or ctx.step == 12 then
    local clap = brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 51) * hit_at(ctx.six_phase, 0.000, 21.0) * 0.13
      + brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 57) * hit_at(ctx.six_phase, 0.012, 29.0) * 0.10
      + sine(ctx.t, 185.0) * hit_at(ctx.six_phase, 0.000, 24.0) * 0.032
    add_pan(acc, clap, -0.18)
    add_pan(acc, clap * 0.78, 0.22)
  end

  local ghost = ctx.step == 7 or ctx.step == 11 or ctx.step == 14 or (ctx.motif_bar == 7 and ctx.step >= 12)
  if ghost then
    local gk = env(ctx.six_phase, 16.0)
    local pitch = 58.0 + 118.0 * env(ctx.six_phase, 35.0)
    add_pan(acc, softclip(sine(ctx.six_phase, pitch) * 1.15, 2.0) * gk * ((ctx.motif_bar == 7 and ctx.step >= 12) and 0.16 or 0.22), 0.0)
  end

  if ctx.step == 2 or ctx.step == 6 or ctx.step == 10 or ctx.step == 14 then
    local senv = env(ctx.six_phase, 13.0)
    local stab = 0.0
    for v = 1, 3 do
      stab = stab + saw(ctx.t, chord_hz(ctx, v, 4.0) * (0.996 + 0.003 * v)) * (v == 1 and 0.34 or 0.22)
    end
    add_pan(acc, softclip(stab, 2.3) * senv * ctx.pump * 0.064, (ctx.step == 6 or ctx.step == 14) and 0.34 or -0.34)
  end

  if ctx.section == 1 and (ctx.step == 3 or ctx.step == 5 or ctx.step == 13 or ctx.step == 15) then
    local rim = sine(ctx.t, 760.0 + 80.0 * ctx.chaos) * env(ctx.six_phase, 72.0)
      + brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 84) * env(ctx.six_phase, 95.0) * 0.16
    add_pan(acc, rim * 0.040, (ctx.step % 2 == 1) and -0.28 or 0.28)
  end

  if ctx.motif_bar == 7 and ctx.step >= 12 then
    local fill_pitch = 230.0 - 24.0 * (ctx.step - 12)
    local fill = brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 63) * env(ctx.six_phase, 42.0) * (ctx.section == 1 and 0.073 or 0.055)
      + sine(ctx.t, fill_pitch) * env(ctx.six_phase, 26.0) * 0.050
    add_pan(acc, fill, (ctx.step % 2 == 1) and -0.42 or 0.42)
  end
  return acc.l, acc.r
end

local function render_pressure(ctx)
  local acc = { l = 0.0, r = 0.0 }
  local note = lead_notes[ctx.motif_bar + 1][ctx.step + 1]
  if note ~= REST then
    if ctx.section == 1 and (ctx.step == 10 or ctx.step == 14) then
      note = note + 12
    end
    local phrase_accent = (ctx.motif_bar >= 4 and 1.08 or 0.94) + (ctx.motif_bar == 7 and 0.12 or 0.0)
    if ctx.section == 1 then phrase_accent = phrase_accent * 1.14 end
    local note_len = ctx.motif_bar == 7 and 0.72 or 0.58
    local renv = env(ctx.six_phase, 6.7) * (ctx.six_phase < six * note_len and 1.0 or 0.0)
    local af = ctx.root * 4.0 * semitone(note)
    local vibrato = 1.0 + 0.012 * sine(ctx.t, 6.1 + ctx.chaos * 2.5) + 0.006 * sine(ctx.t, 10.7)
    local left = softclip(saw(ctx.t, af * vibrato * 0.996) * 0.48 + pulse(ctx.t, af * 1.002, 0.30) * 0.30, 4.1)
    local right = softclip(saw(ctx.t, af * vibrato * 1.007) * 0.45 + pulse(ctx.t, af * 0.991, 0.36) * 0.32, 4.1)
    add_pan(acc, left * renv * ctx.pump * 0.132 * phrase_accent, -0.58)
    add_pan(acc, right * renv * ctx.pump * 0.132 * phrase_accent, 0.58)
  end
  if ctx.bar_lift > 0.0 then
    local riser = (brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 73) * 0.26
      + sine(ctx.t, 320.0 + 4800.0 * ctx.bar_lift * ctx.bar_lift) * 0.25)
      * ctx.bar_lift * ctx.bar_lift * ctx.loop_guard
    add_pan(acc, riser * 0.100, 0.48)
    add_pan(acc, riser * 0.070, -0.36)
  end
  return acc.l, acc.r
end

local function render_boss(ctx)
  local acc = { l = 0.0, r = 0.0 }
  local drone = softclip(
    saw(ctx.t, ctx.root * 0.50 * (1.0 + 0.006 * sine(ctx.t, 0.19))) * 0.28
    + sine(ctx.t, ctx.root * 0.25) * 0.22
    + saw(ctx.t, ctx.root * 0.75) * 0.10,
    3.0) * (0.58 + 0.42 * ctx.pump) * ctx.loop_guard
  add_pan(acc, drone * 0.31, 0.0)

  local note = boss_notes[ctx.motif_bar + 1][ctx.step + 1]
  if note ~= REST then
    if ctx.section == 1 and (ctx.step == 8 or ctx.step == 12 or ctx.step == 15) then
      note = note + 12
    end
    local e = env(ctx.six_phase, ctx.motif_bar == 7 and 5.6 or 7.8) * (ctx.six_phase < six * 0.82 and 1.0 or 0.0)
    local hz = ctx.root * 2.0 * semitone(note)
    local tone = softclip(
      saw(ctx.t, hz * 0.996) * 0.38
      + pulse(ctx.t, hz * semitone(6.0), 0.42) * 0.20
      + sine(ctx.t, hz * 0.5) * 0.18,
      4.8)
    add_pan(acc, tone * e * ctx.pump * (ctx.section == 1 and 0.21 or 0.18), (ctx.step % 4 >= 2) and -0.48 or 0.48)
  end
  if (ctx.step == 3 or ctx.step == 7 or ctx.step == 12 or ctx.step == 15) and ctx.chaos > 0.28 then
    local alarm = brighten_noise(ctx.sample, ctx.step, ctx.song_bar, 89) * env(ctx.six_phase, 25.0) * 0.070
      + sine(ctx.t, ctx.root * 8.0 * semitone((ctx.step % 2 == 1) and 6 or 10)) * env(ctx.six_phase, 34.0) * 0.040
    add_pan(acc, alarm, (ctx.step % 2 == 1) and -0.65 or 0.65)
  end
  return acc.l, acc.r
end

local renderers = {
  bed = render_bed,
  bass = render_bass,
  drums = render_drums,
  pressure = render_pressure,
  boss = render_boss,
}

local function one_pole_lowpass(buf, cutoff)
  local rc = 1.0 / (2.0 * pi * cutoff)
  local dt = 1.0 / sr
  local a = dt / (rc + dt)
  local y = 0.0
  for i = 1, #buf do
    y = y + a * (buf[i] - y)
    buf[i] = y
  end
end

local function one_pole_highpass(buf, cutoff)
  local rc = 1.0 / (2.0 * pi * cutoff)
  local dt = 1.0 / sr
  local a = rc / (rc + dt)
  local y = 0.0
  local prev = 0.0
  for i = 1, #buf do
    local x = buf[i]
    y = a * (y + x - prev)
    buf[i] = y
    prev = x
  end
end

local function add_pingpong_delay(lbuf, rbuf, delay_seconds, mix)
  local d = math.max(1, math.floor(delay_seconds * sr))
  for i = d + 1, #lbuf do
    local wet_l = rbuf[i - d] * mix
    local wet_r = lbuf[i - d] * mix
    lbuf[i] = lbuf[i] + wet_l
    rbuf[i] = rbuf[i] + wet_r
  end
end

local function widen(lbuf, rbuf, amount)
  for i = 1, #lbuf do
    local mid = 0.5 * (lbuf[i] + rbuf[i])
    local side = 0.5 * (lbuf[i] - rbuf[i]) * amount
    lbuf[i] = mid + side
    rbuf[i] = mid - side
  end
end

local function normalize_peak(lbuf, rbuf, target)
  local peak = 0.000001
  for i = 1, #lbuf do
    local al = math.abs(lbuf[i])
    local ar = math.abs(rbuf[i])
    if al > peak then peak = al end
    if ar > peak then peak = ar end
  end
  local gain = target / peak
  if gain > 1.65 then gain = 1.65 end
  for i = 1, #lbuf do
    lbuf[i] = softclip(lbuf[i] * gain, 1.02)
    rbuf[i] = softclip(rbuf[i] * gain, 1.02)
  end
end

local function producer_process(stem_id, lbuf, rbuf)
  if stem_id == "bed" then
    one_pole_lowpass(lbuf, 9500.0)
    one_pole_lowpass(rbuf, 9500.0)
    widen(lbuf, rbuf, 1.08)
    normalize_peak(lbuf, rbuf, 0.82)
  elseif stem_id == "bass" then
    one_pole_lowpass(lbuf, 4200.0)
    one_pole_lowpass(rbuf, 4200.0)
    widen(lbuf, rbuf, 0.58)
    normalize_peak(lbuf, rbuf, 0.74)
  elseif stem_id == "drums" then
    one_pole_highpass(lbuf, 28.0)
    one_pole_highpass(rbuf, 28.0)
    add_pingpong_delay(lbuf, rbuf, six * 0.75, 0.045)
    widen(lbuf, rbuf, 1.22)
    normalize_peak(lbuf, rbuf, 0.76)
  elseif stem_id == "pressure" then
    one_pole_highpass(lbuf, 150.0)
    one_pole_highpass(rbuf, 150.0)
    add_pingpong_delay(lbuf, rbuf, six * 3.0, 0.18)
    add_pingpong_delay(lbuf, rbuf, six * 5.0, 0.08)
    widen(lbuf, rbuf, 1.34)
    normalize_peak(lbuf, rbuf, 0.72)
  elseif stem_id == "boss" then
    one_pole_highpass(lbuf, 42.0)
    one_pole_highpass(rbuf, 42.0)
    add_pingpong_delay(lbuf, rbuf, six * 2.0, 0.11)
    widen(lbuf, rbuf, 1.18)
    normalize_peak(lbuf, rbuf, 0.78)
  end
end

local function write_u16(f, n)
  f:write(string.char(n % 256, math.floor(n / 256) % 256))
end

local function write_u32(f, n)
  f:write(string.char(n % 256, math.floor(n / 256) % 256, math.floor(n / 65536) % 256, math.floor(n / 16777216) % 256))
end

local function write_i16(f, n)
  if n < 0 then n = n + 65536 end
  write_u16(f, n)
end

local function write_wav(path, renderer, stem_id)
  local f = assert(io.open(path, "wb"))
  local data_bytes = frames * 4
  f:write("RIFF")
  write_u32(f, 36 + data_bytes)
  f:write("WAVEfmt ")
  write_u32(f, 16)
  write_u16(f, 1)
  write_u16(f, 2)
  write_u32(f, sr)
  write_u32(f, sr * 4)
  write_u16(f, 4)
  write_u16(f, 16)
  f:write("data")
  write_u32(f, data_bytes)
  local lbuf, rbuf = {}, {}
  for i = 0, frames - 1 do
    local ctx = frame_context(i)
    local l, r = renderer(ctx)
    lbuf[i + 1] = l * 0.92
    rbuf[i + 1] = r * 0.92
  end
  producer_process(stem_id, lbuf, rbuf)
  for i = 1, frames do
    local l = softclip(lbuf[i], 1.05)
    local r = softclip(rbuf[i], 1.05)
    write_i16(f, math.floor(clamp(l, -1.0, 1.0) * 32767.0 + (l >= 0 and 0.5 or -0.5)))
    write_i16(f, math.floor(clamp(r, -1.0, 1.0) * 32767.0 + (r >= 0 and 0.5 or -0.5)))
  end
  f:close()
end

local function copy_file(src, dst)
  local input = assert(io.open(src, "rb"))
  local output = assert(io.open(dst, "wb"))
  while true do
    local chunk = input:read(1024 * 1024)
    if not chunk then break end
    output:write(chunk)
  end
  input:close()
  output:close()
end

local function log_line(f, text)
  f:write(text .. "\n")
end

reaper.RecursiveCreateDirectory(source_dir, 0)

local log = assert(io.open(log_path, "w"))
log_line(log, "PULSE adaptive music REAPER render")
log_line(log, "bpm=" .. tostring(bpm) .. " bars=" .. tostring(bars) .. " sr=" .. tostring(sr) .. " frames=" .. tostring(frames))
log_line(log, "project=" .. project_path)

for _, stem in ipairs(stems) do
  local src_path = source_dir .. "/" .. stem.src
  write_wav(src_path, renderers[stem.id], stem.id)
  stem.src_path = src_path
  os.remove(audio_dir .. "/" .. stem.track .. ".wav")
  log_line(log, "source=" .. src_path)
end

reaper.PreventUIRefresh(1)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE", sr, true)
reaper.GetSetProjectInfo(0, "PROJECT_SRATE_USE", 1, true)
reaper.GetSetProjectInfo(0, "PROJECT_TIMEBASE", 1, true)
reaper.SetTempoTimeSigMarker(0, -1, 0.0, -1, -1, bpm, 4, 4, false)
reaper.GetSetProjectNotes(0, true, "PULSE adaptive music DAW source. Five synced 8-bar stems rendered from REAPER for runtime vertical layering.")

for i, stem in ipairs(stems) do
  reaper.InsertTrackAtIndex(i - 1, false)
  local tr = reaper.GetTrack(0, i - 1)
  reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", stem.track, true)
  reaper.SetMediaTrackInfo_Value(tr, "D_VOL", stem.vol)
  reaper.SetMediaTrackInfo_Value(tr, "D_PAN", stem.pan)
  reaper.SetTrackColor(tr, reaper.ColorToNative(stem.color[1], stem.color[2], stem.color[3]) | 0x1000000)
  reaper.SetOnlyTrackSelected(tr)
  reaper.SetEditCurPos(0.0, false, false)
  reaper.InsertMedia(stem.src_path, 0)
  local item = reaper.GetTrackMediaItem(tr, 0)
  if item then
    reaper.SetMediaItemInfo_Value(item, "D_POSITION", 0.0)
    reaper.SetMediaItemInfo_Value(item, "D_LENGTH", frames / sr)
    reaper.SetMediaItemInfo_Value(item, "B_LOOPSRC", 0)
  end
end

reaper.AddProjectMarker2(0, true, 0.0, frames / sr, "PULSE_8_BAR_LOOP", -1, 0)
for b = 0, bars - 1 do
  reaper.AddProjectMarker2(0, false, b * bar, 0.0, "bar_" .. tostring(b + 1), -1, 0)
end

for i = 0, reaper.CountTracks(0) - 1 do
  local tr = reaper.GetTrack(0, i)
  reaper.SetTrackSelected(tr, true)
end

reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", frames / sr, true)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 128, true) -- selected tracks via master
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", sr, true)
reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_TAILMS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_ADDTOPROJ", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_DITHER", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_NORMALIZE", 0, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", audio_dir, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "$track", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "wave", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT2", "", true)

reaper.Main_SaveProjectEx(0, project_path, 8)
reaper.PreventUIRefresh(-1)

local _, targets = reaper.GetSetProjectInfo_String(0, "RENDER_TARGETS", "", false)
log_line(log, "targets=" .. targets)

for _, stem in ipairs(stems) do
  local target = audio_dir .. "/" .. stem.track .. ".wav"
  copy_file(stem.src_path, target)
  local f = io.open(target, "rb")
  local size = f and f:seek("end") or 0
  if f then f:close() end
  log_line(log, "exported=" .. target .. " bytes=" .. tostring(size))
end

reaper.Main_SaveProjectEx(0, project_path, 8)
log:close()
reaper.Main_OnCommand(40004, 0)
