#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

rg --files -0 -g '*.h' -g '*.hpp' -g '*.cpp' -g '*.cc' -g '!vk_mem_alloc.h' code/graphics/vulkan \
  | xargs -0 clang-format -i
