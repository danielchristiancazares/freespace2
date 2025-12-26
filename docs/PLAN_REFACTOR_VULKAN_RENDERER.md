# VulkanRenderer.cpp Refactoring Analysis

## Summary
- **File**: `code/graphics/vulkan/VulkanRenderer.cpp`
- **Size**: 3875 lines (largest in Vulkan codebase)
- **Functions**: ~65 methods
- **Analysis scope**: Document separation points for future refactoring

---

## Function Inventory by Category

### 1. Post-Processing Pipeline (1580 lines, 38%)
**Location**: Lines 760-2740
**Cohesion**: Very high - all functions follow same pattern

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `endSceneTexture` | 340 | 760 | **Orchestrator** - calls all post passes |
| `recordPreDeferredSceneColorCopy` | 80 | 1248 | Scene backup (LDR) |
| `recordPreDeferredSceneHdrCopy` | 80 | 1327 | Scene backup (HDR) |
| `recordTonemappingToSwapchain` | 123 | 1406 | HDR -> LDR conversion |
| `recordBloomBrightPass` | 79 | 1529 | Bloom threshold extraction |
| `recordBloomBlurPass` | 106 | 1608 | Gaussian blur (ping-pong) |
| `recordBloomCompositePass` | 106 | 1714 | Bloom additive blend |
| `recordSmaaEdgePass` | 88 | 1820 | SMAA edge detection |
| `recordSmaaBlendWeightsPass` | 102 | 1908 | SMAA blending weights |
| `recordSmaaNeighborhoodPass` | 96 | 2010 | SMAA final blend |
| `recordFxaaPrepass` | 70 | 2106 | FXAA luma injection |
| `recordFxaaPass` | 88 | 2176 | FXAA resolve |
| `recordLightshaftsPass` | 108 | 2264 | God rays |
| `recordPostEffectsPass` | 99 | 2372 | Color grading, film grain |
| `generateBloomMipmaps` | 186 | 2471 | Mip chain generation |
| `recordCopyToSwapchain` | 83 | 2657 | Final blit to swapchain |

**Dependencies for extraction**:
- `RenderCtx`, `VulkanFrame` - context tokens
- `VulkanRenderTargets` - image views, samplers
- `VulkanRenderingSession` - state transitions
- `m_smaaAreaTex`, `m_smaaSearchTex` - SMAA lookup textures
- `m_descriptorLayouts` - push descriptor writes

**Extraction recommendation**: Create `VulkanPostProcessing` class. Keep `endSceneTexture` in `VulkanRenderer` as orchestrator.

---

### 2. Deferred Lighting (500 lines, 13%)
**Location**: Lines 649-1248 and 3521-3875
**Cohesion**: High - shared state (meshes, G-buffer bindings)

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `beginDeferredLighting` | 51 | 649 | Start G-buffer pass |
| `endDeferredGeometry` | 5 | 700 | End geometry subpass |
| `deferredLightingBegin` | 4 | 1103 | Typestate wrapper (returns token) |
| `deferredLightingEnd` | 17 | 1109 | Typestate wrapper |
| `deferredLightingFinish` | 33 | 1126 | Apply lights, restore scissor |
| `bindDeferredGlobalDescriptors` | 88 | 1160 | Update global descriptor set |
| `createDeferredLightingResources` | 118 | 3521 | Create meshes (fullscreen/sphere/cylinder) |
| `createSmaaLookupTextures` | 130 | 3639 | Upload SMAA area/search textures |
| `recordDeferredLighting` | 106 | 3769 | Draw light volumes |

**State ownership**:
- `m_fullscreenMesh`, `m_sphereMesh`, `m_cylinderMesh` - light volume geometry
- `m_smaaAreaTex`, `m_smaaSearchTex` - lookup textures (could go to PostProcessing)
- `m_globalDescriptorSet` - G-buffer bindings

**Extraction recommendation**: Expand existing `VulkanDeferredLights` class. It already has `buildDeferredLights()` and UBO structs.

---

