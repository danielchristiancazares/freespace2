Param(
    [string]$Config = "Debug",
    [string]$BuildDir = "build",
    [switch]$Vulkan = $true,
    [switch]$ShaderCompilation = $true,
    [string]$CxxStandard = "",
    [switch]$ConfigureOnly,
    [switch]$Clean,
    [string]$Target = "",
    [string]$ShaderToolPath = "",
    [switch]$Tests,
    [switch]$Verbose,
    [switch]$ForceShaders = $true,
    [switch]$FormatVulkan = $true
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

# Format Vulkan sources, excluding vendored vk_mem_alloc.h.
function Format-Vulkan {
    param(
        [bool]$Enable,
        [bool]$Verbose
    )

    if (-not $Enable) {
        return 0
    }

    $clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue
    if (-not $clangFormat) {
        Write-Warning "clang-format not found; skipping Vulkan format."
        return 0
    }

    $files = Get-ChildItem -Path "code/graphics/vulkan" -Recurse -File -Include *.h, *.hpp, *.cpp, *.cc |
        Where-Object { $_.Name -ne "vk_mem_alloc.h" } |
        ForEach-Object { $_.FullName }

    if (-not $files -or $files.Count -eq 0) {
        return 0
    }

    Write-Host "Formatting Vulkan sources... " -NoNewline

    $batchSize = 200
    $output = @()
    $exitCode = 0
    for ($i = 0; $i -lt $files.Count; $i += $batchSize) {
        $end = [Math]::Min($i + $batchSize - 1, $files.Count - 1)
        $slice = $files[$i..$end]
        $chunkOutput = & $clangFormat.Source -i @slice 2>&1
        $chunkExit = $LASTEXITCODE
        if ($Verbose -or $chunkExit -ne 0) {
            $output += $chunkOutput
        }
        if ($chunkExit -ne 0) {
            $exitCode = $chunkExit
            break
        }
    }

    if ($exitCode -ne 0) {
        Write-Host "FAILED" -ForegroundColor Red
        Write-Host ""
        $output | ForEach-Object { Write-Host $_ }
        return $exitCode
    }

    Write-Host "OK" -ForegroundColor Green
    if ($Verbose -and $output.Count -gt 0) {
        $output | ForEach-Object { Write-Host $_ }
    }
    return 0
}

$EnableVulkan = [bool]$Vulkan
$EnableShaderCompilation = [bool]$ShaderCompilation
$EnableConfigureOnly = [bool]$ConfigureOnly
$EnableClean = [bool]$Clean
$EnableTests = [bool]$Tests
$EnableVerbose = [bool]$Verbose
$EnableForceShaders = [bool]$ForceShaders
$EnableFormatVulkan = [bool]$FormatVulkan

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

# Format Vulkan sources before building.
$result = Format-Vulkan -Enable $EnableFormatVulkan -Verbose $EnableVerbose
if ($result -ne 0) {
    exit $result
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
