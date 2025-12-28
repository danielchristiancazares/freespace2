# OpenGL Backend (FSO) - `code/graphics/opengl`

This directory contains the OpenGL renderer backend for FreeSpace Open (FSO). It provides the implementation behind the `gr_*` graphics API using modern OpenGL (Core Profile 3.3+) with GLAD for extension loading.

## File Manifest

| File | Purpose |
|------|---------|
| `gropengl.*` | Backend initialization, function pointer setup, cleanup, capability queries |
| `gropenglstate.*` | OpenGL state caching and management (blend, depth, stencil, textures, arrays) |
| `gropenglshader.*` | Shader compilation, variant management, uniform setup |
| `ShaderProgram.*` | Object-oriented shader program wrapper with uniform management |
| `gropengltexture.*` | Texture cache, mipmap handling, render target creation |
| `gropengltnl.*` | Buffer management, vertex layout binding, model rendering |
| `gropengldraw.*` | Primitive rendering, scene framebuffers, shadow mapping |
| `gropengldeferred.*` | Deferred lighting pass, G-buffer management, light volumes |
| `gropenglpostprocessing.*` | Post-processing effects (bloom, FXAA/SMAA, tonemapping) |
| `gropenglbmpman.*` | Bitmap manager integration (texture upload, render targets) |
| `gropenglsync.*` | GPU synchronization via fence objects |
| `gropenglquery.*` | Occlusion and timer query objects |
| `gropenglopenxr.*` | OpenXR VR integration |
| `SmaaAreaTex.h` | SMAA lookup texture data (area texture) |
| `SmaaSearchTex.h` | SMAA lookup texture data (search texture) |

## Version Requirements

- **Minimum OpenGL**: 3.3 Core Profile
- **Minimum GLSL**: 330
- **Preferred OpenGL**: 4.5 (highest version attempted during context creation)

The backend attempts to create the highest available OpenGL context from versions 4.5 down to 3.3. Context creation uses the Core Profile exclusively; the Compatibility Profile is not supported.

## Architecture Overview

### Initialization Flow

```
gr_opengl_init()
+-- renderdoc::loadApi()                  # Optional debug integration
+-- opengl_init_display_device()          # Create window + GL context
|   +-- gr_opengl_create_viewport()       # Configure pixel format
|   +-- createOpenGLContext()             # Try 4.5 -> 4.4 -> ... -> 3.3
+-- gladLoadGLLoader()                    # Load function pointers
+-- init_extensions()                     # Query extension support
+-- GL_state.init()                       # Initialize state tracker
+-- opengl_tcache_init()                  # Texture cache
+-- opengl_tnl_init()                     # Buffer/vertex systems
+-- opengl_shader_init()                  # Compile default shaders
+-- opengl_setup_scene_textures()         # Allocate scene FBOs
+-- opengl_post_process_init()            # Post-processing setup
```

### Shutdown Flow

```
gr_opengl_cleanup() / gr_opengl_shutdown()
+-- opengl_tcache_shutdown()
+-- opengl_tnl_shutdown()
+-- opengl_scene_texture_shutdown()
+-- opengl_post_process_shutdown()
+-- opengl_shader_shutdown()
+-- glDeleteVertexArrays()                # Global VAO
+-- GL_context = nullptr                  # Release context
```

## Key Subsystems

### State Management (`gropenglstate.*`)

The `opengl_state` class (`GL_state` global) provides a state-caching layer to minimize redundant OpenGL calls. All state changes should go through `GL_state` rather than calling `gl*` functions directly.

**Components:**
- `opengl_texture_state` - Texture unit binding and target management
- `opengl_array_state` - VAO, VBO, EBO, UBO binding and vertex attribute state
- `opengl_constant_state` - Cached device limits (alignment, max sizes)

**Key Methods:**
```cpp
GL_state.SetAlphaBlendMode(mode)      // Blend state
GL_state.SetZbufferType(type)         // Depth test configuration
GL_state.Texture.Enable(unit, target, id)  // Bind texture
GL_state.Array.BindArrayBuffer(id)    // Bind VBO
GL_state.BindFrameBuffer(fbo)         // Framebuffer binding with stack
GL_state.UseProgram(program)          // Shader program binding
```

**Framebuffer Stack:**
The state tracker maintains a framebuffer stack via `PushFramebufferState()` / `PopFramebufferState()` for temporary render target switches.

### Shader Management (`gropenglshader.*`, `ShaderProgram.*`)

**Shader Compilation:**
- Shaders are compiled on-demand via `gr_opengl_maybe_create_shader(shader_type, flags)`
- Variants are controlled by preprocessor flags injected at compile time
- Compiled shaders are stored in the global `GL_shader` vector

**ShaderProgram Class:**
Encapsulates a linked GLSL program with:
- `ShaderUniforms` for texture unit assignment
- Vertex attribute initialization with default values
- Stage-based shader code addition (vertex, geometry, fragment)

