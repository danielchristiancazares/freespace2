Param(
    [string]$Dump,

    [string]$SymbolPath = "build\bin\Debug",

    [string]$CdbPath = "C:\Users\danie\AppData\Local\Microsoft\WindowsApps\cdbx64.exe",

    [switch]$Help
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

if ($Help) {
    Write-Host "Analyzes the newest fs2_*.mdmp dump under build\bin\Debug by default."
    Write-Host ""
    Write-Host "Usage:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_minidump.ps1"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\analyze_minidump.ps1 -Dump path\to\dump.mdmp"
    Write-Host ""
    Write-Host "Optional parameters:"
    Write-Host "  -Dump        Specific dump file (otherwise newest fs2_*.mdmp is used)"
    Write-Host "  -SymbolPath  Directory containing symbols (default: build\bin\Debug)"
    Write-Host "  -CdbPath     Path to cdbx64.exe (default: $CdbPath)"
    exit 0
}

function Get-LatestDump {
    param(
        [string]$SearchRoot
    )

    $pattern = "fs2_*.mdmp"
    $dumpDir = Join-Path $SearchRoot "build\bin\Debug"
    if (-not (Test-Path -LiteralPath $dumpDir)) {
        return $null
    }

    $latest = Get-ChildItem -Path $dumpDir -Filter $pattern -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    return $latest
}

if (-not $Dump) {
    $candidate = Get-LatestDump -SearchRoot $RepoRoot
    if (-not $candidate) {
        Write-Error "No dump files matching fs2_*.mdmp found under $RepoRoot\build\bin\Debug. Provide -Dump explicitly."
    }
    $Dump = $candidate.FullName
    Write-Host "Using latest dump: $Dump"
}

Set-Location $RepoRoot

if (-not (Test-Path -LiteralPath $Dump)) {
    Write-Error "Dump file not found: $Dump"
}

if (-not (Test-Path -LiteralPath $SymbolPath)) {
    Write-Error "Symbol path not found: $SymbolPath"
}

if (-not (Test-Path -LiteralPath $CdbPath)) {
    Write-Error "cdbx64.exe not found at: $CdbPath"
}

$DumpFull = (Resolve-Path -LiteralPath $Dump).Path
$SymFull = (Resolve-Path -LiteralPath $SymbolPath).Path

# Create a temporary debugger script file to avoid argument escaping issues
$ScriptFile = [System.IO.Path]::GetTempFileName()
try {
    $ScriptContent = @"
.symfix
.sympath+ "$SymFull"
.reload
!analyze -v
q
"@
    Set-Content -Path $ScriptFile -Value $ScriptContent -Encoding ASCII

    $rawOutput = & $CdbPath -z $DumpFull -cf $ScriptFile 2>&1 | Out-String

    # Extract just the essential info
    $lines = $rawOutput -split "`r?`n"

    # Find failure bucket
    $failureBucket = ""
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'Key\s*:\s*Failure\.Bucket') {
            if ($i + 1 -lt $lines.Count -and $lines[$i + 1] -match 'Value:\s*(.+)') {
                $failureBucket = $Matches[1].Trim()
            }
        }
    }

    # Extract stack trace (simplified - just function names)
    $inStack = $false
    $stack = @()
    foreach ($line in $lines) {
        if ($line -match '^STACK_TEXT:') {
            $inStack = $true
            continue
        }
        if ($inStack) {
            # Stop at next section or empty line after stack
            if ($line -match '^\w+:' -and $line -notmatch '^[0-9a-f]') {
                break
            }
            # Parse stack frame - extract symbol (everything after last colon before the offset)
            if ($line -match ':\s*([^:]+![^\+]+)') {
                $symbol = $Matches[1].Trim()
                # Skip noise
                if ($symbol -match 'common_assert|_wassert|__scrt_|WinMainCRTStartup|BaseThreadInitThunk|RtlUserThreadStart') { continue }
                if ($symbol -match 'scalar deleting destructor|std::unique_ptr|std::default_delete|invoke_main') { continue }
                $stack += $symbol
            }
        }
    }

    # Output concise summary
    Write-Host ""
    Write-Host "Crash: $failureBucket" -ForegroundColor Red
    Write-Host ""
    Write-Host "Stack:" -ForegroundColor Yellow
    foreach ($frame in $stack) {
        # Strip common module prefix for readability
        $short = $frame -replace '^fs2_open_[^!]+!', ''
        Write-Host "  $short"
    }
}
finally {
    if (Test-Path $ScriptFile) {
        Remove-Item $ScriptFile -ErrorAction SilentlyContinue
    }
}
