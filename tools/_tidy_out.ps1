# Aufraeumen von out/: Logs, Screenshots, Probes, PDBs, Artefakte in Unterordner.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$out = Join-Path $root 'out'

foreach ($d in 'logs','shots','probes','pdb','artifacts','backup') {
    $p = Join-Path $out $d
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path $p | Out-Null }
}

# --- Logs ---
$keepLogs = @(
  'boot_log.txt','boot_err.txt','buk4_log.txt','dumpmeta_log.txt','fa_ok_run_log.txt',
  'port_nooff_log.txt','port_canonical_log.txt','port_final_gpu_log.txt','port_gpu_path_log.txt',
  'port_live2_log.txt','port_default_log.txt','port_final_shot_log.txt'
)
$logs = Get-ChildItem $out -File | Where-Object { $_.Name -match '_(log|err)\.txt$' -and $keepLogs -notcontains $_.Name }
$logs | Move-Item -Destination (Join-Path $out 'logs') -Force

# --- Screenshots ---
$keepShots = @(
  'shot.bmp','shot_drawfix.bmp','shot_menu.bmp','shot_official2.bmp','shot_engine.bmp',
  'shot_engine_default.bmp','shot_official.bmp','shot_engine_gpu.bmp','shot_engine_final.bmp','menu_frame.png'
)
$shots = Get-ChildItem $out -File | Where-Object {
  ($_.Extension -in '.bmp','.png') -and $keepShots -notcontains $_.Name
}
$shots | Move-Item -Destination (Join-Path $out 'shots') -Force

# --- Probe-/Alt-Binaries ---
$keepExe = @('boot.exe','port.exe','boot2.exe','link_smoke.exe')
$exes = Get-ChildItem $out -File -Filter *.exe | Where-Object { $keepExe -notcontains $_.Name }
$exes | Move-Item -Destination (Join-Path $out 'probes') -Force

# --- PDBs ---
Get-ChildItem $out -File -Filter *.pdb | Move-Item -Destination (Join-Path $out 'pdb') -Force

# --- Artefakte ---
$artPatterns = @('*.o','*.raw')
foreach ($pat in $artPatterns) {
  Get-ChildItem $out -File -Filter $pat | Move-Item -Destination (Join-Path $out 'artifacts') -Force
}
foreach ($name in 'trace_test.ctrt','trace_test2.ctrt','boot.ctrt','hle_trace.txt','missing_calls.txt','romfs_hdr_patched.bin') {
  $p = Join-Path $out $name
  if (Test-Path $p) { Move-Item $p (Join-Path $out 'artifacts') -Force }
}

# --- Backup ---
$bak = Join-Path $out 'romfs_image.hashpatch.bak'
if (Test-Path $bak) { Move-Item $bak (Join-Path $out 'backup') -Force }

# --- Python-Caches ---
Get-ChildItem $root -Recurse -Directory -Filter '__pycache__' -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -notmatch '\\\.git\\' } | Remove-Item -Recurse -Force
Get-ChildItem $root -Recurse -File -Filter '*.pyc' -ErrorAction SilentlyContinue |
  Where-Object { $_.FullName -notmatch '\\\.git\\' } | Remove-Item -Force

Write-Output ("moved logs={0} shots={1} exes={2} pdbs={3}" -f $logs.Count, $shots.Count, $exes.Count, (Get-ChildItem (Join-Path $out 'pdb') -File).Count)
