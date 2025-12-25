Param(
    [string]$Config = "Debug",
    [string]$BuildDir = "build",
    [string]$Filter = "",
    [switch]$Rebuild,
    [switch]$Verbose,
    [switch]$VulkanIT
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Run a command, capturing output. Show output only on failure (unless verbose).
function Invoke-Quietly {
    param(
        [string]$Description,
        [string]$Command,
        [string[]]$Arguments,
        [bool]$ShowOutput = $false
    )

    Write-Host "$Description... " -NoNewline

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Command @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }

    if ($exitCode -ne 0) {
        Write-Host "FAILED" -ForegroundColor Red
        Write-Host ""
        $output | ForEach-Object { Write-Host $_ }
        return $exitCode
    }

    if ($ShowOutput) {
        Write-Host "OK" -ForegroundColor Green
        $output | ForEach-Object { Write-Host $_ }
    } else {
        Write-Host "OK" -ForegroundColor Green
    }

    return 0
}

# Rebuild tests if requested
if ($Rebuild) {
    Write-Host "Rebuilding unit tests..." -ForegroundColor Cyan
    $configureArgs = @(
        "-S", ".",
        "-B", $BuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DFSO_BUILD_TESTS=ON"
    )

    $result = Invoke-Quietly -Description "Configuring" -Command "cmake" -Arguments $configureArgs -ShowOutput $Verbose
    if ($result -ne 0) {
        exit $result
    }

    $buildArgs = @(
        "--build", $BuildDir,
        "--config", $Config,
        "--parallel",
        "--target", "unittests"
    )

    $result = Invoke-Quietly -Description "Building" -Command "cmake" -Arguments $buildArgs -ShowOutput $Verbose
    if ($result -ne 0) {
        exit $result
    }

    Write-Host ""
}

# Resolve test executable path
$testExe = Join-Path $BuildDir "bin" $Config "unittests.exe"

# Handle different build directory structures
if (-not (Test-Path $testExe)) {
    $testExe = Join-Path $BuildDir "bin" "unittests.exe"
}

if (-not (Test-Path $testExe)) {
    Write-Error "Test executable not found at '$testExe'. Have you built the tests? Try with -Rebuild"
    exit 1
}

Write-Host "Running unit tests..." -ForegroundColor Cyan

# Set environment for Vulkan integration tests
if ($VulkanIT) {
    $env:FS2_VULKAN_IT = "1"
    Write-Host "Vulkan integration tests enabled (FS2_VULKAN_IT=1)" -ForegroundColor Yellow
}

# Build test arguments
$testArgs = @()
if ($Filter -and $Filter.Trim()) {
    $testArgs += "--gtest_filter=$Filter"
}

# Run tests and capture output for parsing
Write-Host "Tests... " -NoNewline

$previousErrorActionPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    $output = & $testExe @testArgs 2>&1
    $exitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousErrorActionPreference
}

if ($exitCode -ne 0) {
    Write-Host "FAILED" -ForegroundColor Red
    Write-Host ""

    # Parse output to show summary and failed tests
    $lines = $output | ForEach-Object { $_ }
    $failedTests = @()
    $summary = ""

    foreach ($line in $lines) {
        if ($line -match "^\[  FAILED  \]") {
            $failedTests += $line
        }
        if ($line -match "tests? from .* test suites? ran" -or $line -match "passed|failed") {
            if ($line -match "\[.*\]") {
                $summary = $line
            }
        }
    }

    if ($summary) {
        Write-Host $summary
    }

    if ($failedTests.Count -gt 0) {
        Write-Host ""
        Write-Host "Failed tests:" -ForegroundColor Red
        $failedTests | ForEach-Object { Write-Host $_ }
    }

    if ($Verbose) {
        Write-Host ""
        Write-Host "Full output:" -ForegroundColor Gray
        $lines | ForEach-Object { Write-Host $_ }
    }

    exit $exitCode
}

# Parse success summary from output
$summary = ""
$lines = $output | ForEach-Object { $_ }
foreach ($line in $lines) {
    if ($line -match "^\[.*\].*passed" -or $line -match "tests? from .* test suites? ran") {
        $summary = $line
        break
    }
}

if ($summary) {
    Write-Host "OK - $summary" -ForegroundColor Green
} else {
    Write-Host "OK" -ForegroundColor Green
}

Write-Host ""