**Current Shader:**
The global `Current_shader` pointer tracks the active shader. Use `opengl_shader_set_current()` to change shaders.

**Shader Types:**
Defined in `graphics/2d.h` as `shader_type` enum. Each type maps to specific vertex/fragment/geometry shader files and required vertex attributes.

### Texture Handling (`gropengltexture.*`)

**Texture Cache:**
- `tcache_slot_opengl` extends `gr_bitmap_info` with OpenGL-specific data
- Texture slots are managed by bmpman; the OpenGL backend provides upload/deletion hooks

**Key Functions:**
```cpp
gr_opengl_tcache_set()       // Bind texture for rendering
opengl_tcache_init()         // Initialize cache
opengl_tcache_flush()        // Release all textures
opengl_tcache_frame()        // Per-frame maintenance
```

**Render Targets:**
```cpp
opengl_make_render_target()  // Create FBO + texture
opengl_set_render_target()   // Activate render target
opengl_kill_render_target()  // Destroy render target
```

**Texture Targets:**
Supports `GL_TEXTURE_2D`, `GL_TEXTURE_CUBE_MAP`, and `GL_TEXTURE_2D_ARRAY` for batched rendering.

### Buffer Management (`gropengltnl.*`)

**Buffer Creation:**
```cpp
gr_opengl_create_buffer(type, usage)  // Create buffer handle
gr_opengl_update_buffer_data()        // Upload data
gr_opengl_delete_buffer()             // Destroy buffer
```

**Buffer Types:**
- Vertex buffers (`GL_ARRAY_BUFFER`)
- Index buffers (`GL_ELEMENT_ARRAY_BUFFER`)
- Uniform buffers (`GL_UNIFORM_BUFFER`)

**Vertex Layout Binding:**
```cpp
opengl_bind_vertex_layout(layout, vbo, ibo, offset)
```
Configures vertex attributes based on `vertex_layout` specification, mapping FSO vertex formats to OpenGL attribute locations.

**Uniform Buffer Binding:**
```cpp
gr_opengl_bind_uniform_buffer(bind_point, offset, size, buffer)
```
Binds a range of a uniform buffer to a specific binding point (up to `MAX_UNIFORM_BUFFERS = 6`).

### Drawing Operations (`gropengldraw.*`)

**Scene Textures:**
The backend maintains several scene-wide framebuffers for deferred rendering and post-processing:

| Texture | Purpose |
|---------|---------|
| `Scene_framebuffer` | Main scene FBO |
| `Scene_color_texture` | HDR color buffer |
| `Scene_depth_texture` | Depth buffer |
| `Scene_position_texture` | G-buffer: world position |
| `Scene_normal_texture` | G-buffer: normals |
| `Scene_specular_texture` | G-buffer: specular |
| `Scene_emissive_texture` | G-buffer: emissive |
| `Cockpit_depth_texture` | Separate cockpit depth |

**Multisampling:**
`_ms` suffixed textures/framebuffers are used when MSAA is enabled. Scene rendering uses the MSAA buffers, which are resolved before post-processing.

**Primitive Rendering:**
```cpp
opengl_render_primitives(prim_type, layout, n_verts, buffer, offset)
gr_opengl_render_model(material, vert_source, buffer, texi)
```

### Deferred Lighting (`gropengldeferred.*`)

**G-Buffer Pass:**
```cpp
gr_opengl_deferred_lighting_begin()   // Switch to G-buffer target
// ... render geometry ...
gr_opengl_deferred_lighting_end()     // Finalize G-buffer
```

**Light Volumes:**
- Sphere geometry for point lights
- Cylinder geometry for tube lights
- Rendered with stencil masking to avoid over-shading

**MSAA Handling:**
`gr_opengl_deferred_lighting_msaa()` resolves multisampled G-buffer before lighting.

### Post-Processing (`gropenglpostprocessing.*`)

**Pipeline:**
```cpp
gr_opengl_post_process_begin()        // Start accumulation
// ... render scene ...
gr_opengl_post_process_end()          // Apply effects chain
```

**Effects:**
- Bloom (threshold + blur + composite)
- FXAA / SMAA anti-aliasing
- Tonemapping (HDR to LDR)
- Motion blur
- Distortion effects

**SMAA Textures:**
`SmaaAreaTex.h` and `SmaaSearchTex.h` contain precomputed lookup tables for SMAA.

### Synchronization (`gropenglsync.*`)

GPU synchronization uses OpenGL fence sync objects:
```cpp
gr_opengl_sync_fence()                // Insert fence
gr_opengl_sync_wait(sync, timeout)    // Wait for completion
gr_opengl_sync_delete(sync)           // Delete fence
```

### Query Objects (`gropenglquery.*`)

Supports occlusion queries and timer queries:
```cpp
gr_opengl_create_query_object()
gr_opengl_query_value(obj, type)      // Begin query
gr_opengl_query_value_available(obj)  // Check completion
gr_opengl_get_query_value(obj)        // Read result
```

