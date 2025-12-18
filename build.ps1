Param(
    [string]$Config = "Debug",
    [string]$BuildDir = "build",
    [string]$Vulkan = "true",
    [string]$ShaderCompilation = "true",
    [string]$CxxStandard = "",
    [string]$ConfigureOnly = "false",
    [string]$Clean = "false",
    [string]$Target = "",
    [string]$ShaderToolPath = "",
    [string]$Tests = "false",
    [string]$Verbose = "false",
    [string]$ForceShaders = "true"
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

$EnableVulkan = ConvertTo-Bool $Vulkan
$EnableShaderCompilation = ConvertTo-Bool $ShaderCompilation
$EnableConfigureOnly = ConvertTo-Bool $ConfigureOnly
$EnableClean = ConvertTo-Bool $Clean
$EnableTests = ConvertTo-Bool $Tests
$EnableVerbose = ConvertTo-Bool $Verbose
$EnableForceShaders = ConvertTo-Bool $ForceShaders

# Auto-enable tests if we're explicitly building the test target
if (-not $EnableTests -and $Target -match "unittests") {
    $EnableTests = $true
}

$ResolvedShaderToolPath = $null
if ($ShaderToolPath -and $ShaderToolPath.Trim()) {
    if (-not (Test-Path $ShaderToolPath)) {
        Write-Error "Provided ShaderToolPath '$ShaderToolPath' does not exist."
        exit 1
    }
    $ResolvedShaderToolPath = (Resolve-Path -LiteralPath $ShaderToolPath).Path
} else {
    $defaultShaderToolPath = Join-Path ([Environment]::GetFolderPath('UserProfile')) "Documents/fso-shadertool/build/shadertool/Release/shadertool.exe"
    if (Test-Path $defaultShaderToolPath) {
        $ResolvedShaderToolPath = (Resolve-Path -LiteralPath $defaultShaderToolPath).Path
    }
}

# Clean build directory if requested
if ($EnableClean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning... " -NoNewline
    Remove-Item -Recurse -Force $BuildDir
    Write-Host "OK" -ForegroundColor Green
}

# Force shader recompilation by deleting generated shaders and depfiles
if ($EnableForceShaders -and -not $EnableClean) {
    $generatedShadersDir = Join-Path $BuildDir "generated_shaders"
    $shaderDepDir = Join-Path $BuildDir "code/shaders"
    $deleted = $false

    if (Test-Path $generatedShadersDir) {
        Write-Host "Forcing shader recompilation... " -NoNewline
        Remove-Item -Recurse -Force $generatedShadersDir
        $deleted = $true
    }
    if (Test-Path $shaderDepDir) {
        if (-not $deleted) { Write-Host "Forcing shader recompilation... " -NoNewline }
        Remove-Item -Recurse -Force $shaderDepDir
        $deleted = $true
    }
    if ($deleted) {
        Write-Host "OK" -ForegroundColor Green
    }
}

# Configure
$configureArgs = @(
    "-S", ".",
    "-B", $BuildDir,
    "-G", "Ninja",
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
    $configureArgs += "-DSHADERTOOL_PATH=$ResolvedShaderToolPath"
}

if ($CxxStandard -and $CxxStandard.Trim()) {
    if ($CxxStandard -notmatch '^\d+$') {
        Write-Error "CxxStandard must be a numeric value like '17', '20', or '23'. Got '$CxxStandard'."
        exit 1
    }
    $configureArgs += "-DCMAKE_CXX_STANDARD=$CxxStandard"
    $configureArgs += "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
    $configureArgs += "-DCMAKE_CXX_EXTENSIONS=OFF"
}

$result = Invoke-Quietly -Description "Configuring" -Command "cmake" -Arguments $configureArgs -ShowOutput $EnableVerbose
if ($result -ne 0) {
    exit $result
}

if ($EnableConfigureOnly) {
    Write-Host ""
    Write-Host "Configure succeeded." -ForegroundColor Green
    exit 0
}

# Build
$buildArgs = @(
    "--build", $BuildDir,
    "--config", $Config,
    "--parallel"
)
if ($Target -ne "") {
    $buildArgs += @("--target", $Target)
}

$result = Invoke-Quietly -Description "Building" -Command "cmake" -Arguments $buildArgs -ShowOutput $EnableVerbose
if ($result -ne 0) {
    exit $result
}

Write-Host ""
Write-Host "Build succeeded." -ForegroundColor Green
