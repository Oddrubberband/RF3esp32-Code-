param(
    [Parameter(Mandatory=$true)][ValidateSet('custom','devboard')][string]$Board,
    [string]$Port,
    [ValidateSet('Current','Repaired','Previous')][string]$Version = 'Current',
    [string]$PreviousRoot
)
$ErrorActionPreference = 'Stop'
if ($PreviousRoot -and $Version -ne 'Previous') { throw 'PreviousRoot requires -Version Previous.' }
$projectRoot = if ($Version -eq 'Previous') {
    if (-not $PreviousRoot) { throw 'Previous requires -PreviousRoot pointing to the complete preserved implementation.' }; $PreviousRoot
} else { $PSScriptRoot }
$projectRoot = (Resolve-Path -LiteralPath $projectRoot).Path
$environmentName = if ($Board -eq 'custom') { 'rf3_custom_pcb' } else { 'rf3_esp32_devboard' }
$imageBase = if (Test-Path -LiteralPath (Join-Path $projectRoot 'firmware')) {
    Join-Path $projectRoot 'firmware'
} else { Join-Path $projectRoot '.pio\build' }
$imageDirectory = Join-Path $imageBase $environmentName
$manifest = Get-Content -LiteralPath (Join-Path $projectRoot 'Firmware_Images.json') -Raw | ConvertFrom-Json
$selected = if ($manifest.Current) { $manifest.Current } else { $manifest.Repaired }
$expected = $selected.$environmentName
if (-not $expected) { throw 'No verified image hashes for this board. Use the complete release package.' }
foreach ($fileName in @('bootloader.bin','partitions.bin','firmware.bin')) {
    $imagePath = Join-Path $imageDirectory $fileName
    if ((Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash -ne $expected.$fileName) {
        throw "Image changed or wrong package: $imagePath. Use the matching verified images."
    }
}
$pythonPath = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\python.exe'
$esptoolPath = Join-Path $env:USERPROFILE '.platformio\packages\tool-esptoolpy\esptool.py'
if (-not (Test-Path -LiteralPath $pythonPath) -or -not (Test-Path -LiteralPath $esptoolPath)) {
    throw 'PlatformIO Python/esptool was not found. Install PlatformIO and build the firmware target first.'
}
if (-not $Port) { $Port = Read-Host 'Board USB programming COM port (not Bluetooth)' }
if ($Port -notmatch '^COM[1-9][0-9]*$') { throw 'Enter a Windows COM port, for example COM5.' }
Write-Host "Version: $Version | Board: $Board | USB port: $Port"
Write-Host "Verified images: $imageDirectory"
if ($Board -eq 'custom') { Write-Host 'Custom PCB: 16 MB; CE17 / CSN27 / IRQ16.' }
else { Write-Host 'Devboard: 4 MB; CE27 / CSN5 / IRQ26.' }
Write-Host 'Disconnect this board in its GUI and close its serial monitor.'
Write-Host 'Hold BOOT. Tap EN at Connecting; release BOOT when Writing starts.'
$null = Read-Host 'Press Enter to start the manual upload'
& $pythonPath $esptoolPath --chip esp32 --port $Port --baud 115200 --before no_reset --after no_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect 0x1000 (Join-Path $imageDirectory 'bootloader.bin') 0x8000 (Join-Path $imageDirectory 'partitions.bin') 0x10000 (Join-Path $imageDirectory 'firmware.bin')
if ($LASTEXITCODE -ne 0) { throw 'Upload failed. Preserve the error output before retrying.' }
Write-Host 'Upload verified. Release BOOT, then press EN manually to run.'