### 3. Frame Management (330 lines, 9%)
**Location**: Lines 173-559
**Cohesion**: High - frame lifecycle

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `queryCompletedSerial` | 17 | 173 | Query timeline semaphore |
| `maybeRunVulkanStress` | 42 | 190 | Stress testing harness |
| `acquireImage` | 27 | 232 | Swapchain image acquire |
| `acquireImageOrThrow` | 26 | 259 | Acquire with exception |
| `beginFrame` | 56 | 285 | Start frame recording |
| `endFrame` | 11 | 341 | Submit frame |
| `updateSavedScreenCopy` | 121 | 352 | Screen save tracking |
| `incrementModelDraw` | 4 | 473 | Stats counter |
| `incrementPrimDraw` | 4 | 477 | Stats counter |
| `logFrameCounters` | 5 | 481 | Debug logging |
| `prepareFrameForReuse` | 11 | 486 | Reset frame state |
| `recycleOneInFlight` | 20 | 497 | Move frame to available |
| `acquireAvailableFrame` | 42 | 517 | Get frame for recording |
| `applySetupFrameDynamicState` | 15 | 559 | Viewport/scissor setup |

**State ownership**:
- `m_frames`, `m_availableFrames`, `m_inFlightFrames` - frame containers
- `m_submitTimeline`, `m_submitSerial`, `m_completedSerial` - sync primitives

**Extraction recommendation**: Some logic already in `VulkanFrameFlow.h` (tokens). Could create `VulkanFrameManager` but coupling is tight.

---

### 4. Descriptor Management (265 lines, 7%)
**Location**: Lines 2933-3199
**Cohesion**: High - all about descriptor updates

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `getBindlessTextureIndex` | 50 | 2933 | Bindless slot lookup |
| `setModelUniformBinding` | 45 | 2983 | Per-draw uniform offset |
| `setSceneUniformBinding` | 21 | 3028 | Scene-level uniforms |
| `updateModelDescriptors` | 122 | 3049 | Write descriptor set |
| `beginModelDescriptorSync` | 28 | 3171 | Frame-start sync |

**State ownership**:
- `m_modelBindlessCache` - per-frame descriptor cache

**Extraction recommendation**: Could merge into `VulkanDescriptorLayouts` or create `VulkanDescriptorBinder`.

---

### 5. Initialization (200 lines, 5%)
**Location**: Lines 46-173, 3394-3407
**Cohesion**: Medium - one-time setup

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| Constructor | 5 | 46 | Initialize members |
| `initialize` | 57 | 53 | Create all subsystems |
| `createDescriptorResources` | 9 | 110 | Descriptor pool/layouts |
| `createFrames` | 25 | 119 | Allocate frame objects |
| `createRenderTargets` | 5 | 144 | Delegate to manager |
| `createRenderingSession` | 6 | 149 | Create session |
| `createUploadCommandPool` | 7 | 155 | Upload pool |
| `createSubmitTimelineSemaphore` | 11 | 162 | Timeline sync |
| `shutdown` | 13 | 3394 | Cleanup |

**Extraction recommendation**: Keep in VulkanRenderer - initialization is the coordinator's job.

---

### 6. Buffer API Wrappers (70 lines, 2%)
**Location**: Lines 2740-2810

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `setModelVertexHeapHandle` | 12 | 2740 | Register vertex heap |
| `deleteBuffer` | 5 | 2752 | Delegate to BufferManager |
| `updateBufferData` | 5 | 2757 | Delegate |
| `updateBufferDataOffset` | 5 | 2762 | Delegate |
| `mapBuffer` | 5 | 2767 | Delegate |
| `flushMappedBuffer` | 5 | 2772 | Delegate |
| `resizeBuffer` | 33 | 2777 | Delegate with validation |

**Extraction recommendation**: Keep as thin wrappers - they provide unified API.

---

### 7. Scene Texture Management (90 lines, 2%)
**Location**: Lines 705-760

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `setPendingRenderTargetSwapchain` | 5 | 705 | Request swapchain target |
| `requestMainTargetWithDepth` | 10 | 710 | Request main + depth |
| `beginSceneTexture` | 23 | 720 | Enter HDR scene mode |
| `copySceneEffectTexture` | 17 | 743 | Copy for effects |

**State ownership**:
- `m_sceneTexture` - optional state tracking

**Extraction recommendation**: Could merge into `VulkanPostProcessing` since it's scene texture lifecycle.

---

