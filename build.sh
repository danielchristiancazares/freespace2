#!/bin/bash
set -e

usage() {
  cat <<'EOF'
Usage: ./build.sh [options] [build_type] [jobs]

Options:
  -c, --config <config>            Build configuration (Debug, Release, etc)
  -B, --build-dir <dir>            Build directory
  -j, --jobs <n>                   Parallel build jobs
  --vulkan | --no-vulkan           Enable/disable Vulkan renderer (default: on)
  --shader-compilation | --no-shader-compilation
                                   Enable/disable shader compilation (default: on)
  --cxx-standard <n>               C++ standard (e.g. 17, 20, 23)
  --configure-only                 Configure and exit
  --clean                          Remove build directory before configuring
  --target <name>                  Build target
  --shader-tool-path <path>        Override shadertool path
  --tests | --no-tests             Enable/disable unit tests (auto on for unittests target)
  --verbose                        Show command output
  --force-shaders | --no-force-shaders
                                   Force shader recompilation by deleting generated outputs
  --format-vulkan | --no-format-vulkan
                                   Run clang-format on Vulkan sources before build
  -h, --help                       Show this help
EOF
}

die() {
  echo "Error: $*" >&2
  exit 1
}

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

detect_jobs() {
  if command_exists nproc; then
    nproc
    return
  fi
  if command_exists sysctl; then
    sysctl -n hw.ncpu
    return
  fi
  getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1
}

abs_path() {
  local path="$1"
  if [ -d "$path" ]; then
    (cd "$path" && pwd -P)
  else
    (cd "$(dirname "$path")" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$path")")
  fi
}

run_quietly() {
  local description="$1"
  local show_output="$2"
  shift 2

  printf "%s... " "$description"

  local output
  local exit_code
  set +e
  output=$("$@" 2>&1)
  exit_code=$?
  set -e

  if [ "$exit_code" -ne 0 ]; then
    echo "FAILED"
    echo ""
    echo "$output"
    return "$exit_code"
  fi

  echo "OK"
  if [ "$show_output" -eq 1 ] && [ -n "$output" ]; then
    echo "$output"
  fi
  return 0
}

format_vulkan() {
  local enable="$1"
  local verbose="$2"

  if [ "$enable" -eq 0 ]; then
    return 0
  fi

  if ! command_exists clang-format; then
    echo "clang-format not found; skipping Vulkan format."
    return 0
  fi

  local files=()
  local file
  while IFS= read -r -d '' file; do
    files+=("$file")
  done < <(
    find code/graphics/vulkan -type f \
      \( -name '*.h' -o -name '*.hpp' -o -name '*.cpp' -o -name '*.cc' \) \
      ! -name 'vk_mem_alloc.h' -print0
  )

  if [ "${#files[@]}" -eq 0 ]; then
    return 0
  fi

  printf "Formatting Vulkan sources... "

  local batch_size=200
  local i=0
  local exit_code=0
  local output=""

  set +e
  while [ "$i" -lt "${#files[@]}" ]; do
    local slice=("${files[@]:$i:$batch_size}")
    if [ "$verbose" -eq 1 ]; then
      clang-format -i "${slice[@]}"
      exit_code=$?
    else
      output=$(clang-format -i "${slice[@]}" 2>&1)
      exit_code=$?
    fi
    if [ "$exit_code" -ne 0 ]; then
      break
    fi
    i=$((i + batch_size))
  done
  set -e

  if [ "$exit_code" -ne 0 ]; then
    echo "FAILED"
    echo ""
    if [ -n "$output" ]; then
      echo "$output"
    fi
    return "$exit_code"
  fi

  echo "OK"
  return 0
}

