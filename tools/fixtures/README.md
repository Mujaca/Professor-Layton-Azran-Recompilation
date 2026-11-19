# Test-Fixtures

`port_boot_20261119.log` ist ein echter, unveraenderter stdout-Mitschnitt des
headless Ports an diesem Datum. Erzeugt mit:

```powershell
$env:HLE_SRV_NAMES="1"; $env:HLE_IPC_LOG="200"; $env:HLE_PERF="1"
.\out\boot.exe out\romfs   # nach ca. 8 s beendet
```

Der Lauf erreicht 17 `[srvname]`-Acquisitions und **kein** `boss:U`; damit ist
er der reproduzierbare Gegenstand fuer den `trace-diff`-Selbsttest
(`tools/selftest_tools.py`). Die Datei ist kein generiertes Artefakt, sondern
ein bewusst eingecheckter Messwert.
