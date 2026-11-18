param([Parameter(Mandatory)][string]$Dump, [Parameter(Mandatory)][string]$Out, [int]$Seconds = 8, [switch]$Software)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$pcsx2 = Join-Path $root 'tools\bin\pcsx2'
$exe = Join-Path $pcsx2 'pcsx2-qt.exe'
$snaps = Join-Path $pcsx2 'snaps'
if (-not (Test-Path $exe)) { throw "no PCSX2 at $exe (SETUP.md)" }

$ini = Join-Path $pcsx2 'inis\PCSX2.ini'
$renderer = if ($Software) { 13 } else { -1 }
$text = (Get-Content $ini -Raw) -replace '(?ms)^\[EmuCore/GS\]\r?\nRenderer = -?\d+\r?\n', ''
Set-Content $ini -Value ($text.TrimEnd() + "`r`n`r`n[EmuCore/GS]`r`nRenderer = $renderer`r`n") -Encoding ascii
$dumpPath = (Resolve-Path $Dump).Path
$before = @{}
Get-ChildItem $snaps -Recurse -Filter *.png -ErrorAction SilentlyContinue | ForEach-Object { $before[$_.FullName] = 1 }

$p = Start-Process $exe -ArgumentList @('-batch', '-nogui', '--', "`"$dumpPath`"") -PassThru
try {
    Start-Sleep -Seconds $Seconds
    if ($p.HasExited) { throw "PCSX2 exited early (code $($p.ExitCode)); see $pcsx2\logs" }
    $shell = New-Object -ComObject WScript.Shell
    $shot = $null
    for ($try = 0; $try -lt 3 -and -not $shot; $try++) {
        [void]$shell.AppActivate($p.Id)
        Start-Sleep -Milliseconds 700
        $shell.SendKeys('{F8}')
        for ($i = 0; $i -lt 16 -and -not $shot; $i++) {
            Start-Sleep -Milliseconds 250
            $shot = Get-ChildItem $snaps -Recurse -Filter *.png -ErrorAction SilentlyContinue |
                Where-Object { -not $before.ContainsKey($_.FullName) } | Select-Object -First 1
        }
    }
    if (-not $shot) { throw "no screenshot appeared in $snaps" }
    for ($i = 0; ; $i++) {
        Start-Sleep -Milliseconds 500
        try { Copy-Item $shot.FullName $Out -Force -ErrorAction Stop; break } catch { if ($i -ge 10) { throw } }
    }
    Write-Output "PCSX2 screenshot $($shot.Name) -> $Out"
}
finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
