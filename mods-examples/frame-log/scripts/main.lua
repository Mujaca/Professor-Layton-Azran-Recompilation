-- Beispiel: nur lesen und loggen (docs/MODDING.md, Stufe B).
-- Veraendert weder Gastspeicher noch Eingabe; mit --deterministic muss die
-- Bildfolge bytegleich zu einem Lauf ohne Mods sein.

local EVERY = 300           -- alle N VBlanks eine Zeile ins Log

local fmt = require("fmt")  -- scripts/fmt.lua desselben Mods

on_load(function()
  -- 0x00100000 = Beginn des Programmcodes (3DS-Speicherlayout, kein Spielwissen)
  log("geladen, erstes Code-Wort " .. fmt.hex32(mem.read_u32(0x00100000)))
end)

on_frame(function(frame)
  if guest.vblank() % EVERY == 0 then
    local i = input.get()
    log(string.format("vblank=%d frame=%d t=%.3fs touch=%s a=%s",
        guest.vblank(), frame, guest.time_ns() / 1e9, tostring(i.touch), tostring(i.a)))
  end
end)
