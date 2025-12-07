# Object Management & Collision — `code/object`

This subsystem manages the lifecycle, spatial state, and interaction logic for all dynamic entities within the game world. It serves as the authoritative source for entity positions and orchestrates the transition between physics, collision detection, and rendering.

## Architecture

The object subsystem operates on a fixed-pool array of `object` instances, managed through linked lists to minimize allocation overhead during high-intensity combat.

### 1. The `object` Class (`object.h`)
The `object` class is the root representation for every dynamic entity.
*   **Core Types:** `OBJ_SHIP`, `OBJ_WEAPON`, `OBJ_DEBRIS`, `OBJ_FIREBALL`, `OBJ_ASTEROID`, and `OBJ_BEAM`.
*   **Spatial State:** Stores authoritative `pos` (position) and `orient` (orientation), along with `last_pos` and `last_orient` for interpolation and swept-collision detection.
*   **Physics Integration:** Each object contains a `physics_info` struct (see `code/physics`) which defines its mass, velocity, and rotational inertia.
*   **Subsystem Links:** The `instance` member acts as an index into type-specific arrays (e.g., if `type == OBJ_SHIP`, `instance` indexes the `Ships[]` array).

### 2. Lifecycle Management
*   **Creation:** `obj_create()` initializes a new slot in the `Objects[]` pool and links it into the active object list.
*   **Deletion:** Objects are marked with `Object_Flags::Should_be_dead`. The `obj_delete_all_that_should_be_dead()` function performs the actual cleanup at the end of the frame to avoid use-after-free errors during the physics or rendering phases.
*   **Signatures:** Every object is assigned a unique `signature`. This allows safe referencing (via `object_h`) that can detect if an object has been deleted and its array slot reused.

### 3. Collision Detection (`objcollide.h`)
The engine uses a multi-stage collision pipeline:
*   **Broad Phase:** Uses a sorted axis-based approach (Sweep and Prune) to identify potential overlaps.
*   **Narrow Phase:** Performs detailed checks based on object types:
    *   `collideshipship.cpp`: Complex mesh-to-mesh or sphere-to-mesh checks for ships.
    *   `collideshipweapon.cpp`: Ray-to-mesh checks for projectiles.
*   **Response:** Calculates impulses and applies damage to hull/shields based on impact velocity and location.

### 4. Rendering Orchestration (`objectsort.cpp`)
Before rendering, objects are sorted based on distance from the `Viewer_obj`.
*   **Opaque Objects:** Rendered front-to-back to optimize depth testing.
*   **Transparent Objects:** Rendered back-to-front to ensure correct alpha blending.
*   **Vulkan Integration:** This sorting logic determines the order in which `VulkanRenderer::ensureRenderingStarted()` is called and how draw calls are recorded into command buffers.

## Design Patterns

### State as Location (Flags)
Object behavior is driven by `Object_Flags` (defined in `object_flags.h`):
*   `Collides`: Enables participation in the collision pipeline.
*   `Renders`: Enables the object to be passed to the rendering subsystem.
*   `Physics`: Enables velocity and orientation integration.

### Typestate Phases
The game loop processes objects in a strict sequence:
1.  **Move Phase:** `obj_move_all()` updates positions based on physics.
2.  **Collide Phase:** `obj_sort_and_collide()` resolves interactions.
3.  **Render Phase:** `obj_render_all()` queues drawing operations.
4.  **Cleanup Phase:** `obj_delete_all_that_should_be_dead()` purges marked objects.

## Interaction with Vulkan

The Vulkan backend relies on the Object subsystem for:
*   **Culling:** Object `radius` and `pos` are used to perform frustum culling before any Vulkan API calls are made.
*   **Uniform Updates:** Object `orient` and `pos` are packed into `matrix4` format and uploaded to GPU uniform buffers via `VulkanRingBuffer`.
*   **Batching:** Collision groups and object types influence how primitives are batched in `batching.cpp` before submission to the graphics backend.

## Common Pitfalls

*   **Signature Validation:** Never access `Objects[index]` directly if the index was stored across frames. Always use `object_h` or verify the `signature`.
*   **Direct Modification:** Avoid modifying `pos` or `orient` directly without updating `last_pos` and `last_orient`, as this will break motion-based collision detection and interpolation.
*   **Collision Groups:** Incorrectly setting `collision_group_id` can cause objects to ghost through each other or cause performance death-spirals if too many objects are checked.
