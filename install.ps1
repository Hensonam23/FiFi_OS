#Requires -RunAsAdministrator
# FiFi OS Installer — Windows
# Usage: right-click install.bat -> Run as administrator

param()

$ErrorActionPreference = "Stop"
$Host.UI.RawUI.WindowTitle = "FiFi OS Installer"

# ── helpers ───────────────────────────────────────────────────────────────────
function Show-Header {
    param([string]$Step = "", [string]$Title = "FiFi OS Installer")
    Clear-Host
    Write-Host ""
    Write-Host "  ==============================================" -ForegroundColor Cyan
    Write-Host "   FiFi OS Installer  $Step" -ForegroundColor Cyan
    Write-Host "  ==============================================" -ForegroundColor Cyan
    if ($Title -ne "FiFi OS Installer") {
        Write-Host ""
        Write-Host "  $Title" -ForegroundColor White
    }
    Write-Host ""
}

function Pause-For-Enter {
    param([string]$Prompt = "  Press Enter to continue...")
    Write-Host $Prompt -NoNewline -ForegroundColor Gray
    $null = Read-Host
}

# ── STEP 1: Welcome ───────────────────────────────────────────────────────────
Show-Header "" "Welcome"
Write-Host "  Welcome to the FiFi OS Installer!" -ForegroundColor White
Write-Host ""
Write-Host "  This wizard will write FiFi OS to a USB drive." -ForegroundColor Gray
Write-Host "  Steps: locate image -> select drive -> confirm -> write -> done" -ForegroundColor Gray
Write-Host ""
Write-Host "  WARNING: The selected drive will be completely erased." -ForegroundColor Red
Write-Host ""
Pause-For-Enter "  Press Enter to begin or Ctrl+C to exit..."

# ── STEP 2: Locate ISO ────────────────────────────────────────────────────────
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$IsoPath = Join-Path $ScriptDir "fifi.iso"

Show-Header "Step 1/4" "Locating image"
if (-not (Test-Path $IsoPath)) {
    Write-Host "  ERROR: fifi.iso not found." -ForegroundColor Red
    Write-Host ""
    Write-Host "  Place fifi.iso in the same folder as this script:" -ForegroundColor Gray
    Write-Host "    $ScriptDir" -ForegroundColor Yellow
    Write-Host ""
    Pause-For-Enter "  Press Enter to exit..."
    exit 1
}

$IsoSize = (Get-Item $IsoPath).Length
$IsoMB   = [math]::Ceiling($IsoSize / 1MB)
Write-Host "  Found: fifi.iso  ($IsoMB MB)" -ForegroundColor Green
Write-Host ""
Pause-For-Enter

# ── STEP 3: Drive selection ───────────────────────────────────────────────────
Show-Header "Step 2/4" "Select target drive"
Write-Host "  Available drives:" -ForegroundColor Cyan
Write-Host ""

$AllDisks = Get-Disk | Where-Object { $_.Size -gt 50MB } | Sort-Object Number
if ($AllDisks.Count -eq 0) {
    Write-Host "  No drives found. Plug in a USB drive and re-run." -ForegroundColor Red
    Pause-For-Enter; exit 1
}

foreach ($d in $AllDisks) {
    $sizeStr = if ($d.Size -ge 1GB) { "$([math]::Round($d.Size/1GB,1)) GB" } else { "$([math]::Round($d.Size/1MB)) MB" }
    $sysTag  = if ($d.IsSystem) { "  [SYSTEM]" } else { "" }
    $color   = if ($d.IsSystem) { "Red" } else { "White" }
    Write-Host ("  [{0}] {1,-40} {2,8}{3}" -f $d.Number, $d.FriendlyName, $sizeStr, $sysTag) -ForegroundColor $color
}

