Param(
    [string]$Config = "Debug",
    [string]$BuildDir = "build",
    [string]$Vulkan = "true",
    [string]$ShaderCompilation = "true",
    [string]$Clean = "false",
    [string]$Target = "",
    [string]$ShaderToolPath = "",
    [string]$Tests = "false"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Convert string params to booleans (handles "true", "false", "1", "0", etc.)
function ConvertTo-Bool([string]$value) {
    switch ($value.ToLower()) {
        { $_ -in "true", "1", "yes", "on" } { return $true }
        { $_ -in "false", "0", "no", "off" } { return $false }
        default { return [bool]$value }
    }
}

$EnableVulkan = ConvertTo-Bool $Vulkan
$EnableShaderCompilation = ConvertTo-Bool $ShaderCompilation
$EnableClean = ConvertTo-Bool $Clean
$EnableTests = ConvertTo-Bool $Tests

# Auto-enable tests if we're explicitly building the test target
if (-not $EnableTests -and $Target -match "unittests") {
    $EnableTests = $true
}

$ResolvedShaderToolPath = $null
if ($ShaderToolPath -and $ShaderToolPath.Trim()) {
    if (-not (Test-Path $ShaderToolPath)) {
        throw "Provided ShaderToolPath '$ShaderToolPath' does not exist."
    }
    $ResolvedShaderToolPath = (Resolve-Path -LiteralPath $ShaderToolPath).Path
} else {
    $defaultShaderToolPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) "Documents/fso-shadertool/build/shadertool/Release/shadertool.exe"
    if (Test-Path $defaultShaderToolPath) {
        $ResolvedShaderToolPath = (Resolve-Path -LiteralPath $defaultShaderToolPath).Path
    }
}

# Clean build directory if requested (default: yes)
if ($EnableClean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory: $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# Configure (idempotent; re-runs CMake to pick up changes)
$configureArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-DCMAKE_BUILD_TYPE=$Config"
)
if ($EnableVulkan) {
    $configureArgs += "-DFSO_BUILD_WITH_VULKAN=ON"
} else {
    $configureArgs += "-DFSO_BUILD_WITH_VULKAN=OFF"
}
if ($EnableTests) {
    $configureArgs += "-DFSO_BUILD_TESTS=ON"
} else {
    $configureArgs += "-DFSO_BUILD_TESTS=OFF"
}
if ($EnableShaderCompilation) {
    $configureArgs += "-DSHADERS_ENABLE_COMPILATION=ON"
} else {
    $configureArgs += "-DSHADERS_ENABLE_COMPILATION=OFF"
}
if ($ResolvedShaderToolPath -and $EnableShaderCompilation) {
    Write-Host "Using shadertool at $ResolvedShaderToolPath"
    $configureArgs += "-DSHADERTOOL_PATH=$ResolvedShaderToolPath"
}

cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

# Build (optionally with an explicit target, e.g., -Target clean or -Target unittests)
$buildArgs = @(
    "--build", $BuildDir,
    "--config", $Config,
    "--parallel"
)
if ($Target -ne "") {
    $buildArgs += @("--target", $Target)
}

cmake @buildArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}
