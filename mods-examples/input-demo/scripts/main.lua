-- Beispiel: Eingaben aus Lua (docs/MODDING.md, Stufe B).
--
-- 1. Fester Ablauf in Gast-Frames: tippt im Titelmenue auf "New Game"
--    (Touchscreen 160/64) ab Gast-Frame 2550 fuer 20 Gast-Frames - genau
--    wie `port.exe --press touch@g2550:20:160:64`. Mit --deterministic
--    entsteht dieselbe Bildfolge wie mit diesem --press.
-- 2. Hotkey (nur Echtzeit, im deterministischen Modus ist host.* neutral):
--    solange F6 gedrueckt ist, wird der Touchscreen an der Mausposition
--    beruehrt (ohne Klick). Ueber dem Touchscreen zeigt der Zeiger ein
--    Fadenkreuz.

local SCHEDULE = {
  { from = 2550, frames = 20, touch_x = 160, touch_y = 64 },
}

local cursor = "arrow"

local function set_cursor(mode)
  if mode ~= cursor then
    host.set_cursor(mode)
    cursor = mode
  end
end

on_frame(function(frame)
  for _, ev in ipairs(SCHEDULE) do
    if frame >= ev.from and frame < ev.from + ev.frames then
      input.set{ touch = true, touch_x = ev.touch_x, touch_y = ev.touch_y }
      if frame == ev.from and not ev.logged then
        ev.logged = true
        log(string.format("tippe %d/%d ab Gast-Frame %d", ev.touch_x, ev.touch_y, frame))
      end
    end
  end

  local m = host.mouse()
  set_cursor(m.inside_touch and "crosshair" or "arrow")
  if host.key("F6") and m.inside_touch then
    input.set{ touch = true, touch_x = m.x, touch_y = m.y }
  end

  if guest.vblank() % 600 == 0 then
    log(string.format("vblank=%d gast-frame=%d", guest.vblank(), frame))
  end
end)
