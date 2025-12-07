# 3D Rendering Pipeline — `code/render`

This subsystem implements the high-level 3D transformation, projection, and geometry processing pipeline (the `g3_` API). It acts as the primary bridge between the game's world-space logic (ships, debris, stars) and the low-level `gr_` graphics abstraction.

## Architecture

The 3D pipeline follows a **Stateful Transformation Pattern**. It manages a stack of coordinate systems (instances) and provides utilities for converting world-space coordinates into screen-space pixels.

### 1. The `g3_` API (`3d.h`)
The core interface for all 3D operations:
*   **Lifecycle:** `g3_start_frame()` and `g3_end_frame()` demarcate 3D rendering blocks.
*   **View Setup:** `g3_set_view()` and `g3_set_view_matrix()` configure the camera position and orientation.
*   **Transformation:** `g3_rotate_vertex()` converts world-space points to view-space.
*   **Projection:** `g3_project_vertex()` converts view-space points to 2D screen coordinates.
*   **Drawing:** `g3_draw_line()`, `g3_draw_sphere()`, and `g3_render_model()` (the main entry point for ship rendering).

### 2. Coordinate System & Instancing (`3dsetup.cpp`)
Manages the transformation hierarchy:
*   **`View_matrix` / `View_position`**: The current camera transform.
*   **`g3_start_instance_matrix()`**: Pushes a new local coordinate system onto the stack. Used for rendering submodels (turrets, spinning radars) or placing objects in the world.
*   **`g3_done_instance()`**: Pops the stack, restoring the previous coordinate system.
*   **Transform Stack:** A fixed-depth stack (`MAX_INSTANCE_DEPTH = 32`) that stores matrices and positions.

### 3. Math & Projection (`3dmath.cpp`)
Contains the heavy lifting for 3D calculations:
*   **FOV Handling:** Manages zoom and aspect ratio scaling.
*   **Clipping:** Implements view-frustum clipping for lines and polygons to prevent rendering artifacts from points behind the camera (`CC_BEHIND`).
*   **Point-to-Vec:** Utilities to cast rays from 2D screen clicks back into 3D world space (used for targeting and editor picking).

### 4. Batching (`batching.cpp`)
A modern addition to the pipeline that collects similar primitives (triangles, lines) into contiguous buffers before submission to the GPU. This significantly reduces draw call overhead in the `gr_` backend.

## Design Patterns

### Hardware Transform & Lighting (HT&L)
While the legacy pipeline performed rotation and projection on the CPU, the modern path (especially Vulkan) prefers **HT&L**.
*   In HT&L mode, `g3_` functions may skip CPU projection and instead pass transformation matrices directly to the graphics backend via uniform buffers.
*   The `RenderCtx` and `FrameCtx` tokens in Vulkan enforce that these matrices are updated at the correct phase of the frame.

### Typestate Sequence
The 3D pipeline enforces a strict call order:
1.  `gr_setup_frame()` (Backend init)
2.  `g3_start_frame()` (Render state init)
3.  `g3_set_view()` (Camera setup)
4.  Render Objects (`g3_start_instance` -> draw -> `g3_done_instance`)
5.  `g3_end_frame()` (Cleanup)

## Interaction with Vulkan
The Vulkan backend relies on `code/render` to provide:
*   **Matrix Data:** The `View_matrix` and `Object_matrix` are packed into uniform blocks.
*   **Clipping Codes:** Used to determine if an object is entirely off-screen before committing expensive GPU resources.
*   **Batching:** `batching.cpp` interacts with `gr_render_primitives_batched` to feed the Vulkan ring buffers.

## Common Pitfalls
*   **Matrix Transposure:** `matrix` (3x3) is row-major, but `matrix4` (4x4) is column-major. Incorrect conversion will result in objects flying into infinity.
*   **Z-Buffer Sentiment:** `g3_start_frame` requires a flag to enable/disable Z-buffering. Disabling it mid-frame can cause sorting issues for transparent effects.
*   **Instance Leaks:** Every `g3_start_instance` **must** have a corresponding `g3_done_instance`. Failing to pop the stack will corrupt all subsequent object transforms.
