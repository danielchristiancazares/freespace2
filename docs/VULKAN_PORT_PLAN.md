# FreeSpace 2 Vulkan Rendering Pipeline Port - Comprehensive Plan

## Executive Summary

This document outlines a detailed multi-phased plan for porting the FreeSpace 2 rendering pipeline from OpenGL to Vulkan. The existing codebase provides a solid foundation with:

- **Function pointer-based graphics abstraction** (~80+ operations in `screen` struct)
- **Basic Vulkan scaffolding** already in place (instance, device, swapchain, frame sync)
- **SPIR-V shader support** available
- **Mature OpenGL implementation** as reference (~4,800 LOC)

The port is organized into **7 phases** with clear milestones and dependencies.

---

## Table of Contents

1. [Current State Analysis](#current-state-analysis)
2. [Phase 1: Core Infrastructure](#phase-1-core-infrastructure)
3. [Phase 2: Resource Management](#phase-2-resource-management)
4. [Phase 3: Basic Rendering Pipeline](#phase-3-basic-rendering-pipeline)
5. [Phase 4: Advanced Rendering Features](#phase-4-advanced-rendering-features)
6. [Phase 5: Post-Processing Pipeline](#phase-5-post-processing-pipeline)
7. [Phase 6: Specialized Renderers](#phase-6-specialized-renderers)
8. [Phase 7: Optimization and Polish](#phase-7-optimization-and-polish)
9. [Risk Assessment and Mitigation](#risk-assessment-and-mitigation)
10. [Testing Strategy](#testing-strategy)

---

## Current State Analysis

### What Exists in Vulkan Backend

**Files:**
- `code/graphics/vulkan/VulkanRenderer.cpp` (908 lines) - Basic renderer
- `code/graphics/vulkan/VulkanRenderer.h` - Class definition
- `code/graphics/vulkan/RenderFrame.cpp/h` - Frame synchronization
- `code/graphics/vulkan/gr_vulkan.cpp/h` - Initialization entry point
- `code/graphics/vulkan/vulkan_stubs.cpp` (396 lines) - Stub implementations

**Implemented:**
- Vulkan instance creation with debug layers
- Physical device selection and scoring
- Logical device creation with queue families (graphics, transfer, present)
- Swapchain creation with format/present mode selection
- Basic render pass (single color attachment)
- Minimal graphics pipeline (hardcoded test triangle)
- Frame-in-flight synchronization (semaphores, fences)
- Command pool and buffer management
- Basic flip/present loop

**Not Implemented (70+ functions as stubs):**
- Buffer creation and management
- Texture loading and caching
- Framebuffer/render target system
- All material-specific renderers
- Shader compilation and management
- Uniform buffer binding
- State management (depth, stencil, blend)
- Post-processing pipeline
- Deferred lighting system
- Shadow mapping
- Query objects

### Key OpenGL References

| Component | OpenGL File | Lines | Purpose |
|-----------|-------------|-------|---------|
| Main Device | `gropengl.cpp` | 1,610 | Device init, buffers |
| Draw Calls | `gropengldraw.cpp` | 1,203 | Rendering, FBOs |
| State Cache | `gropenglstate.cpp` | 908 | State management |
| Shaders | `gropenglshader.cpp` | 1,152 | Shader compilation |
| Textures | `gropengltexture.cpp` | 1,983 | Texture cache |
| Deferred | `gropengldeferred.cpp` | ~900 | G-buffer, lighting |
| Post-Processing | `gropenglpostprocessing.cpp` | 1,215 | Effects pipeline |
| Transform | `gropengltnl.cpp` | 1,268 | Vertex handling |

---

## Phase 1: Core Infrastructure

**Objective:** Establish foundational Vulkan systems required by all subsequent phases.

### 1.1 Memory Management System

**Files to Create:**
- `code/graphics/vulkan/VulkanMemory.h/cpp`

**Implementation:**

```cpp
class VulkanMemoryAllocator {
    // Use VMA (Vulkan Memory Allocator) library
    VmaAllocator m_allocator;

public:
    struct AllocationInfo {
        VkBuffer buffer;
        VmaAllocation allocation;
        VmaAllocationInfo info;
    };

    AllocationInfo allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                  VmaMemoryUsage memUsage);
    AllocationInfo allocateImage(const VkImageCreateInfo& info,
                                 VmaMemoryUsage memUsage);
    void free(VmaAllocation allocation);
    void* map(VmaAllocation allocation);
    void unmap(VmaAllocation allocation);
    void flush(VmaAllocation allocation, size_t offset, size_t size);
};
```

**Tasks:**
1. Integrate VMA library (header-only, well-tested)
2. Create allocator initialization in `VulkanRenderer::initialize()`
3. Implement allocation tracking for debugging
4. Add memory budget querying
5. Create staging buffer pool for transfers

**Reference:** OpenGL uses `gr_heap_allocate()` in `code/graphics/util/GPUMemoryHeap.cpp`

### 1.2 Command Buffer Management

**Files to Create:**
- `code/graphics/vulkan/CommandBufferPool.h/cpp`

**Implementation:**

```cpp
class CommandBufferPool {
    vk::Device m_device;
    vk::CommandPool m_pool;
    SCP_vector<vk::CommandBuffer> m_availableBuffers;
    SCP_vector<vk::CommandBuffer> m_inFlightBuffers;

public:
    vk::CommandBuffer acquire();
    void release(vk::CommandBuffer buffer);
    void reset(); // Called per-frame

    // Immediate execution helper
    void executeImmediate(std::function<void(vk::CommandBuffer)> func);
};
```

**Tasks:**
1. Create separate pools for graphics and transfer queues
2. Implement command buffer recycling
3. Add immediate execution for one-shot uploads
4. Thread-safe acquisition for multi-threaded recording

### 1.3 Synchronization Primitives

**Files to Modify:**
- `code/graphics/vulkan/RenderFrame.cpp`

**Implementation:**

```cpp
struct FrameResources {
    vk::UniqueSemaphore imageAvailable;
    vk::UniqueSemaphore renderFinished;
    vk::UniqueFence inFlightFence;

    vk::UniqueCommandBuffer primaryCmdBuffer;
    SCP_vector<vk::UniqueCommandBuffer> secondaryCmdBuffers;

    // Per-frame uniform buffer allocations
    struct UniformAllocation {
        vk::Buffer buffer;
        size_t offset;
        size_t size;
    };
    SCP_vector<UniformAllocation> uniformAllocations;
};
```

**Tasks:**
1. Expand `RenderFrame` to manage per-frame resources
2. Implement resource retirement (wait for fence before reuse)
3. Add timeline semaphores for advanced synchronization
4. Create barrier helpers for image layout transitions

### 1.4 Descriptor Set Management

**Files to Create:**
- `code/graphics/vulkan/DescriptorManager.h/cpp`

**Implementation:**

```cpp
class DescriptorSetAllocator {
    vk::Device m_device;
    SCP_vector<vk::UniqueDescriptorPool> m_pools;

    // Layout cache
    SCP_unordered_map<size_t, vk::UniqueDescriptorSetLayout> m_layoutCache;

public:
    vk::DescriptorSetLayout getLayout(const SCP_vector<vk::DescriptorSetLayoutBinding>& bindings);
    vk::DescriptorSet allocate(vk::DescriptorSetLayout layout);
    void reset(); // Per-frame reset
};

class DescriptorWriter {
    vk::DescriptorSet m_set;
    SCP_vector<vk::WriteDescriptorSet> m_writes;

public:
    DescriptorWriter& bindBuffer(uint32_t binding, vk::Buffer buffer,
                                 size_t offset, size_t range);
    DescriptorWriter& bindImage(uint32_t binding, vk::ImageView view,
                                vk::Sampler sampler);
    void update(vk::Device device);
};
```

**Tasks:**
1. Create descriptor pool with automatic expansion
2. Implement layout caching by hash
3. Add bindless texture support preparation
4. Per-frame descriptor set allocation with reset

### 1.5 Pipeline Cache System

**Files to Create:**
- `code/graphics/vulkan/PipelineCache.h/cpp`

**Implementation:**

```cpp
struct PipelineKey {
    shader_type shaderType;
    uint32_t shaderFlags;
    vk::RenderPass renderPass;
    uint32_t subpass;

    // Dynamic state
    bool dynamicViewport;
    bool dynamicScissor;

    // Vertex input
    size_t vertexLayoutHash;

    // Fixed-function state
    gr_alpha_blend blendMode;
    gr_zbuffer_type depthMode;
    int stencilMode;
    int cullMode;

    size_t hash() const;
    bool operator==(const PipelineKey& other) const;
};

class PipelineManager {
    vk::Device m_device;
    vk::UniquePipelineCache m_cache;
    SCP_unordered_map<PipelineKey, vk::UniquePipeline> m_pipelines;

public:
    vk::Pipeline getOrCreate(const PipelineKey& key,
                             vk::PipelineLayout layout,
                             const SCP_vector<vk::PipelineShaderStageCreateInfo>& stages);
    void saveCache(const SCP_string& filename);
    void loadCache(const SCP_string& filename);
};
```

**Tasks:**
1. Implement pipeline key hashing
2. Create pipeline cache serialization
3. Add background pipeline compilation
4. Implement pipeline derivatives for faster creation

### 1.6 Implement gr_screen Function Pointers - Core

**Functions to Implement:**

```cpp
// In gr_vulkan.cpp or new vulkan_functions.cpp

// Frame management
void vk_flip();           // Present frame
void vk_setup_frame();    // Begin frame, reset state

// Clipping
void vk_set_clip(int x, int y, int w, int h, int resize_mode);
void vk_reset_clip();

// Clear operations
void vk_clear();
void vk_set_clear_color(int r, int g, int b);

// Z-buffer
int vk_zbuffer_get();
int vk_zbuffer_set(int mode);
void vk_zbuffer_clear(int use_zbuffer);

// Stencil
int vk_stencil_set(int mode);
void vk_stencil_clear();
```

**Reference:** `code/graphics/opengl/gropengl.cpp:975-1117` for function pointer assignments

---

## Phase 2: Resource Management

**Objective:** Implement buffer and texture management systems.

### 2.1 Buffer Management

**Files to Create:**
- `code/graphics/vulkan/VulkanBuffer.h/cpp`

**Implementation:**

```cpp
struct VulkanBufferInfo {
    vk::Buffer buffer;
    VmaAllocation allocation;
    size_t size;
    BufferType type;
    BufferUsageHint usage;
    void* mappedPtr;  // For persistent mapping
};

class BufferManager {
    SCP_vector<VulkanBufferInfo> m_buffers;
    SCP_vector<size_t> m_freeList;

public:
    gr_buffer_handle create(BufferType type, BufferUsageHint usage);
    void destroy(gr_buffer_handle handle);
    void update(gr_buffer_handle handle, size_t size, const void* data);
    void updateOffset(gr_buffer_handle handle, size_t offset,
                      size_t size, const void* data);
    void* map(gr_buffer_handle handle);
    void flush(gr_buffer_handle handle, size_t offset, size_t size);

    vk::Buffer getVkBuffer(gr_buffer_handle handle);
};
```

**Tasks:**
1. Implement `gf_create_buffer` - Create Vulkan buffer with appropriate usage flags
2. Implement `gf_delete_buffer` - Destroy buffer and free memory
3. Implement `gf_update_buffer_data` - Staging buffer copy or direct map
4. Implement `gf_update_buffer_data_offset` - Partial updates
5. Implement `gf_map_buffer` - Persistent mapping for streaming
6. Implement `gf_flush_mapped_buffer` - Flush mapped range

**Buffer Type Mapping:**
```cpp
VkBufferUsageFlags getUsageFlags(BufferType type, BufferUsageHint hint) {
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    switch (type) {
    case BufferType::Vertex:
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    case BufferType::Index:
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
    case BufferType::Uniform:
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        break;
    }

    return flags;
}

VmaMemoryUsage getMemoryUsage(BufferUsageHint hint) {
    switch (hint) {
    case BufferUsageHint::Static:
        return VMA_MEMORY_USAGE_GPU_ONLY;
    case BufferUsageHint::Dynamic:
        return VMA_MEMORY_USAGE_CPU_TO_GPU;
    case BufferUsageHint::Streaming:
    case BufferUsageHint::PersistentMapping:
        return VMA_MEMORY_USAGE_CPU_TO_GPU;
    }
}
```

**Reference:** `code/graphics/opengl/gropengl.cpp:319-467` for OpenGL buffer implementation

### 2.2 Texture Management

**Files to Create:**
- `code/graphics/vulkan/VulkanTexture.h/cpp`

**Implementation:**

```cpp
struct VulkanTextureSlot {
    vk::Image image;
    vk::ImageView view;
    VmaAllocation allocation;
    vk::Sampler sampler;

    int width, height;
    int arrayLayers;
    int mipLevels;
    vk::Format format;
    vk::ImageLayout currentLayout;

    int bitmapHandle;
    int textureType;  // TCACHE_TYPE_*
    bool isRenderTarget;
};

class TextureManager {
    SCP_vector<VulkanTextureSlot> m_textures;
    SCP_unordered_map<int, size_t> m_bitmapToTexture;

    // Sampler cache
    SCP_unordered_map<size_t, vk::UniqueSampler> m_samplers;

public:
    bool loadTexture(int bitmapHandle, int textureType);
    void freeTexture(int bitmapHandle);
    vk::ImageView getView(int bitmapHandle);
    vk::Sampler getSampler(int bitmapHandle, int addressMode);

    // Render targets
    int createRenderTarget(int width, int height, int bpp, int flags);
    bool setRenderTarget(int handle, int face);
};
```

**Texture Format Mapping:**
```cpp
vk::Format mapTextureFormat(int bpp, int flags, bool compressed) {
    if (compressed) {
        if (flags & DDS_DXT1) return vk::Format::eBc1RgbUnormBlock;
        if (flags & DDS_DXT3) return vk::Format::eBc2UnormBlock;
        if (flags & DDS_DXT5) return vk::Format::eBc3UnormBlock;
        if (flags & DDS_BC7)  return vk::Format::eBc7UnormBlock;
    }

    switch (bpp) {
    case 32: return vk::Format::eR8G8B8A8Unorm;
    case 24: return vk::Format::eR8G8B8Unorm;
    case 16: return vk::Format::eR5G5B5A1UnormPack16;
    case 8:  return vk::Format::eR8Unorm;
    default: return vk::Format::eR8G8B8A8Unorm;
    }
}
```

**Tasks:**
1. Implement `gf_bm_create` - Create texture from bitmap data
2. Implement `gf_bm_free_data` - Free texture resources
3. Implement `gf_bm_init` - Initialize texture slot
4. Implement `gf_bm_data` - Upload texture data
5. Implement `gf_bm_page_in_start` - Batch texture uploads
6. Implement `gf_preload` - Preload texture to GPU
7. Implement `gf_update_texture` - Update existing texture
8. Implement `gf_get_bitmap_from_texture` - Read back texture data
9. Implement `gf_set_texture_addressing` - Set wrap mode

**Reference:** `code/graphics/opengl/gropengltexture.cpp` for complete texture cache

### 2.3 Framebuffer/Render Target System

**Files to Create:**
- `code/graphics/vulkan/VulkanFramebuffer.h/cpp`

**Implementation:**

```cpp
struct VulkanRenderTarget {
    vk::Image colorImage;
    vk::ImageView colorView;
    VmaAllocation colorAlloc;

    vk::Image depthImage;
    vk::ImageView depthView;
    VmaAllocation depthAlloc;

    vk::UniqueFramebuffer framebuffer;
    vk::UniqueRenderPass renderPass;

    int width, height;
    int bitmapHandle;
    bool isCubemap;
    int currentFace;
};

class FramebufferManager {
    // Scene rendering framebuffers
    struct SceneFramebuffers {
        VulkanRenderTarget main;
        VulkanRenderTarget mainMS;  // MSAA version

        // G-buffer attachments
        vk::Image positionTex;
        vk::Image normalTex;
        vk::Image specularTex;
        vk::Image emissiveTex;

        vk::UniqueRenderPass deferredPass;
        vk::UniqueFramebuffer deferredFB;
    } m_scene;

    // Custom render targets
    SCP_vector<VulkanRenderTarget> m_renderTargets;

public:
    void createSceneFramebuffers(int width, int height, int msaaSamples);
    void resizeSceneFramebuffers(int width, int height);

    int createRenderTarget(int width, int height, int bpp, int mmLvl, int flags);
    bool setRenderTarget(int handle, int face);

    vk::RenderPass getSceneRenderPass();
    vk::Framebuffer getSceneFramebuffer();
};
```

**Tasks:**
1. Implement `gf_bm_make_render_target` - Create render target texture
2. Implement `gf_bm_set_render_target` - Bind render target
3. Create scene framebuffers (forward rendering)
4. Create G-buffer framebuffers (deferred rendering)
5. Implement MSAA framebuffers
6. Handle cubemap render targets

**Reference:** `code/graphics/opengl/gropengldraw.cpp:96-400` for scene texture setup

### 2.4 Uniform Buffer Integration

**Files to Create:**
- `code/graphics/vulkan/VulkanUniformBuffer.h/cpp`

**Implementation:**

```cpp
class VulkanUniformBufferManager {
    struct FrameBuffer {
        vk::Buffer buffer;
        VmaAllocation allocation;
        void* mappedPtr;
        size_t currentOffset;
        size_t size;
    };

    std::array<FrameBuffer, MAX_FRAMES_IN_FLIGHT> m_frameBuffers;
    uint32_t m_currentFrame;
    size_t m_alignment;  // From device limits

public:
    struct Allocation {
        vk::Buffer buffer;
        size_t offset;
        size_t size;
    };

    Allocation allocate(size_t size);
    void* getMappedPtr(const Allocation& alloc);
    void beginFrame(uint32_t frameIndex);
    void endFrame();
};
```

**Tasks:**
1. Query `minUniformBufferOffsetAlignment` from device
2. Implement per-frame uniform buffer allocation
3. Implement `gf_bind_uniform_buffer` function
4. Create descriptor set updates for uniform buffers

**Uniform Block Types (from `uniform_structs.h`):**
- `Lights` - Light data array
- `ModelData` - Per-model matrices and material properties
- `NanoVGData` - Vector graphics data
- `DecalInfo` - Decal instance data
- `DeferredGlobals` - Shadow matrices, cascade data
- `Matrices` - View/projection matrices
- `MovieData` - Video playback data
- `GenericData` - Post-processing effects

---

## Phase 3: Basic Rendering Pipeline

**Objective:** Implement core rendering functionality to display 3D content.

### 3.1 Shader System

**Files to Create:**
- `code/graphics/vulkan/VulkanShader.h/cpp`

**Implementation:**

```cpp
struct VulkanShaderModule {
    vk::UniqueShaderModule module;
    vk::ShaderStageFlagBits stage;
    SCP_string entryPoint;
};

struct VulkanShaderProgram {
    shader_type type;
    uint32_t flags;

    SCP_vector<VulkanShaderModule> stages;

    // Reflection data
    SCP_vector<vk::DescriptorSetLayoutBinding> bindings;
    SCP_vector<vk::PushConstantRange> pushConstants;
    vk::UniqueDescriptorSetLayout descriptorLayout;
    vk::UniquePipelineLayout pipelineLayout;
};

class ShaderManager {
    SCP_unordered_map<shader_descriptor_t, VulkanShaderProgram> m_programs;

public:
    int maybeCreateShader(shader_type type, uint32_t flags);
    VulkanShaderProgram* getProgram(int handle);
    vk::PipelineLayout getPipelineLayout(int handle);

    // Load SPIR-V from embedded resources
    vk::UniqueShaderModule loadSPIRV(const SCP_string& name);
};
```

**SPIR-V Shader Files:**
Already exist in `code/graphics/shaders/compiled/`:
- `default-material.vert.spv`, `default-material.frag.spv`
- Model shaders need compilation from `.sdr` sources

**Tasks:**
1. Implement `gf_maybe_create_shader` - Load/compile SPIR-V shaders
2. Create shader reflection for automatic descriptor layout
3. Implement shader variant system (flags → defines)
4. Port shader preprocessing from `gropenglshader.cpp`
5. Create push constant support for frequently changing uniforms

**Reference:** `code/graphics/opengl/gropenglshader.cpp` for shader compilation

### 3.2 Vertex Input System

**Files to Create:**
- `code/graphics/vulkan/VulkanVertexInput.h/cpp`

**Implementation:**

```cpp
struct VulkanVertexBinding {
    uint32_t binding;
    uint32_t stride;
    vk::VertexInputRate inputRate;
};

struct VulkanVertexAttribute {
    uint32_t location;
    uint32_t binding;
    vk::Format format;
    uint32_t offset;
};

class VertexInputManager {
    SCP_unordered_map<size_t, std::pair<
        SCP_vector<VulkanVertexBinding>,
        SCP_vector<VulkanVertexAttribute>>> m_cache;

public:
    std::pair<const SCP_vector<VulkanVertexBinding>&,
              const SCP_vector<VulkanVertexAttribute>&>
    getInputState(const vertex_layout& layout);
};
```

**Vertex Format Mapping:**
```cpp
vk::Format mapVertexFormat(vertex_format_data::vertex_format fmt) {
    switch (fmt) {
    case POSITION4: return vk::Format::eR32G32B32A32Sfloat;
    case POSITION3: return vk::Format::eR32G32B32Sfloat;
    case POSITION2: return vk::Format::eR32G32Sfloat;
    case COLOR4:    return vk::Format::eR8G8B8A8Unorm;
    case COLOR4F:   return vk::Format::eR32G32B32A32Sfloat;
    case TEX_COORD2: return vk::Format::eR32G32Sfloat;
    case TEX_COORD4: return vk::Format::eR32G32B32A32Sfloat;
    case NORMAL:    return vk::Format::eR32G32B32Sfloat;
    case TANGENT:   return vk::Format::eR32G32B32A32Sfloat;
    case MODEL_ID:  return vk::Format::eR32Sfloat;
    case RADIUS:    return vk::Format::eR32Sfloat;
    case UVEC:      return vk::Format::eR32G32B32Sfloat;
    default:        return vk::Format::eUndefined;
    }
}
```

**Tasks:**
1. Create vertex layout to Vulkan input state conversion
2. Cache converted layouts by hash
3. Support instanced rendering with divisors

### 3.3 Render Pass Management

**Files to Create:**
- `code/graphics/vulkan/RenderPassManager.h/cpp`

**Implementation:**

```cpp
enum class RenderPassType {
    Forward,          // Standard forward rendering
    DeferredGBuffer,  // G-buffer fill pass
    DeferredLighting, // Light accumulation
    PostProcess,      // Post-processing effects
    Shadow,           // Shadow map rendering
    UI                // User interface overlay
};

struct RenderPassInfo {
    vk::UniqueRenderPass pass;
    SCP_vector<vk::ClearValue> clearValues;
    vk::Extent2D extent;
};

class RenderPassManager {
    SCP_unordered_map<RenderPassType, RenderPassInfo> m_passes;

public:
    vk::RenderPass getPass(RenderPassType type);
    void beginPass(vk::CommandBuffer cmd, RenderPassType type,
                   vk::Framebuffer fb);
    void endPass(vk::CommandBuffer cmd);
};
```

**Tasks:**
1. Create forward rendering pass (color + depth)
2. Create deferred G-buffer pass (6 attachments)
3. Create post-processing passes
4. Create shadow map pass

### 3.4 Basic Primitive Rendering

**Files to Create:**
- `code/graphics/vulkan/VulkanDraw.h/cpp`

**Implementation:**

```cpp
class VulkanDrawContext {
    vk::CommandBuffer m_cmd;
    vk::Pipeline m_currentPipeline;
    vk::PipelineLayout m_currentLayout;

    // Current state
    vk::Buffer m_boundVertexBuffer;
    vk::Buffer m_boundIndexBuffer;

public:
    void bindPipeline(vk::Pipeline pipeline, vk::PipelineLayout layout);
    void bindVertexBuffer(vk::Buffer buffer, size_t offset);
    void bindIndexBuffer(vk::Buffer buffer, size_t offset, vk::IndexType type);
    void bindDescriptorSet(uint32_t set, vk::DescriptorSet ds);
    void pushConstants(vk::ShaderStageFlags stages, uint32_t offset,
                       uint32_t size, const void* data);

    void draw(uint32_t vertexCount, uint32_t instanceCount,
              uint32_t firstVertex, uint32_t firstInstance);
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                     uint32_t firstIndex, int32_t vertexOffset,
                     uint32_t firstInstance);
};
```

**Tasks:**
1. Implement `gf_render_primitives`:
   ```cpp
   void vk_render_primitives(material* mat, primitive_type prim,
                             vertex_layout* layout, int offset,
                             int n_verts, gr_buffer_handle buffer,
                             size_t buffer_offset);
   ```
2. Map primitive types:
   ```cpp
   vk::PrimitiveTopology mapPrimitiveType(primitive_type type) {
       switch (type) {
       case PRIM_TYPE_POINTS:    return vk::PrimitiveTopology::ePointList;
       case PRIM_TYPE_LINES:     return vk::PrimitiveTopology::eLineList;
       case PRIM_TYPE_LINESTRIP: return vk::PrimitiveTopology::eLineStrip;
       case PRIM_TYPE_TRIS:      return vk::PrimitiveTopology::eTriangleList;
       case PRIM_TYPE_TRISTRIP:  return vk::PrimitiveTopology::eTriangleStrip;
       case PRIM_TYPE_TRIFAN:    return vk::PrimitiveTopology::eTriangleFan;
       }
   }
   ```

**Reference:** `code/graphics/opengl/gropengltnl.cpp:456-603` for draw call handling

### 3.5 State Management

**Files to Create:**
- `code/graphics/vulkan/VulkanState.h/cpp`

**Implementation:**

```cpp
struct VulkanRenderState {
    // Depth state
    bool depthTestEnabled;
    bool depthWriteEnabled;
    vk::CompareOp depthCompareOp;

    // Stencil state
    bool stencilEnabled;
    vk::StencilOpState stencilFront;
    vk::StencilOpState stencilBack;

    // Blend state
    bool blendEnabled;
    vk::BlendFactor srcColorBlend;
    vk::BlendFactor dstColorBlend;
    vk::BlendOp colorBlendOp;
    vk::BlendFactor srcAlphaBlend;
    vk::BlendFactor dstAlphaBlend;
    vk::BlendOp alphaBlendOp;

    // Rasterizer state
    vk::CullModeFlags cullMode;
    vk::FrontFace frontFace;
    vk::PolygonMode polygonMode;
    float lineWidth;

    // Dynamic state
    vk::Rect2D scissor;
    vk::Viewport viewport;

    size_t hash() const;
};

class StateManager {
    VulkanRenderState m_currentState;
    VulkanRenderState m_pendingState;

public:
    void setDepthMode(gr_zbuffer_type mode);
    void setStencilMode(int mode);
    void setBlendMode(gr_alpha_blend mode);
    void setCullMode(int mode);
    void setScissor(int x, int y, int w, int h);
    void setViewport(int x, int y, int w, int h);

    // Returns true if pipeline needs recreation
    bool applyState(PipelineManager& pipelines);
};
```

**Blend Mode Mapping:**
```cpp
void mapBlendMode(gr_alpha_blend mode, vk::BlendFactor& src, vk::BlendFactor& dst) {
    switch (mode) {
    case ALPHA_BLEND_NONE:
        src = vk::BlendFactor::eOne;
        dst = vk::BlendFactor::eZero;
        break;
    case ALPHA_BLEND_ADDITIVE:
        src = vk::BlendFactor::eOne;
        dst = vk::BlendFactor::eOne;
        break;
    case ALPHA_BLEND_ALPHA_ADDITIVE:
        src = vk::BlendFactor::eSrcAlpha;
        dst = vk::BlendFactor::eOne;
        break;
    case ALPHA_BLEND_ALPHA_BLEND_ALPHA:
        src = vk::BlendFactor::eSrcAlpha;
        dst = vk::BlendFactor::eOneMinusSrcAlpha;
        break;
    case ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR:
        src = vk::BlendFactor::eSrcAlpha;
        dst = vk::BlendFactor::eOneMinusSrcColor;
        break;
    case ALPHA_BLEND_PREMULTIPLIED:
        src = vk::BlendFactor::eOne;
        dst = vk::BlendFactor::eOneMinusSrcAlpha;
        break;
    }
}
```

**Tasks:**
1. Implement depth state tracking
2. Implement stencil state tracking
3. Implement blend state tracking
4. Implement dynamic state (viewport, scissor)
5. Create pipeline key generation from state

---

## Phase 4: Advanced Rendering Features

**Objective:** Implement complex rendering systems (deferred, shadows, etc.)

### 4.1 Model Rendering

**Files to Create:**
- `code/graphics/vulkan/VulkanModelRenderer.h/cpp`

**Implementation:**

```cpp
void vk_render_model(model_material* material_info,
                     indexed_vertex_source* vert_source,
                     vertex_buffer* bufferp,
                     size_t texi)
{
    buffer_data* datap = &bufferp->tex_buf[texi];

    // 1. Select shader based on material flags
    int shader = maybeCreateModelShader(material_info);

    // 2. Setup uniform data
    model_uniform_data uniforms;
    convert_model_material(material_info, bufferp, datap, uniforms);

    // 3. Bind textures (base, glow, normal, specular, etc.)
    bindModelTextures(material_info, datap);

    // 4. Bind vertex/index buffers
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuffer, &vert_source->Vertex_offset);
    vkCmdBindIndexBuffer(cmd, ibuffer, vert_source->Index_offset, indexType);

    // 5. Draw
    vkCmdDrawIndexed(cmd, datap->n_verts, 1, 0,
                     vert_source->Base_vertex_offset, 0);
}
```

**Tasks:**
1. Implement `gf_render_model` function
2. Port model shader selection logic
3. Implement texture binding for all model map types
4. Handle submodel transforms
5. Support batched submodel rendering

**Reference:** `code/graphics/opengl/gropengltnl.cpp:605-644` for model rendering

### 4.2 Deferred Lighting System

**Files to Create:**
- `code/graphics/vulkan/VulkanDeferred.h/cpp`

**Implementation:**

```cpp
class DeferredRenderer {
    // G-buffer attachments
    struct GBuffer {
        vk::Image color;      // Albedo + ambient occlusion
        vk::Image position;   // World position
        vk::Image normal;     // Surface normals
        vk::Image specular;   // Specular + roughness
        vk::Image emissive;   // Self-illumination
        vk::Image depth;      // Depth buffer
    } m_gbuffer;

    vk::UniqueRenderPass m_gbufferPass;
    vk::UniqueRenderPass m_lightingPass;
    vk::UniqueFramebuffer m_gbufferFB;
    vk::UniqueFramebuffer m_lightingFB;

public:
    void begin(bool clearNonColorBufs);
    void end();
    void msaaResolve();
    void finish();

    void renderLightVolumes(const SCP_vector<light>& lights);
};
```

**G-Buffer Layout:**
| Attachment | Format | Content |
|------------|--------|---------|
| 0 | RGBA16F | Albedo RGB + AO |
| 1 | RGBA16F | World Position XYZ |
| 2 | RGBA16F | Normal XYZ + Roughness |
| 3 | RGBA8 | Specular RGB + Metallic |
| 4 | RGBA16F | Emissive RGB + Intensity |
| Depth | D24S8 | Depth + Stencil |

**Tasks:**
1. Implement `gf_deferred_lighting_begin` - Begin G-buffer pass
2. Implement `gf_deferred_lighting_end` - End G-buffer, begin lighting
3. Implement `gf_deferred_lighting_msaa` - MSAA resolve pass
4. Implement `gf_deferred_lighting_finish` - Complete deferred pass
5. Port light volume rendering
6. Implement stencil-based light culling

**Reference:** `code/graphics/opengl/gropengldeferred.cpp` for deferred implementation

### 4.3 Shadow Mapping

**Files to Create:**
- `code/graphics/vulkan/VulkanShadows.h/cpp`

**Implementation:**

```cpp
class ShadowMapRenderer {
    static constexpr int CASCADE_COUNT = 4;

    struct ShadowCascade {
        vk::Image depthImage;
        vk::ImageView depthView;
        vk::UniqueFramebuffer framebuffer;
        matrix4 viewProjMatrix;
        float splitDistance;
    };

    std::array<ShadowCascade, CASCADE_COUNT> m_cascades;
    vk::UniqueRenderPass m_shadowPass;
    vk::UniqueSampler m_shadowSampler;  // PCF sampler

public:
    void begin(matrix4* shadow_view_matrix, const matrix* light_matrix,
               vec3d* eye_pos);
    void end();

    void bindShadowMaps(vk::CommandBuffer cmd, uint32_t binding);
};
```

**Tasks:**
1. Implement `gf_shadow_map_start` - Begin shadow pass
2. Implement `gf_shadow_map_end` - End shadow pass
3. Create cascaded shadow map framebuffers
4. Implement shadow matrix calculation
5. Create shadow sampling with PCF

**Reference:** `code/graphics/shadows.cpp` and `gropengldeferred.cpp`

### 4.4 Scene Texture System

**Implementation:**

```cpp
void vk_scene_texture_begin() {
    // Transition scene texture to render target
    // Begin scene render pass
}

void vk_scene_texture_end() {
    // End scene render pass
    // Transition scene texture to shader read
}

void vk_copy_effect_texture() {
    // Copy scene to effect texture for distortion effects
}
```

**Tasks:**
1. Implement `gf_scene_texture_begin` - Begin rendering to scene texture
2. Implement `gf_scene_texture_end` - End scene rendering
3. Implement `gf_copy_effect_texture` - Copy for effects

---

## Phase 5: Post-Processing Pipeline

**Objective:** Implement complete post-processing effects chain.

### 5.1 Post-Processing Framework

**Files to Create:**
- `code/graphics/vulkan/VulkanPostProcess.h/cpp`

**Implementation:**

```cpp
class PostProcessPipeline {
    struct PostProcessPass {
        vk::UniqueRenderPass pass;
        vk::UniqueFramebuffer framebuffer;
        vk::UniquePipeline pipeline;
        vk::Image inputTexture;
        vk::Image outputTexture;
    };

    // Ping-pong framebuffers
    PostProcessPass m_passA;
    PostProcessPass m_passB;

    // Effect textures
    vk::Image m_bloomTexture;
    vk::Image m_smaaEdges;
    vk::Image m_smaaBlend;

public:
    void begin();
    void end();

    void applyBloom();
    void applyFXAA();
    void applySMAA();
    void applyTonemap();
    void applyEffects(const SCP_vector<post_effect_t>& effects);
};
```

### 5.2 Bloom Effect

**Implementation:**

```cpp
void PostProcessPipeline::applyBloom() {
    // 1. Bright pass - extract bright pixels
    renderBrightPass(m_sceneTexture, m_bloomTexture);

    // 2. Generate mipmaps for blur
    generateMipmaps(m_bloomTexture);

    // 3. Blur each mip level (separable gaussian)
    for (int mip = 0; mip < BLOOM_MIP_LEVELS; ++mip) {
        blurMipLevel(m_bloomTexture, mip, BlurDirection::Horizontal);
        blurMipLevel(m_bloomTexture, mip, BlurDirection::Vertical);
    }

    // 4. Composite back to scene
    compositeBloom(m_sceneTexture, m_bloomTexture);
}
```

**Tasks:**
1. Create bright pass shader pipeline
2. Implement separable gaussian blur
3. Create bloom composite shader
4. Implement intensity control

### 5.3 Anti-Aliasing

**FXAA Implementation:**
```cpp
void PostProcessPipeline::applyFXAA() {
    // 1. Prepass - calculate luminance
    renderFXAAPrepass(m_inputTexture, m_luminanceTexture);

    // 2. Main FXAA pass
    renderFXAA(m_luminanceTexture, m_outputTexture);
}
```

**SMAA Implementation:**
```cpp
void PostProcessPipeline::applySMAA() {
    // 1. Edge detection
    renderSMAAEdges(m_inputTexture, m_smaaEdges);

    // 2. Blending weight calculation
    renderSMAAWeights(m_smaaEdges, m_smaaAreaTex, m_smaaSearchTex, m_smaaBlend);

    // 3. Neighborhood blending
    renderSMAABlend(m_inputTexture, m_smaaBlend, m_outputTexture);
}
```

**Tasks:**
1. Port FXAA shaders to SPIR-V
2. Port SMAA shaders to SPIR-V
3. Create SMAA lookup textures (area, search)
4. Implement quality level selection

### 5.4 Tone Mapping

**Implementation:**

```cpp
void PostProcessPipeline::applyTonemap() {
    // Push constants for tonemapping parameters
    struct TonemapParams {
        float exposure;
        float gamma;
        int tonemapMode;  // Linear, Reinhard, ACES, etc.
    } params;

    params.exposure = Gr_exposure;
    params.gamma = Gr_gamma;
    params.tonemapMode = Gr_tonemap_mode;

    vkCmdPushConstants(cmd, m_tonemapLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(params), &params);

    // Full-screen quad
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
```

**Tasks:**
1. Implement HDR to LDR conversion
2. Support multiple tonemapping operators
3. Implement gamma correction
4. Support VR linear output

### 5.5 Additional Effects

**Implementation for remaining effects:**

```cpp
// Effect uniform data
struct PostEffectUniforms {
    float noiseAmount;
    float saturation;
    float brightness;
    float contrast;
    float filmGrain;
    vec3 tint;
    float ditherStrength;
};

void PostProcessPipeline::applyEffects(const SCP_vector<post_effect_t>& effects) {
    PostEffectUniforms uniforms = {};

    for (const auto& effect : effects) {
        switch (effect.uniform_type) {
        case PostEffectUniformType::NoiseAmount:
            uniforms.noiseAmount = effect.intensity;
            break;
        case PostEffectUniformType::Saturation:
            uniforms.saturation = effect.intensity;
            break;
        // ... etc
        }
    }

    updateUniformBuffer(m_effectUniforms, &uniforms);
    renderEffectPass();
}
```

**Tasks:**
1. Implement `gf_post_process_begin`
2. Implement `gf_post_process_end`
3. Implement `gf_post_process_set_effect`
4. Implement `gf_post_process_set_defaults`
5. Implement `gf_post_process_save_zbuffer`
6. Implement `gf_post_process_restore_zbuffer`

**Reference:** `code/graphics/opengl/gropenglpostprocessing.cpp` for effect implementation

---

## Phase 6: Specialized Renderers

**Objective:** Implement material-specific rendering systems.

### 6.1 Particle Rendering

**Files to Create:**
- `code/graphics/vulkan/VulkanParticle.h/cpp`

**Implementation:**

```cpp
void vk_render_primitives_particle(particle_material* material_info,
                                   primitive_type prim_type,
                                   vertex_layout* layout,
                                   int offset, int n_verts,
                                   gr_buffer_handle buffer_handle)
{
    // Select particle shader (point sprites or quads)
    int shader = material_info->get_shader_flags() & SDR_FLAG_PARTICLE_POINT_GEN
        ? m_particleGeomShader : m_particleQuadShader;

    // Bind particle-specific uniforms
    particle_uniform_data uniforms;
    uniforms.windowWidth = gr_screen.max_w;
    uniforms.windowHeight = gr_screen.max_h;
    uniforms.nearPlane = Min_draw_distance;
    uniforms.farPlane = Max_draw_distance;

    // Render
    bindShader(shader);
    updateUniforms(&uniforms);
    draw(prim_type, offset, n_verts, buffer_handle);
}
```

**Tasks:**
1. Implement `gf_render_primitives_particle`
2. Port particle vertex/geometry/fragment shaders
3. Support soft particles (depth-based fade)
4. Support animated particle textures

### 6.2 Distortion Effects

**Implementation:**

```cpp
void vk_render_primitives_distortion(distortion_material* material_info,
                                     primitive_type prim_type,
                                     vertex_layout* layout,
                                     int offset, int n_verts,
                                     gr_buffer_handle buffer_handle)
{
    // Distortion requires scene texture copy
    copySceneToDistortionTexture();

    // Render distortion geometry with refraction shader
    bindShader(m_distortionShader);

    distortion_uniform_data uniforms;
    uniforms.distortionStrength = material_info->get_distortion_strength();
    uniforms.thrusterRendering = material_info->is_thruster_render();

    updateUniforms(&uniforms);
    draw(prim_type, offset, n_verts, buffer_handle);
}
```

**Tasks:**
1. Implement `gf_render_primitives_distortion`
2. Port distortion shaders
3. Implement thruster-specific distortion

### 6.3 Shield Impact Rendering

**Implementation:**

```cpp
void vk_render_shield_impact(shield_material* material_info,
                             primitive_type prim_type,
                             vertex_layout* layout,
                             gr_buffer_handle buffer_handle,
                             int n_verts)
{
    // Shield uses additive blending
    setBlendMode(ALPHA_BLEND_ADDITIVE);

    // Impact position and orientation in uniforms
    shield_uniform_data uniforms;
    uniforms.impactPosition = material_info->get_impact_pos();
    uniforms.impactRadius = material_info->get_impact_radius();
    vm_matrix_to_mat4(material_info->get_impact_orient(), uniforms.impactMatrix);

    bindShader(m_shieldShader);
    updateUniforms(&uniforms);
    draw(prim_type, 0, n_verts, buffer_handle);
}
```

**Tasks:**
1. Implement `gf_render_shield_impact`
2. Port shield shaders
3. Implement impact animation

### 6.4 Decal Rendering

**Implementation:**

```cpp
void vk_render_decals(decal_material* material_info,
                      primitive_type prim_type,
                      vertex_layout* layout,
                      int num_elements,
                      const indexed_vertex_source& buffers,
                      const gr_buffer_handle& instance_buffer,
                      int num_instances)
{
    // Decals use instanced rendering
    bindShader(m_decalShader);

    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &instanceOffset);
    vkCmdBindIndexBuffer(cmd, indexBuffer, indexOffset, indexType);

    vkCmdDrawIndexed(cmd, num_elements, num_instances, 0, 0, 0);
}
```

**Tasks:**
1. Implement `gf_render_decals`
2. Implement `gf_start_decal_pass`
3. Implement `gf_stop_decal_pass`
4. Port decal shaders with deferred decal support

### 6.5 UI Rendering (NanoVG)

**Implementation:**

```cpp
void vk_render_nanovg(nanovg_material* material_info,
                      primitive_type prim_type,
                      vertex_layout* layout,
                      int offset, int n_verts,
                      gr_buffer_handle buffer_handle)
{
    // NanoVG uses custom uniform structure
    nanovg_uniform_data uniforms;
    material_info->fill_nanovg_uniforms(uniforms);

    bindShader(m_nanovgShader);
    updateUniforms(&uniforms);
    draw(prim_type, offset, n_verts, buffer_handle);
}
```

**Tasks:**
1. Implement `gf_render_nanovg`
2. Port NanoVG shaders
3. Support all NanoVG blend modes

### 6.6 Video Playback

**Implementation:**

```cpp
void vk_render_movie(movie_material* material_info,
                     primitive_type prim_type,
                     vertex_layout* layout,
                     int n_verts,
                     gr_buffer_handle buffer,
                     size_t buffer_offset)
{
    // Movie rendering with YUV to RGB conversion
    movie_uniform_data uniforms;
    uniforms.yTexture = material_info->get_y_texture();
    uniforms.uTexture = material_info->get_u_texture();
    uniforms.vTexture = material_info->get_v_texture();

    bindShader(m_movieShader);
    updateUniforms(&uniforms);
    draw(prim_type, 0, n_verts, buffer);
}
```

**Tasks:**
1. Implement `gf_render_movie`
2. Port YUV conversion shader

### 6.7 Batched Bitmap Rendering

**Implementation:**

```cpp
void vk_render_primitives_batched(batched_bitmap_material* material_info,
                                  primitive_type prim_type,
                                  vertex_layout* layout,
                                  int offset, int n_verts,
                                  gr_buffer_handle buffer_handle)
{
    // Batched bitmaps for HUD elements
    bindShader(m_batchedBitmapShader);

    // Simple textured quad rendering
    draw(prim_type, offset, n_verts, buffer_handle);
}
```

**Tasks:**
1. Implement `gf_render_primitives_batched`
2. Port batched bitmap shaders

### 6.8 RocketUI Rendering

**Implementation:**

```cpp
void vk_render_rocket_primitives(interface_material* material_info,
                                 primitive_type prim_type,
                                 vertex_layout* layout,
                                 int n_indices,
                                 gr_buffer_handle vertex_buffer,
                                 gr_buffer_handle index_buffer)
{
    // RocketUI integration
    bindShader(m_rocketuiShader);

    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuffer, &offset);
    vkCmdBindIndexBuffer(cmd, ibuffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmd, n_indices, 1, 0, 0, 0);
}
```

**Tasks:**
1. Implement `gf_render_rocket_primitives` (function pointer, not std::function)
2. Port RocketUI shaders
3. Handle RocketUI texture management

---

## Phase 7: Optimization and Polish

**Objective:** Performance optimization and feature completion.

### 7.1 Performance Optimizations

**Pipeline State Caching:**
- Cache pipeline objects by state hash
- Implement pipeline derivatives
- Background pipeline compilation

**Descriptor Set Optimization:**
- Use descriptor set indexing for bindless textures
- Implement push descriptors for frequent updates
- Pool descriptor sets per frame

**Command Buffer Optimization:**
- Secondary command buffers for parallel recording
- Indirect drawing for batched geometry
- Multi-threaded command recording

**Memory Optimization:**
- Dedicated allocations for large resources
- Memory aliasing for transient resources
- Defragmentation for long sessions

### 7.2 Debug and Validation

**Tasks:**
1. Implement `gf_push_debug_group` - VK_EXT_debug_marker
2. Implement `gf_pop_debug_group`
3. Add validation layer integration
4. Implement GPU profiling markers

### 7.3 Query Objects

**Implementation:**

```cpp
int vk_create_query_object() {
    // Create timestamp/occlusion query
    vk::QueryPoolCreateInfo info;
    info.queryType = vk::QueryType::eTimestamp;
    info.queryCount = 1;

    auto pool = m_device.createQueryPoolUnique(info);
    return registerQueryPool(std::move(pool));
}

void vk_query_value(int obj, QueryType type) {
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        m_queryPools[obj], 0);
}

uint64_t vk_get_query_value(int obj) {
    uint64_t result;
    vkGetQueryPoolResults(m_device, m_queryPools[obj], 0, 1,
                          sizeof(result), &result, sizeof(result),
                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return result;
}
```

**Tasks:**
1. Implement `gf_create_query_object`
2. Implement `gf_query_value`
3. Implement `gf_query_value_available`
4. Implement `gf_get_query_value`
5. Implement `gf_delete_query_object`

### 7.4 Capability System

**Implementation:**

```cpp
bool vk_is_capable(gr_capability capability) {
    switch (capability) {
    case CAPABILITY_ENVIRONMENT_MAP:
        return m_features.envMapping;
    case CAPABILITY_NORMAL_MAP:
        return true;
    case CAPABILITY_SOFT_PARTICLES:
        return true;
    case CAPABILITY_DISTORTION:
        return true;
    case CAPABILITY_POST_PROCESSING:
        return true;
    case CAPABILITY_DEFERRED_LIGHTING:
        return true;
    case CAPABILITY_SHADOWS:
        return true;
    case CAPABILITY_THICK_OUTLINE:
        return m_features.geometryShaders;
    case CAPABILITY_BATCHED_SUBMODELS:
        return true;
    case CAPABILITY_TIMESTAMP_QUERY:
        return true;
    case CAPABILITY_SEPARATE_BLEND_FUNCTIONS:
        return m_features.independentBlend;
    case CAPABILITY_PERSISTENT_BUFFER_MAPPING:
        return true;  // Always supported in Vulkan
    default:
        return false;
    }
}

bool vk_get_property(gr_property property, void* destination) {
    switch (property) {
    case UNIFORM_BUFFER_OFFSET_ALIGNMENT:
        *static_cast<int*>(destination) =
            m_physicalDeviceProperties.limits.minUniformBufferOffsetAlignment;
        return true;
    // ... other properties
    }
}
```

**Tasks:**
1. Implement `gf_is_capable` with accurate capability reporting
2. Implement `gf_get_property` for device limits

### 7.5 Sync and Fence System

**Implementation:**

```cpp
gr_sync vk_sync_fence() {
    vk::FenceCreateInfo info;
    auto fence = m_device.createFenceUnique(info);

    // Submit empty command buffer to signal fence
    vk::SubmitInfo submit;
    m_graphicsQueue.submit(1, &submit, fence.get());

    return reinterpret_cast<gr_sync>(fence.release());
}

bool vk_sync_wait(gr_sync sync, uint64_t timeout_ns) {
    vk::Fence fence = reinterpret_cast<vk::Fence>(sync);
    auto result = m_device.waitForFences(1, &fence, VK_TRUE, timeout_ns);
    return result == vk::Result::eSuccess;
}

void vk_sync_delete(gr_sync sync) {
    vk::Fence fence = reinterpret_cast<vk::Fence>(sync);
    m_device.destroyFence(fence);
}
```

**Tasks:**
1. Implement `gf_sync_fence`
2. Implement `gf_sync_wait`
3. Implement `gf_sync_delete`

### 7.6 Screen Operations

**Tasks:**
1. Implement `gf_save_screen` - Copy framebuffer to texture
2. Implement `gf_restore_screen` - Blit saved texture back
3. Implement `gf_free_screen` - Free saved screen
4. Implement `gf_get_region` - Read back framebuffer region
5. Implement `gf_print_screen` - Screenshot to file
6. Implement `gf_blob_screen` - Screenshot to memory

### 7.7 Additional Features

**Environment Mapping:**
1. Implement `gf_dump_envmap` - Save environment map
2. Implement `gf_calculate_irrmap` - Generate irradiance map

**OpenXR Support (Optional):**
1. Implement `gf_openxr_get_extensions`
2. Implement `gf_openxr_test_capabilities`
3. Implement `gf_openxr_create_session`
4. Implement `gf_openxr_get_swapchain_format`
5. Implement `gf_openxr_acquire_swapchain_buffers`
6. Implement `gf_openxr_flip`

---

## Risk Assessment and Mitigation

### High Risk Items

| Risk | Impact | Mitigation |
|------|--------|------------|
| Shader compilation complexity | High | Maintain SPIR-V compilation pipeline, test each shader type |
| State management differences | High | Create comprehensive state mapping, test edge cases |
| Synchronization bugs | High | Use validation layers, careful barrier placement |
| Memory management issues | Medium | Use VMA library, track allocations carefully |
| Performance regressions | Medium | Profile early and often, compare with OpenGL |

### Dependencies

| Dependency | Version | License |
|------------|---------|---------|
| Vulkan SDK | 1.1+ | Apache 2.0 |
| vulkan.hpp | Bundled | Apache 2.0 |
| VMA | 3.0+ | MIT |
| SDL2 | 2.0.6+ | zlib |
| SPIRV-Cross (optional) | Latest | Apache 2.0 |

---

## Testing Strategy

### Unit Testing

1. **Memory allocation tests** - Allocate/free patterns
2. **Buffer management tests** - Create/update/map operations
3. **Pipeline creation tests** - All state combinations
4. **Descriptor set tests** - Binding and updating

### Integration Testing

1. **Basic rendering test** - Simple triangle rendering
2. **Texture rendering test** - Textured quad
3. **Model rendering test** - Single ship model
4. **Post-processing test** - Each effect individually
5. **Full scene test** - Complete mission rendering

### Performance Testing

1. **Draw call benchmarks** - Compare with OpenGL
2. **Memory bandwidth tests** - Texture upload speeds
3. **GPU utilization** - Profile with vendor tools
4. **Frame time analysis** - Consistent frame pacing

### Regression Testing

1. **Visual comparison** - Screenshot comparison with OpenGL
2. **Gameplay testing** - Full campaign playthrough
3. **Mod compatibility** - Test popular total conversions
4. **Edge cases** - Resolution changes, window modes, multi-monitor

---

## Implementation Priority

### Critical Path (Required for basic functionality)

1. Phase 1: Core Infrastructure (1.1-1.6)
2. Phase 2: Resource Management (2.1-2.4)
3. Phase 3: Basic Rendering (3.1-3.5)
4. Phase 4.1: Model Rendering

### High Priority (Required for feature parity)

5. Phase 4.2-4.4: Deferred, Shadows, Scene Textures
6. Phase 5: Post-Processing
7. Phase 6.1-6.2: Particles, Distortion

### Medium Priority (Full feature support)

8. Phase 6.3-6.8: Specialized Renderers
9. Phase 7.1-7.4: Optimization, Debug, Queries

### Low Priority (Optional/Future)

10. Phase 7.5-7.7: Sync, Screen Ops, OpenXR

---

## Conclusion

This plan provides a comprehensive roadmap for porting FreeSpace 2's rendering pipeline to Vulkan. The existing infrastructure with function pointer abstraction and basic Vulkan scaffolding provides an excellent foundation. The phased approach allows incremental progress with testable milestones at each stage.

Key success factors:
1. **Maintain OpenGL as fallback** during development
2. **Test early and often** with validation layers
3. **Profile continuously** to catch performance issues
4. **Document thoroughly** for future maintenance

The port will modernize the rendering backend, enabling better performance on modern hardware and opening possibilities for future enhancements like ray tracing support.
