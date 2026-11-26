-- Hilfsmodul fuer require("fmt").
local M = {}

function M.hex32(v)
  return string.format("0x%08X", v)
end

return M
