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

# Ensure the MSVC developer environment is loaded when using Ninja on Windows.
# Without this, builds can fail due to missing standard headers (e.g. float.h) if the
# script is run outside a Developer PowerShell/Command Prompt.
function Ensure-MsvcEnvironment {
    function Test-HeaderInIncludePath {
        param([string]$Header)

        if (-not $env:INCLUDE) {
            return $false
        }

        foreach ($dir in ($env:INCLUDE -split ";")) {
            if (-not $dir) { continue }
            if (Test-Path (Join-Path $dir $Header)) {
                return $true
            }
        }

        return $false
    }

    if (Test-HeaderInIncludePath "float.h") {
        return
    }

    $pf86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if (-not $pf86) {
        Write-Error "MSVC environment not detected (VCToolsInstallDir unset) and ProgramFiles(x86) is missing."
        exit 1
    }

    $vswhere = Join-Path $pf86 "Microsoft Visual Studio\\Installer\\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Write-Error "MSVC environment not detected (VCToolsInstallDir unset) and vswhere.exe was not found at '$vswhere'."
        exit 1
    }

    # Request a VS instance with the C++ toolset installed.
    $installPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) |
        Select-Object -First 1
    if (-not $installPath) {
        Write-Error "vswhere.exe did not find a Visual Studio instance with the C++ toolset installed."
        exit 1
    }

    $vsDevCmd = Join-Path $installPath "Common7\\Tools\\VsDevCmd.bat"
    if (-not (Test-Path $vsDevCmd)) {
        Write-Error "VsDevCmd.bat was not found at '$vsDevCmd'."
        exit 1
    }

    Write-Host "Setting up Visual Studio build environment... " -NoNewline
    $env:VSCMD_SKIP_SENDTELEMETRY = "1"

    # Import environment variables set by VsDevCmd into the current PowerShell session.
    $dump = & cmd.exe /c "call `"$vsDevCmd`" -no_logo -arch=x64 -host_arch=x64 && set" 2>$null
    foreach ($line in $dump) {
        if ($line -match "^(.*?)=(.*)$") {
            $name = $matches[1]
            $value = $matches[2]
            # Skip special cmd.exe internal vars like "=C:".
            if ($name.StartsWith("=")) { continue }
            Set-Item -Path ("Env:" + $name) -Value $value
        }
    }

    if (-not (Test-HeaderInIncludePath "float.h")) {
        Write-Host "FAILED" -ForegroundColor Red
        Write-Error "Failed to initialize the MSVC developer environment (standard headers still missing from INCLUDE). Try running from a Visual Studio Developer PowerShell."
        exit 1
    }

    Write-Host "OK" -ForegroundColor Green
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

# Make sure we have the MSVC environment before configuring/building with Ninja.
Ensure-MsvcEnvironment

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
