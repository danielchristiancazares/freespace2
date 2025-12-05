# Crash fix: ImGui SDL backend shutdown when using Vulkan

**Status**: **FIXED**

## Problem

Exiting the game with the Vulkan renderer enabled showed a Microsoft Visual C++ Runtime assertion dialog:
- `Assertion failed! ... imgui_impl_sdl.cpp Line: 423`
- Expression: `bd != nullptr && "No platform backend to shutdown, or already shutdown?"`

## Root cause

- `SDLGraphicsOperations::~SDLGraphicsOperations` always called `ImGui_ImplSDL2_Shutdown()` and `ImGui_ImplOpenGL3_Shutdown()`.
- The ImGui SDL/OpenGL backends are only initialized in the OpenGL path (`createOpenGLContext` via `ImGui_ImplSDL2_InitForOpenGL` / `ImGui_ImplOpenGL3_Init`).
- When running with `-vulkan`, those backends are never initialized, so `ImGui_ImplSDL2_GetBackendData()` returned `nullptr` and the shutdown assert fired.

## Fix

In `freespace2/SDLGraphicsOperations.cpp`, guard the ImGui shutdown calls so they only run for the OpenGL renderer:
- Keep `SDL_QuitSubSystem(SDL_INIT_VIDEO);` unconditional.
- Under `#if SDL_VERSION_ATLEAST(2, 0, 6)`, only call `ImGui_ImplSDL2_Shutdown()` and `ImGui_ImplOpenGL3_Shutdown()` when `!Cmdline_vulkan`.

## Note

If a dedicated Vulkan ImGui backend is added later, revisit this section to ensure its init/shutdown are balanced.



