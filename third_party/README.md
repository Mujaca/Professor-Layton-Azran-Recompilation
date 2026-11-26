# Fremdquellen

## Lua 5.4.7 (`third_party/lua/`)

* Quelle: <https://www.lua.org/ftp/lua-5.4.7.tar.gz>
* SHA-256 des Archivs:
  `9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30`
  (stimmt mit der Angabe auf lua.org ueberein)
* Lizenz: MIT, siehe `lua/LICENSE` (Text vom Ende von `lua.h`).
* Abgelegt: `src/*.c`, `src/*.h` und `readme.html` des Archivs, **unveraendert**.
  Anpassungen nur ueber Compiler-Defines in `tools/build.py`
  (`LUA_SOURCES`, `LUA_DEFINES`).
* Verwendet von der Lua-Mod-Schnittstelle (`runtime/modding/lua_host.c`,
  docs/MODDING.md, Stufe B).

Mitgebaut werden nur Kern und die Bibliotheken der Sandbox:

```
lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
lparser lstate lstring ltable ltm lundump lvm lzio      (Kern)
lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib (Bibliotheken)
```

Nicht gebaut: `lua.c`, `luac.c` (eigenstaendige Programme), `linit.c`
(oeffnet alle Bibliotheken; die Runtime oeffnet nur die erlaubten selbst),
`liolib.c`, `loslib.c`, `ldblib.c`, `loadlib.c` (Datei-, Betriebssystem-,
Debug- und DLL-Zugriff, in der Sandbox verboten).
