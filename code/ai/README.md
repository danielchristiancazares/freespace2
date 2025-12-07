# Artificial Intelligence Subsystem — `code/ai`

The AI subsystem governs the decision-making, combat tactics, and mission-level goal fulfillment for all non-player entities (NPCs) in the game world.

## Architecture

The AI is implemented as a state-driven machine that operates on each AI-controlled object every frame. It is tightly coupled with the `ship` and `object` subsystems.

### 1. The `ai_info` Structure (`ai.h`)
Each AI ship is associated with an `ai_info` struct, stored in the global `Ai_info[]` array.
*   **Mode & Submode:** Defines the current high-level behavior (e.g., `AIM_CHASE`) and granular tactic (e.g., `SM_EVADE_SQUIGGLE`).
*   **Targeting:** Tracks `target_objnum`, `attacker_objnum`, and `targeted_subsys`.
*   **Sensors:** Stores state for stealth detection (`stealth_last_visible_stamp`) and aspect lock timing.
*   **Pathfinding:** Manages internal path data (`pnode`) for navigating around large ships or through waypoints.

### 2. AI Modes & Submodes
Behaviors are organized into a hierarchy:
*   **Modes (`AIM_*`):** The primary intent (Chase, Guard, Dock, Waypoints, Strafe, Play Dead).
*   **Submodes (`SM_*` / `AIS_*`):** The specific implementation of that intent. For example, a ship in `AIM_CHASE` may cycle through `SM_ATTACK`, `SM_EVADE`, and `SM_GET_BEHIND` based on the tactical situation.

### 3. AI Goals & Objectives (`aigoals.h`)
Mission logic interacts with the AI via a stack-based goal system.
*   **`ai_goal`:** Represents an objective like "Destroy Ship X" or "Guard Wing Y".
*   **Assignment:** Goals are assigned via S-Expressions (SEXP) in the mission file or dynamic player orders.
*   **Priority:** Goals have a priority (0-100). The AI always attempts to fulfill the highest-priority achievable goal.
*   **Typestates:** The system uses specific context tokens (e.g., `DeferredGeometryCtx`) to ensure logic flows are followed correctly in complex sequences like docking.

### 4. Profiles & Difficulty (`ai_profiles.h`)
AI performance is scaled by the game's difficulty setting (`NUM_SKILL_LEVELS`).
*   **`ai_profile_t`:** Loaded from `ai_profiles.tbl`. It defines global "fixes" for legacy bugs and sets difficulty-related constants (e.g., `max_attackers`, `predict_position_delay`).
*   **`ai_class`:** Loaded from `ai.tbl`. Defines specific "personalities" (e.g., Rookie, Ace) which modify accuracy, evasion, and aggressiveness.

### 5. Turret AI (`aiturret.cpp`)
Sub-objects like turrets have their own autonomous logic.
*   **Targeting:** Turrets scan for enemies independently of the parent ship's target.
*   **Prediction:** Calculates lead-pursuit vectors for projectile weapons based on target velocity and distance.
*   **Constraints:** Respects fire-cones and line-of-sight (LOS) defined in the model.

## Design Patterns

### The Processing Loop
`ai_process()` is called once per frame per AI ship. It follows this internal flow:
1.  **Goal Processing:** Check if the current goal is satisfied or achievable.
2.  **Mode Update:** Select the best `submode` based on target distance and threat.
3.  **Movement:** Calculate desired velocity and rotation, which are then passed to the `physics` subsystem.
4.  **Weapons:** Determine when to fire primary/secondary banks or deploy countermeasures.

### Lua Integration (`ailua.h`)
The AI can be extended or overridden via Lua.
*   `AIM_LUA`: Allows a script to take total control of a ship's movement and targeting.
*   Hooks: Scripting can be triggered on events like "On Turret Fired".

## Interaction with Vulkan

The AI subsystem impacts Vulkan rendering primarily through:
*   **Debug Visualization:** When `Ai_render_debug_flag` is set, the AI system renders paths, target vectors, and goal points using `gr_render_primitives` or `debug_sphere::add`.
*   **Transformation Updates:** AI-driven submodel rotations (like turrets or spinning radars) update the `matrix4` data in the Vulkan uniform buffers.

## Common Pitfalls

*   **Goal Signature Stale:** Never store a goal pointer across frames. Use the `signature` to verify the goal still exists.
*   **Submode Bloat:** When adding new tactics, ensure they are compatible with existing modes. Adding a submode to `AIM_CHASE` that expects docking data will cause crashes.
*   **Skill Level Scaling:** Always use the `NUM_SKILL_LEVELS` array when defining new AI constants to ensure modders can tune the behavior for different players.

## Documentation Note
The AI system is deeply intertwined with the `code/parse` subsystem. For specific behavior of `ai_profiles.tbl` or `ai.tbl`, refer to the technical Wiki.
