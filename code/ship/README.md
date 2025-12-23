# Ship Management — `code/ship`

This subsystem implements the gameplay logic for ships, which are specialized `object` types. It handles subsystems, weapons, shields, AI integration, and damage simulation.

## Architecture

The ship system is built on top of the generic `object` system. Every ship is an object (`OBJ_SHIP`), but not every object is a ship.

### 1. The `ship` Class (`ship.h`)
The `ship` class extends the basic object with space-combat specific data:
*   **Identity:** `ship_name`, `ship_info_index` (class type), `team` (Friendly, Hostile, etc.).
*   **Systems:** `weapons` (banks, ammo, cooldowns), `shield_points`, `afterburner_fuel`.
*   **Subsystems:** A linked list of `ship_subsys` representing engines, turrets, sensors, and communications.
*   **Status:** `flags` (e.g., `SF_DYING`, `SF_ENGINES_DISABLED`), `hull_strength`, `shield_integrity`.

### 2. Subsystem Management
Ships are composed of subsystems defined in the model file (POF) and table data.
*   **`ship_subsys`:** Represents a specific instance of a subsystem on a ship (e.g., "Engine 1").
*   **Damage Propagation:** Damage to a specific point on the ship hull is routed to the nearest subsystem.
*   **Functional Impact:** Subsystem health directly affects ship capabilities (e.g., engines below 30% cannot warp).

### 3. Weapons & Turrets
*   **Primary/Secondary Banks:** Managed via `ship_weapon`. Tracks ammo, linkage, and firing status.
*   **Turrets:** Specialized subsystems that act as autonomous weapons. They have their own AI logic (`code/ai/aiturret.cpp`) but store their state (orientation, target) in `ship_subsys`.

### 4. Damage & Collision Response (`shiphit.cpp`)
Handles the result of a collision or weapon impact:
*   **Shields:** Quadrant-based damage. Shield impacts render a procedurally generated effect (`create_shield_explosion`).
*   **Hull:** When shields fail, damage is applied to the hull and subsystems.
*   **Effects:** Sparks, debris, and electrical arcs are spawned based on damage severity.

## Design Patterns

### Typestate: Object Linkage
A `ship` struct is always linked to an `object`.
*   `Objects[objnum].instance` points to `Ships[shipnum]`.
*   `Ships[shipnum].objnum` points back to `Objects[objnum]`.
*   **Critical:** This linkage must remain consistent. `ship_create()` and `ship_delete()` manage this bidirectional link.

### State as Location (Flags)
Ship behavior is heavily flag-driven (`ship_flags.h`):
*   `Ship_Flags::Warping_in`: Processing arrival effect.
*   `Ship_Flags::Dying`: Processing death roll/explosion.
*   `Ship_Flags::Invulnerable`: Immune to damage.

## Interaction with Vulkan

The Vulkan backend interacts with the ship subsystem for:
*   **Texture Replacement:** `ship::replacement_textures` allows dynamic skinning (e.g., pirate markings). The backend's `VulkanRenderer::setReplacementTextures` consumes this list.
*   **Shield Rendering:** Shield impacts generate geometry (shield mesh) that is rendered via `gr_render_shield_impact`.
*   **Thruster Glows:** Engine glow intensity (`thruster_glow_noise`) is calculated here and passed to the renderer as a uniform.
*   **Subsystem visibility:** Destroyed subsystems (like blown-off turrets) are hidden by the renderer based on flags in `ship_subsys`.

## Common Pitfalls

*   **Subsystem Iteration:** Subsystems are a linked list, not a vector. Iterating them requires traversing `next` pointers.
*   **Weapon Banks:** Indices 0-2 are primaries, 3-6 are secondaries. Mixing these up causes "impossible" weapon loads.
*   **AI Class:** The `ai_class` index in `ship` must be valid if the ship is AI-controlled. Invalid indices crash the AI decision loop.