BUILD_DIR="build"
BUILD_TYPE="Debug"
JOBS="$(detect_jobs)"
ENABLE_VULKAN=1
ENABLE_SHADER_COMPILATION=1
ENABLE_CONFIGURE_ONLY=0
ENABLE_CLEAN=0
TARGET=""
SHADER_TOOL_PATH=""
ENABLE_TESTS=0
ENABLE_VERBOSE=0
ENABLE_FORCE_SHADERS=1
ENABLE_FORMAT_VULKAN=1
CXX_STANDARD=""

CONFIG_SET=0
JOBS_SET=0
POSITIONAL=()

while [ "$#" -gt 0 ]; do
  case "$1" in
    -c|--config)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      BUILD_TYPE="$2"
      CONFIG_SET=1
      shift 2
      ;;
    -B|--build-dir)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      BUILD_DIR="$2"
      shift 2
      ;;
    -j|--jobs)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      JOBS="$2"
      JOBS_SET=1
      shift 2
      ;;
    --vulkan)
      ENABLE_VULKAN=1
      shift
      ;;
    --no-vulkan)
      ENABLE_VULKAN=0
      shift
      ;;
    --shader-compilation)
      ENABLE_SHADER_COMPILATION=1
      shift
      ;;
    --no-shader-compilation)
      ENABLE_SHADER_COMPILATION=0
      shift
      ;;
    --cxx-standard)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      CXX_STANDARD="$2"
      shift 2
      ;;
    --configure-only)
      ENABLE_CONFIGURE_ONLY=1
      shift
      ;;
    --clean)
      ENABLE_CLEAN=1
      shift
      ;;
    --target)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      TARGET="$2"
      shift 2
      ;;
    --shader-tool-path)
      [ "$#" -ge 2 ] || die "Missing value for $1"
      SHADER_TOOL_PATH="$2"
      shift 2
      ;;
    --tests)
      ENABLE_TESTS=1
      shift
      ;;
    --no-tests)
      ENABLE_TESTS=0
      shift
      ;;
    --verbose)
      ENABLE_VERBOSE=1
      shift
      ;;
    --force-shaders)
      ENABLE_FORCE_SHADERS=1
      shift
      ;;
    --no-force-shaders)
      ENABLE_FORCE_SHADERS=0
      shift
      ;;
    --format-vulkan)
      ENABLE_FORMAT_VULKAN=1
      shift
      ;;
    --no-format-vulkan)
      ENABLE_FORMAT_VULKAN=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      POSITIONAL+=("$@")
      break
      ;;
    -*)
      die "Unknown option: $1"
      ;;
    *)
      POSITIONAL+=("$1")
      shift
      ;;
  esac
done

if [ "${#POSITIONAL[@]}" -gt 0 ]; then
  if [ "$CONFIG_SET" -eq 0 ]; then
    BUILD_TYPE="${POSITIONAL[0]}"
  fi
  if [ "${#POSITIONAL[@]}" -gt 1 ] && [ "$JOBS_SET" -eq 0 ]; then
    JOBS="${POSITIONAL[1]}"
  fi
  if [ "${#POSITIONAL[@]}" -gt 2 ]; then
    die "Too many positional arguments"
  fi
fi

if [ -n "$CXX_STANDARD" ] && ! [[ "$CXX_STANDARD" =~ ^[0-9]+$ ]]; then
  die "CxxStandard must be a numeric value like '17', '20', or '23'. Got '$CXX_STANDARD'."
fi

if ! [[ "$JOBS" =~ ^[0-9]+$ ]]; then
  die "Jobs must be a numeric value. Got '$JOBS'."
fi

if [ "$ENABLE_TESTS" -eq 0 ] && [[ "$TARGET" == *unittests* ]]; then
  ENABLE_TESTS=1
fi

RESOLVED_SHADER_TOOL_PATH=""
if [ -n "$SHADER_TOOL_PATH" ]; then
  if [ ! -e "$SHADER_TOOL_PATH" ]; then
    die "Provided ShaderToolPath '$SHADER_TOOL_PATH' does not exist."
  fi
  RESOLVED_SHADER_TOOL_PATH="$(abs_path "$SHADER_TOOL_PATH")"
