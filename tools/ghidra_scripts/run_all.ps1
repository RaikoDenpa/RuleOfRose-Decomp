param(
    [switch]$Fresh,
    [string]$ProjectDir = (Join-Path $PSScriptRoot '..\..\ghidra\projects'),
    [string]$ProjectName = 'RuleOfRose',
    [string]$ReportDir = (Join-Path ([IO.Path]::GetTempPath()) 'ror-ghidra-reports')
)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$headless = Join-Path $root 'ghidra\support\analyzeHeadless.bat'
$elf = Join-Path $root 'disc\SLES_542.18'
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Java\jdk-26.0.1' }
New-Item -ItemType Directory -Force $ProjectDir, $ReportDir | Out-Null
$ProjectDir = (Resolve-Path $ProjectDir).Path

function Invoke-Headless([string[]]$a) {
    & $headless @a 2>&1 | ForEach-Object { "$_" } |
        Where-Object { $_ -match '\.java> |ERROR|REPORT: (Import|Save|Analysis)' } |
        ForEach-Object { $_ -replace '^INFO\s+', '' -replace ' \(GhidraScript\)\s*$', '' -replace ' \(HeadlessAnalyzer\)\s*$', '' }
    if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless failed ($LASTEXITCODE)" }
}

if ($Fresh) {
    Invoke-Headless @($ProjectDir, $ProjectName, '-import', $elf, '-processor', 'r5900:LE:32:default', '-overwrite')
}
$steps = @(
    @('RoRClasses.java', (Join-Path $ReportDir 'classes')),
    @('RoRSyscalls.java', (Join-Path $ReportDir 'syscalls.tsv')),
    @('RoRSdkStrings.java', (Join-Path $ReportDir 'sdk_strings.tsv')),
    @('RoRJumpTables.java'),
    @('RoRFunctionPointers.java', (Join-Path $ReportDir 'function_pointers.tsv')),
    @('RoRApplyNames.java'),
    @('RoRSourceFiles.java', (Join-Path $ReportDir 'sources'))
)
foreach ($s in $steps) {
    $a = @($ProjectDir, $ProjectName, '-process', 'SLES_542.18', '-noanalysis', '-scriptPath', $PSScriptRoot, '-postScript') + $s
    Invoke-Headless $a
}
Write-Host "reports: $ReportDir"
Write-Host "notes:   python tools\ghidra_scripts\report_classes.py $ReportDir\classes > ...  /  report_sources.py $ReportDir\sources > ..."
