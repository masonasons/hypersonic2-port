-- Render Hypersonic 2 reference audio: one region (and one WAV) per patch listed in job.tsv.
-- Run:  reaper.exe -newinst -nosplash -new "<repo>\tools\reaper\hs2_render.lua"
-- Each region: bank select (CC0=0, CC32=bank) + program change at region start, a LOAD_TIME gap so
-- Hypersonic can stream the samples, then the fixed test sequence below. Result: <out>\B###_P###.wav
-- plus render.rpp (for inspection) and done.txt (written last, so the caller knows it finished).

local SCRIPT_DIR = ({reaper.get_action_context()})[2]:match("^(.*)[\\/]")
local FX_NAME = "Hypersonic 2 (x86)"
local TEMPO = 120
local LOAD_TIME = 4.0      -- seconds between program change and the first note
local CH = 0               -- MIDI channel 1

-- test sequence: {start (s, after LOAD_TIME), length (s), pitch, velocity}
local SEQ = {
  {0.0, 2.0, 60, 100},     -- C4 (middle C) long, then release
  {3.5, 1.0, 36, 100},     -- C2
  {5.5, 1.0, 84, 100},     -- C6
  {7.5, 1.0, 60, 30},      -- C4 soft
  {9.5, 1.0, 60, 127},     -- C4 hard
  {11.5, 1.5, 60, 100},    -- C major chord
  {11.5, 1.5, 64, 100},
  {11.5, 1.5, 67, 100},
}
local TAIL = 3.0
local SEQ_END = 13.0
local SLOT = LOAD_TIME + SEQ_END + TAIL

local function log(s) reaper.ShowConsoleMsg(tostring(s) .. "\n") end

local function read_job(path)
  local f = assert(io.open(path, "r"), "cannot open " .. path)
  local out, jobs = nil, {}
  for line in f:lines() do
    local a, b, c = line:match("^([^\t]*)\t([^\t]*)\t?(.*)$")
    if a == "#out" then out = b
    elseif a and tonumber(a) then jobs[#jobs + 1] = {bank = tonumber(a), prog = tonumber(b), name = c} end
  end
  f:close()
  return out, jobs
end

local function main()
  local out, jobs = read_job(SCRIPT_DIR .. "\\job.tsv")
  assert(out and #jobs > 0, "empty job")
  reaper.SetCurrentBPM(0, TEMPO, false)

  reaper.InsertTrackAtIndex(0, false)
  local tr = reaper.GetTrack(0, 0)
  local fx = reaper.TrackFX_AddByName(tr, FX_NAME, false, -1)
  assert(fx >= 0, "could not insert " .. FX_NAME)

  local seen = {}
  for i, j in ipairs(jobs) do
    local t0 = (i - 1) * SLOT
    local item = reaper.CreateNewMIDIItemInProj(tr, t0, t0 + SLOT, false)
    local take = reaper.GetActiveTake(item)
    local ppq = function(t) return reaper.MIDI_GetPPQPosFromProjTime(take, t) end
    reaper.MIDI_InsertCC(take, false, false, ppq(t0), 0xB0, CH, 0, 0)
    reaper.MIDI_InsertCC(take, false, false, ppq(t0) + 1, 0xB0, CH, 32, j.bank)
    reaper.MIDI_InsertCC(take, false, false, ppq(t0) + 2, 0xC0, CH, j.prog, 0)
    for _, n in ipairs(SEQ) do
      local s = t0 + LOAD_TIME + n[1]
      reaper.MIDI_InsertNote(take, false, false, ppq(s), ppq(s + n[2]), CH, n[3], n[4], true)
    end
    reaper.MIDI_Sort(take)
    local label = string.format("B%03d_P%03d", j.bank, j.prog)
    if seen[label] then label = label .. "_" .. i end
    seen[label] = true
    reaper.AddProjectMarker2(0, true, t0, t0 + SLOT, label, -1, 0)
  end

  reaper.GetSetProjectInfo_String(0, "RENDER_FILE", out, true)
  reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "$region", true)
  reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdyAAAA==", true) -- WAV, 32-bit float
  reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 3, true)    -- all project regions
  reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)      -- master mix
  reaper.GetSetProjectInfo(0, "RENDER_SRATE", 44100, true)
  reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 2, true)
  reaper.GetSetProjectInfo(0, "RENDER_TAILFLAG", 0, true)

  -- Render speed has no API: save, patch "RENDER_1X 0 0" -> "RENDER_1X 1 0" (1x offline), reopen.
  local rpp = out .. "\\render.rpp"
  reaper.Main_SaveProjectEx(0, rpp, 0)
  local fh = assert(io.open(rpp, "r"))
  local text = fh:read("a")
  fh:close()
  local patched, n = text:gsub("RENDER_1X %d+ %d+", "RENDER_1X 1 0")
  assert(n == 1, "RENDER_1X line not found in saved project")
  fh = assert(io.open(rpp, "w"))
  fh:write(patched)
  fh:close()
  reaper.Main_openProject("noprompt:" .. rpp)
  local t = reaper.time_precise()
  reaper.Main_OnCommand(42230, 0) -- render using most recent settings, auto-close dialog
  local f = io.open(out .. "\\done.txt", "w")
  f:write(string.format("%d patches rendered in %.1f s\n", #jobs, reaper.time_precise() - t))
  f:close()
  reaper.Main_SaveProjectEx(0, rpp, 0)
  reaper.Main_OnCommand(40004, 0) -- quit
end

local ok, err = pcall(main)
if not ok then
  log("hs2_render failed: " .. tostring(err))
end
