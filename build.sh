#!/bin/bash
set -e

BUILD_DIR="build"
BUILD_TYPE="${1:-Debug}"
JOBS="${2:-$(sysctl -n hw.ncpu)}"

# Create build directory if it doesn't exist
mkdir -p "$BUILD_DIR"

# Configure if needed
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "Configuring with CMake..."
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DFSO_BUILD_INCLUDED_LIBS=ON \
        -DFSO_BUILD_TESTS=ON \
        -DFSO_BUILD_WITH_OPENGL=ON \
        -DFSO_BUILD_WITH_VULKAN=ON
fi

# Build
echo "Building with $JOBS jobs..."
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "Build complete. Binary at: $BUILD_DIR/bin/"
