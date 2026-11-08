-- Startup probe for the portable render REAPER: reports audio mode and whether Hypersonic 2 loads.
local dir = ({reaper.get_action_context()})[2]:match("^(.*)[\\/]")
local f = io.open(dir .. "\\probe.txt", "w")
local _, mode = reaper.GetAudioDeviceInfo("MODE", "")
local _, dev = reaper.GetAudioDeviceInfo("IDENT_OUT", "")
reaper.InsertTrackAtIndex(0, false)
local fx = reaper.TrackFX_AddByName(reaper.GetTrack(0, 0), "Hypersonic 2 (x86)", false, -1)
local name = ""
if fx >= 0 then _, name = reaper.TrackFX_GetFXName(reaper.GetTrack(0, 0), fx, "") end
f:write(string.format("mode=%s\ndevice=%s\nhs2_fx=%d %s\n", mode, dev, fx, name))
f:close()