Write-Host ""
Write-Host "  Drives marked [SYSTEM] contain your operating system." -ForegroundColor DarkGray
Write-Host "  Do NOT select them." -ForegroundColor DarkGray
Write-Host ""
$DriveNum = Read-Host "  Enter drive number"

try { $TargetDisk = Get-Disk -Number ([int]$DriveNum) }
catch {
    Write-Host "  Invalid drive number." -ForegroundColor Red
    Pause-For-Enter; exit 1
}

if ($TargetDisk.IsSystem) {
    Write-Host "  ERROR: You selected the system drive. Aborting." -ForegroundColor Red
    Pause-For-Enter; exit 1
}

# ── STEP 4: Confirm ───────────────────────────────────────────────────────────
$SizeStr = if ($TargetDisk.Size -ge 1GB) { "$([math]::Round($TargetDisk.Size/1GB,1)) GB" } else { "$([math]::Round($TargetDisk.Size/1MB)) MB" }

Show-Header "Step 3/4" "Confirm"
Write-Host "  You are about to ERASE:" -ForegroundColor Yellow
Write-Host ""
Write-Host ("  Drive:  [{0}] {1}" -f $TargetDisk.Number, $TargetDisk.FriendlyName) -ForegroundColor White
Write-Host "  Size:   $SizeStr" -ForegroundColor White
Write-Host "  Image:  fifi.iso ($IsoMB MB)" -ForegroundColor White
Write-Host ""
Write-Host "  THIS CANNOT BE UNDONE." -ForegroundColor Red
Write-Host ""
$Confirm = Read-Host "  Type YES to confirm"
if ($Confirm -ne "YES") {
    Write-Host "  Cancelled." -ForegroundColor Gray
    exit 0
}

# ── STEP 5: Write ─────────────────────────────────────────────────────────────
Show-Header "Step 4/4" "Writing FiFi OS..."

# Remove partition access paths so Windows releases file locks
try {
    Get-Partition -DiskNumber $TargetDisk.Number -ErrorAction SilentlyContinue |
        ForEach-Object { $_ | Remove-PartitionAccessPath -AccessPath ($_.AccessPaths | Select-Object -First 1) -ErrorAction SilentlyContinue }
} catch { }

$DiskPath = "\\.\PhysicalDrive$($TargetDisk.Number)"
Write-Host "  Target:  $DiskPath" -ForegroundColor Gray
Write-Host "  Writing: $IsoMB MB" -ForegroundColor Gray
Write-Host ""

try {
    $reader = [System.IO.File]::OpenRead($IsoPath)
    $writer = [System.IO.File]::Open($DiskPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)

    $buf     = New-Object byte[] (4MB)
    $total   = $reader.Length
    $written = 0

    while (($n = $reader.Read($buf, 0, $buf.Length)) -gt 0) {
        $writer.Write($buf, 0, $n)
        $written += $n
        $pct = [math]::Round($written * 100 / $total)
        Write-Progress -Activity "Writing FiFi OS" -Status "$pct% — $([math]::Round($written/1MB)) / $IsoMB MB" -PercentComplete $pct
    }

    $writer.Flush()
} finally {
    if ($reader) { $reader.Close() }
    if ($writer) { $writer.Close() }
    Write-Progress -Activity "Writing FiFi OS" -Completed
}

Write-Host "  Write complete." -ForegroundColor Green
Write-Host ""

# ── STEP 6: Done ─────────────────────────────────────────────────────────────
Show-Header "" "Done!"
Write-Host "  FiFi OS has been installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host ("  Drive:  [{0}] {1}" -f $TargetDisk.Number, $TargetDisk.FriendlyName) -ForegroundColor White
Write-Host "  Written: $IsoMB MB" -ForegroundColor White
Write-Host ""
Write-Host "  You can now safely remove the drive." -ForegroundColor Gray
Write-Host "  In BIOS/UEFI, set USB as first boot device." -ForegroundColor Gray
Write-Host ""
Pause-For-Enter "  Press Enter to exit..."