### 8. Texture API Wrappers (90 lines, 2%)
**Location**: Lines 3199-3298

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `preloadTexture` | 7 | 3199 | Delegate to TextureManager |
| `updateTexture` | 23 | 3206 | Streaming texture update |
| `releaseBitmap` | 18 | 3229 | Release with deferred delete |
| `uploadMovieTexture` | 26 | 3247 | Movie YUV upload |
| `drawMovieTexture` | 17 | 3273 | Movie quad draw |
| `releaseMovieTexture` | 8 | 3290 | Movie cleanup |

**Extraction recommendation**: Keep as thin wrappers.

---

### 9. Render State (100 lines, 3%)
**Location**: Lines 2810-2933

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `beginDecalPass` | 16 | 2810 | Prepare for decals |
| `setViewport` | 10 | 2826 | Dynamic viewport |
| `setScissor` | 10 | 2836 | Dynamic scissor |
| `createBitmapRenderTarget` | 43 | 2846 | RTT creation |
| `setBitmapRenderTarget` | 44 | 2889 | Activate RTT |

**Extraction recommendation**: Could merge into `VulkanRenderingSession` since it manages render targets.

---

### 10. Graphics State (115 lines, 3%)
**Location**: Lines 3407-3521

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `setClearColor` | 4 | 3407 | Set clear color |
| `setCullMode` | 17 | 3411 | Cull mode state |
| `setZbufferMode` | 28 | 3428 | Depth mode state |
| `getZbufferMode` | 5 | 3456 | Query depth mode |
| `requestClear` | 4 | 3461 | Request buffer clear |
| `zbufferClear` | 18 | 3465 | Clear depth buffer |
| `saveScreen` | 14 | 3483 | Save screen to texture |
| `freeScreen` | 16 | 3497 | Free saved screen |
| `frozenScreenHandle` | 8 | 3513 | Query frozen screen |

**State ownership**:
- `m_zbufferMode` - depth mode tracking
- `m_savedScreen` - screen capture state

**Extraction recommendation**: Keep in VulkanRenderer - this is core renderer state.

---

### 11. Init Submission (100 lines, 3%)
**Location**: Lines 3298-3394

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `submitInitCommandsAndWait` | 96 | 3298 | One-shot command submission |

**Extraction recommendation**: Keep - init-time only helper.

---

### 12. Debug Helpers (40 lines, 1%)
**Location**: Lines 574-612

| Function | Lines | Start | Purpose |
|----------|-------|-------|---------|
| `pushDebugGroup` | 21 | 574 | Debug label start |
| `popDebugGroup` | 17 | 595 | Debug label end |
| `flushQueuedTextureUploads` | 37 | 612 | Flush texture queue |

---

## Recommended Extraction Priority

### Phase 1: VulkanPostProcessing (1580 lines -> new file)
- Clearest boundaries
- Largest impact
- Minimal coupling
- `endSceneTexture` stays as orchestrator

### Phase 2: Expand VulkanDeferredLights (350 lines -> existing file)
- Already has infrastructure
- Natural grouping

### Phase 3: VulkanDescriptorBinder (265 lines -> new file)
- Clean separation
- Could also merge into VulkanDescriptorLayouts

---

## Dependency Graph

```
VulkanRenderer
    |
    +-- VulkanPostProcessing (new)
    |       |-- RenderCtx, VulkanFrame
    |       |-- VulkanRenderTargets
    |       |-- VulkanRenderingSession
    |       |-- VulkanPipelineManager
    |       |-- SmaaLookupTextures
    |
    +-- VulkanDeferredLights (expand)
    |       |-- VulkanFrame
    |       |-- VulkanRenderTargets
    |       |-- VulkanBufferManager (for meshes)
    |
    +-- VulkanDescriptorBinder (new)
            |-- VulkanDescriptorLayouts
            |-- VulkanTextureManager
            |-- ModelBindlessCache
```

---

## Implementation Notes

If extraction proceeds:
1. **Forward declarations** in VulkanRenderer.h for new classes
2. **Friend classes** or accessor methods for private state
3. **Context tokens** (RenderCtx, FrameCtx) remain unchanged
4. **VulkanRenderingSession** interaction stays via existing public API
5. **Pipeline/shader access** via VulkanRenderer accessors
