$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Tool paths (PlatformIO installs these under %USERPROFILE%\.platformio)
# ---------------------------------------------------------------------------
$PioExe      = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$PythonExe   = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$EsptoolPy   = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"

$FirmwareDir    = $PSScriptRoot
$BuildDir       = Join-Path $FirmwareDir ".pio\build\nodemcuv2"
$FirmwareBin    = Join-Path $BuildDir "firmware.bin"
$LittlefsBin    = Join-Path $BuildDir "littlefs.bin"
$MergedBin      = Join-Path $FirmwareDir "elmahdy-relay-full.bin"
$LittlefsOffset = "0x200000"
$MaxBytes       = 1048576

function banner([string]$msg) {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
}

function gzip_file([string]$src) {
    $dst = "$src.gz"
    $srcInfo = Get-Item $src
    if ((Test-Path $dst) -and ((Get-Item $dst).LastWriteTime -ge $srcInfo.LastWriteTime)) {
        Write-Host "  [skip] $([IO.Path]::GetFileName($dst))  (up to date)"
        return
    }
    $bytes = [IO.File]::ReadAllBytes($src)
    $ms    = New-Object IO.MemoryStream
    $gz    = New-Object IO.Compression.GZipStream($ms, [IO.Compression.CompressionLevel]::Optimal)
    $gz.Write($bytes, 0, $bytes.Length)
    $gz.Close()
    [IO.File]::WriteAllBytes($dst, $ms.ToArray())
    $orig = $bytes.Length
    $comp = $ms.ToArray().Length
    $pct  = [math]::Round((1 - $comp / [double]$orig) * 100, 1)
    Write-Host ("  [gzip] {0,-32} {1,6} -> {2,6} bytes  ({3}% saved)" -f `
        [IO.Path]::GetFileName($dst), $orig, $comp, $pct)
}

# ---------------------------------------------------------------------------
banner "Checking prerequisites"

foreach ($f in @($PioExe, $PythonExe, $EsptoolPy)) {
    if (-not (Test-Path $f)) {
        Write-Error "Not found: $f`nMake sure PlatformIO is installed via VS Code or pip."
        exit 1
    }
}
$pioVer = & $PioExe --version 2>&1 | Select-Object -First 1
Write-Host "  pio     : $pioVer"
Write-Host "  esptool : $EsptoolPy"
Write-Host "  Output  : $MergedBin"

# ---------------------------------------------------------------------------
banner "Step 1/4 - Gzipping web assets"

$wwwDir  = Join-Path $FirmwareDir "data\www"
$dataDir = Join-Path $FirmwareDir "data"
$skip    = @("sw.js", "manifest.json")

foreach ($ext in @("*.html","*.css","*.js","*.json")) {
    Get-ChildItem -Path $wwwDir -Filter $ext -File |
        Where-Object { $_.Name -notin $skip } |
        ForEach-Object { gzip_file $_.FullName }
}
Get-ChildItem -Path $dataDir -Filter "lang_*.json" -File |
    ForEach-Object { gzip_file $_.FullName }

Write-Host "  Done."

# ---------------------------------------------------------------------------
banner "Step 2/4 - Building LittleFS image"

Push-Location $FirmwareDir
& $PioExe run -e nodemcuv2 -t buildfs
if ($LASTEXITCODE -ne 0) { throw "pio buildfs failed (exit $LASTEXITCODE)" }
Pop-Location

if (-not (Test-Path $LittlefsBin)) { throw "LittleFS image not found: $LittlefsBin" }
$lfsKb = [math]::Round((Get-Item $LittlefsBin).Length / 1024.0, 1)
Write-Host "  LittleFS image: $lfsKb KB"

# ---------------------------------------------------------------------------
banner "Step 3/4 - Compiling firmware"

Push-Location $FirmwareDir
& $PioExe run -e nodemcuv2
if ($LASTEXITCODE -ne 0) { throw "pio run failed (exit $LASTEXITCODE)" }
Pop-Location

if (-not (Test-Path $FirmwareBin)) { throw "Firmware binary not found: $FirmwareBin" }
$fwSize = (Get-Item $FirmwareBin).Length
$fwKb   = [math]::Round($fwSize / 1024.0, 1)
Write-Host "  Firmware: $fwKb KB"

# ---------------------------------------------------------------------------
banner "Step 4/4 - Merging firmware.bin + littlefs.bin"

Write-Host "  Flash map:"
Write-Host "    0x000000  ->  firmware.bin   (app code)"
Write-Host "    $LittlefsOffset  ->  littlefs.bin   (filesystem)"
Write-Host ""

& $PythonExe $EsptoolPy --chip esp8266 merge_bin `
    --output $MergedBin `
    --target-offset 0x0 `
    0x0 $FirmwareBin `
    $LittlefsOffset $LittlefsBin

if ($LASTEXITCODE -ne 0) { throw "esptool.py merge_bin failed (exit $LASTEXITCODE)" }
if (-not (Test-Path $MergedBin)) { throw "Merged binary not produced at $MergedBin" }

$mergedSize = (Get-Item $MergedBin).Length
$mergedKb   = [math]::Round($mergedSize / 1024.0, 1)

# Validate firmware portion only (must fit in the 1MB OTA slot).
if ($fwSize -gt $MaxBytes) {
    $limitKb = [math]::Round($MaxBytes / 1024.0, 1)
    throw "Firmware binary too large: $fwKb KB > $limitKb KB (1 MB OTA limit)"
}

$fwRemaining = $MaxBytes - $fwSize
Write-Host ("  Size check: PASS  firmware {0} KB, {1} bytes to spare" -f $fwKb, $fwRemaining) `
    -ForegroundColor Green

# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  BUILD COMPLETE" -ForegroundColor Green
Write-Host "============================================================" -ForegroundColor Green
Write-Host "  Firmware  : $fwKb KB"
Write-Host "  LittleFS  : $lfsKb KB"
Write-Host "  Merged    : $mergedKb KB  (limit 1024 KB)"
Write-Host ""
Write-Host "  Output:" -ForegroundColor Yellow
Write-Host "    $MergedBin" -ForegroundColor Yellow
Write-Host ""
Write-Host "  Flash with Tasmotizer:"
Write-Host "    File   : elmahdy-relay-full.bin"
Write-Host "    Offset : 0x0  (tick Erase Flash first)"
Write-Host "============================================================" -ForegroundColor Green