else
  for candidate in \
    "$HOME/Documents/fso-shadertool/build/shadertool/Release/shadertool" \
    "$HOME/Documents/fso-shadertool/build/shadertool/Release/shadertool.exe" \
    "${USERPROFILE:-}/Documents/fso-shadertool/build/shadertool/Release/shadertool.exe"
  do
    if [ -n "$candidate" ] && [ -e "$candidate" ]; then
      RESOLVED_SHADER_TOOL_PATH="$(abs_path "$candidate")"
      break
    fi
  done
fi

if [ "$ENABLE_CLEAN" -eq 1 ] && [ -d "$BUILD_DIR" ]; then
  printf "Cleaning... "
  rm -rf "$BUILD_DIR"
  echo "OK"
fi

if [ "$ENABLE_FORCE_SHADERS" -eq 1 ] && [ "$ENABLE_CLEAN" -eq 0 ]; then
  generated_shaders_dir="$BUILD_DIR/generated_shaders"
  shader_dep_dir="$BUILD_DIR/code/shaders"
  printed=0
  deleted=0

  if [ -d "$generated_shaders_dir" ]; then
    printf "Forcing shader recompilation... "
    rm -rf "$generated_shaders_dir"
    printed=1
    deleted=1
  fi
  if [ -d "$shader_dep_dir" ]; then
    if [ "$printed" -eq 0 ]; then
      printf "Forcing shader recompilation... "
      printed=1
    fi
    rm -rf "$shader_dep_dir"
    deleted=1
  fi

  if [ "$deleted" -eq 1 ]; then
    echo "OK"
  fi
fi

configure_args=(
  -S .
  -B "$BUILD_DIR"
  -G Ninja
  "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
)
if [ "$ENABLE_VULKAN" -eq 1 ]; then
  configure_args+=("-DFSO_BUILD_WITH_VULKAN=ON")
else
  configure_args+=("-DFSO_BUILD_WITH_VULKAN=OFF")
fi
if [ "$ENABLE_TESTS" -eq 1 ]; then
  configure_args+=("-DFSO_BUILD_TESTS=ON")
else
  configure_args+=("-DFSO_BUILD_TESTS=OFF")
fi
if [ "$ENABLE_SHADER_COMPILATION" -eq 1 ]; then
  configure_args+=("-DSHADERS_ENABLE_COMPILATION=ON")
else
  configure_args+=("-DSHADERS_ENABLE_COMPILATION=OFF")
fi
if [ -n "$RESOLVED_SHADER_TOOL_PATH" ] && [ "$ENABLE_SHADER_COMPILATION" -eq 1 ]; then
  configure_args+=("-DSHADERTOOL_PATH=$RESOLVED_SHADER_TOOL_PATH")
fi
if [ -n "$CXX_STANDARD" ]; then
  configure_args+=("-DCMAKE_CXX_STANDARD=$CXX_STANDARD")
  configure_args+=("-DCMAKE_CXX_STANDARD_REQUIRED=ON")
  configure_args+=("-DCMAKE_CXX_EXTENSIONS=OFF")
fi

run_quietly "Configuring" "$ENABLE_VERBOSE" cmake "${configure_args[@]}"

if [ "$ENABLE_CONFIGURE_ONLY" -eq 1 ]; then
  echo ""
  echo "Configure succeeded."
  exit 0
fi

format_vulkan "$ENABLE_FORMAT_VULKAN" "$ENABLE_VERBOSE"

build_args=(
  --build "$BUILD_DIR"
  --config "$BUILD_TYPE"
  --parallel "$JOBS"
)
if [ -n "$TARGET" ]; then
  build_args+=(--target "$TARGET")
fi

run_quietly "Building" "$ENABLE_VERBOSE" cmake "${build_args[@]}"

echo ""
echo "Build succeeded."
