#pragma once

#include <cstdint>
#include <limits>

// Shared movie-related types used by cutscene playback and graphics backends.

// Matches FFmpeg AVColorSpace/AVColorRange at a high level.
enum class MovieColorSpace : uint8_t { BT601, BT709 };
enum class MovieColorRange : uint8_t { Narrow, Full }; // Narrow = 16-235, Full = 0-255

// Opaque handle returned by the renderer backend for movie textures.
enum class MovieTextureHandle : uint32_t { Invalid = std::numeric_limits<uint32_t>::max() };

inline bool gr_is_valid(MovieTextureHandle h) { return h != MovieTextureHandle::Invalid; }

