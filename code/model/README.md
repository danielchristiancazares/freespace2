# Model Subsystem — `code/model`

This subsystem manages the loading, representation, and rendering of the engine's 3D assets, stored in the proprietary **POF (Parity Object File)** format. It handles complex hierarchical meshes, submodel animations, and multi-layered texturing.

## Architecture

The model subsystem is the primary consumer of the `code/render` and `code/graphics` pipelines. It represents ships and stations not as single meshes, but as trees of submodels.

### 1. The `polymodel` Class (`model.h`)
A `polymodel` is the static template for a 3D asset loaded from disk.
*   **Hierarchy:** Contains an array of `bsp_info` (submodels) organized in a parent-child tree.
*   **Detail Levels (LOD):** Stores multiple versions of the mesh for different distances (`detail[]`).
*   **Hardpoints:** Defines positions for gun/missile banks, docking bays, thrusters, and "eyes" (viewpoints).
*   **Shields:** Stores the specialized `shield_info` mesh used for high-frequency collision and impact effects.

### 2. The `polymodel_instance` Structure
Represents a specific, live instance of a model in the game world.
*   **Stateful Submodels:** Each instance has a unique `submodel_instance` array tracking current rotation angles and translation offsets (e.g., a rotating radar dish or a tracking turret).
*   **Texture Overrides:** Allows specific instances to have different skins (e.g., damaged hulls or team-specific markings) without duplicating the geometry.

### 3. Model Loading & Interpretation (`modelread.cpp`, `modelinterp.cpp`)
*   **POF Parser:** `model_load()` reads the binary POF format and reconstructs the vertex buffers and hierarchy.
*   **Interpreter:** The "Interpreter" logic bridges the gap between the static BSP-based data and the modern vertex-buffer-oriented rendering path.

### 4. Rendering (`modelrender.cpp`, `modelrender.h`)
*   **Deferred Queueing:** `model_render_queue()` adds model components to a `model_draw_list`.
*   **Batching:** Optimized paths collect submodels from multiple ships into unified draw calls to reduce GPU overhead.
*   **Special Effects:** Handles rendering of thruster glows, electrical arcs, and insignias.

## Design Patterns

### Hierarchical Transformation
The subsystem uses a recursive tree traversal to calculate global positions for submodels.
`Parent Transform * Local Offset * Submodel Rotation = Global Submodel Transform`.

### State as Location (Texture Mapping)
The engine supports multi-layered PBR-like texturing using the `texture_map` class:
*   `TM_BASE_TYPE`: Diffuse/Albedo.
*   `TM_NORMAL_TYPE`: Normal maps for surface detail.
*   `TM_SPECULAR_TYPE`: Specular/Reflectance.
*   `TM_GLOW_TYPE`: Self-illumination maps.

## Interaction with Vulkan

The model subsystem is the "heavy lifter" for the Vulkan backend:
*   **Vertex Pulling:** The Vulkan backend typically uses a vertex-pulling strategy where the model's vertex data is stored in a large GPU storage buffer and indexed by the shader.
*   **Bindless Textures:** All textures used by a model are assigned slots in a global descriptor array managed by `VulkanTextureManager`.
*   **Uniform Blocks:** Per-instance data (matrices, thruster noise, flash effects) is packed into `ModelData` uniform blocks (`uniform_block_type::ModelData`).
*   **Typestates:** `ModelBoundFrame` tokens ensure that model descriptors are synchronized before any draw calls are recorded.

## Common Pitfalls

*   **Submodel Bounds:** If a submodel's bounding box is calculated incorrectly during POF generation, it will be aggressively culled by the `model_render_check_detail_box` logic, leading to "invisible" turrets or parts.
*   **Matrix Order:** Remember that `matrix` (3x3) used for orientation is row-major, while the GPU expects `matrix4` (4x4) in column-major order.
*   **LOD Switching:** Hard-coded detail distances can cause "popping." Verify `detail_distance[]` settings in the table data if objects disappear prematurely.

## Documentation Note
The POF file format is complex and binary. For details on the internal chunk structure (e.g., `TXTR`, `HDR2`, `OBJ2`), refer to the engine's technical Wiki or the `modelread.cpp` parser logic.