Timer queries require `GL_ARB_timer_query` extension.

### OpenXR / VR Support (`gropenglopenxr.*`)

Provides OpenGL-specific OpenXR integration:
- Extension enumeration for OpenXR instance creation
- Session creation with OpenGL graphics binding
- Swapchain format selection
- Swapchain buffer acquisition and presentation

## Integration with Graphics Abstraction

The backend connects to the engine via the function pointer table in `gr_screen`. Initialization is performed in `gr_opengl_init_function_pointers()`, which assigns all `gf_*` function pointers.

**Key Integration Points:**
- `gf_flip` / `gf_setup_frame` - Frame lifecycle
- `gf_render_model` / `gf_render_primitives` - Drawing
- `gf_bm_*` - Bitmap/texture management hooks
- `gf_deferred_lighting_*` - Deferred pipeline control
- `gf_post_process_*` - Post-processing pipeline control

## Extension Dependencies

**Required:**
- Shader support (core in 3.3+)
- Framebuffer objects (core in 3.0+)
- Vertex array objects (core in 3.0+)

**Optional (capability-gated):**
| Extension | Capability | Purpose |
|-----------|------------|---------|
| `GL_ARB_timer_query` | `CAPABILITY_TIMESTAMP_QUERY` | GPU timing |
| `GL_ARB_draw_buffers_blend` | `CAPABILITY_SEPARATE_BLEND_FUNCTIONS` | Per-buffer blend |
| `GL_ARB_buffer_storage` | `CAPABILITY_PERSISTENT_BUFFER_MAPPING` | Persistent mapping |
| `GL_ARB_texture_compression_bptc` | `CAPABILITY_BPTC` | BC7 texture compression |
| `GL_ARB_vertex_attrib_binding` | `CAPABILITY_INSTANCED_RENDERING` | Instanced draws |
| `GL_EXT_texture_compression_s3tc` | - | S3TC/DXT compression |
| `GL_EXT_texture_filter_anisotropic` | - | Anisotropic filtering |
| `GL_KHR_debug` | - | Debug labels/groups |

## Global State

| Global | Type | Purpose |
|--------|------|---------|
| `GL_state` | `opengl_state` | State cache |
| `GL_shader` | `SCP_vector<opengl_shader_t>` | Compiled shaders |
| `Current_shader` | `opengl_shader_t*` | Active shader |
| `GL_vao` | `GLuint` | Global VAO (Core Profile requirement) |
| `GL_version` | `int` | GL version (e.g., 45 = 4.5) |
| `GLSL_version` | `int` | GLSL version (e.g., 450) |
| `GL_initted` | `bool` | Initialization flag |

## Common Patterns

### State Caching
Always use `GL_state` for state changes:
```cpp
// Correct
GL_state.SetAlphaBlendMode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
GL_state.DepthTest(GL_TRUE);

// Avoid direct calls
// glEnable(GL_DEPTH_TEST);  // Bypasses cache
```

### Framebuffer Switching
Use the framebuffer stack for temporary switches:
```cpp
GL_state.PushFramebufferState();
GL_state.BindFrameBuffer(target_fbo);
// ... render to target ...
GL_state.PopFramebufferState();
```

### Shader Usage
```cpp
int shader_handle = gr_opengl_maybe_create_shader(SDR_TYPE_DEFAULT, flags);
opengl_shader_set_current(shader_handle);
// ... set uniforms ...
// ... draw ...
```

## Debug Support

**Debug Output:**
When built with `NDEBUG` undefined, the backend enables `GL_ARB_debug_output` for driver messages. Messages are routed to the engine's logging system.

**Object Labels:**
`opengl_set_object_label()` assigns human-readable names to GL objects when `GL_KHR_debug` is available.

**Debug Groups:**
`gr_opengl_push_debug_group()` / `gr_opengl_pop_debug_group()` create hierarchical debug regions visible in tools like RenderDoc.

**RenderDoc Integration:**
The backend loads the RenderDoc API at startup if available, enabling frame capture from within the application.

## Build Configuration

The OpenGL backend is built when `FSO_BUILD_WITH_OPENGL` is enabled (default). Key compile-time options:

| Define | Effect |
|--------|--------|
| `NDEBUG` | Disables debug output, error checking |
| `FS_OPENGL_DEBUG` | Forces debug features in release builds |

## Differences from Vulkan Backend

| Aspect | OpenGL | Vulkan |
|--------|--------|--------|
| State Management | Cached global state | Explicit command buffer recording |
| Synchronization | Implicit (driver-managed) | Explicit fences/semaphores |
| Resource Binding | Bind points (texture units, UBO slots) | Descriptor sets |
| Render Passes | Implicit via FBO binding | Explicit dynamic rendering |
| Pipeline State | Mutable global state | Immutable pipeline objects |
| Threading | Single-threaded | Single-threaded (FSO design) |

The OpenGL backend uses traditional immediate-mode patterns with state caching, while the Vulkan backend uses explicit command recording with capability tokens.
