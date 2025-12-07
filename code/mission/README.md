# Mission & Campaign Subsystem — `code/mission`

This subsystem manages the lifecycle of a mission, from parsing the `.fs2` mission files to orchestrating the overall campaign flow, goal evaluation, and event logging.

## Architecture

The mission subsystem acts as the high-level director of the engine, coordinating between the filesystem, AI, objects, and the renderer.

### 1. Mission Parsing (`missionparse.h`)
The engine uses a "Parse Object" pattern to bridge the gap between static file data and active game entities.
*   **`The_mission`**: A global struct containing the current mission's metadata (name, author, skybox, lighting, etc.).
*   **`p_object` (Parse Object)**: A temporary template used during loading. It stores the properties of a ship or wing (position, orientation, AI goals) before they are "born" into the mission.
*   **Arrival/Departure**: Manages how and when ships enter or leave the battlefield (e.g., `ArrivalLocation::NEAR_SHIP`, `DepartureLocation::TO_DOCK_BAY`).

### 2. Campaign Management (`missioncampaign.h`)
Handles the progression between individual missions.
*   **`Campaign`**: A global struct tracking the state of the current campaign.
*   **Branching**: Missions can have multiple exit points determined by SEXP formulas.
*   **Persistence**: Stores `persistent_variables` and `persistent_containers` that carry state (like wingman survival or choices made) across missions.

### 3. Goals & Events (`missiongoals.h`)
The logical heart of a mission's win/loss conditions.
*   **`mission_goal`**: Represents high-level objectives (Primary, Secondary, Bonus). Status is tracked as `GOAL_INCOMPLETE`, `GOAL_COMPLETE`, or `GOAL_FAILED`.
*   **`mission_event`**: Finer-grained triggers used for mid-mission logic, messaging, and directing the narrative flow.
*   **Evaluation**: Goals and events are evaluated every frame using the SEXP (S-Expression) engine.

### 4. Briefing & Communication (`missionbriefcommon.h`, `missionmessage.h`)
*   **Briefings**: Orchestrates the multi-stage pre-mission briefing, including text, voice, and iconic animations.
*   **Messages**: Manages in-mission character chatter, including persona-based voice acting and HUD message display.

## Design Patterns

### The "Parse Object" to "Object" Transition
Missions don't load all ships into memory at once. Instead:
1.  `parse_main()` reads the `.fs2` file and populates `Parse_objects`.
2.  `mission_maybe_make_ship_arrive()` is called when an arrival cue (SEXP) is satisfied.
3.  The engine creates a real `object` and `ship` from the `p_object` template.

### Typestates in Campaigns
Campaign flow is a sequence of state transitions:
`Init -> Briefing -> Mission -> Debriefing -> Campaign Save -> Next Mission`.

## Interaction with Vulkan

The mission subsystem provides the environment data that the Vulkan backend renders:
*   **Skybox & Environment**: `The_mission.skybox_model` and `The_mission.envmap_name` are used by `VulkanRenderer` to set up the background and reflection probes.
*   **Global Lighting**: `The_mission.ambient_light_level` and `The_mission.lighting_profile_name` populate global uniform buffers.
*   **Volumetrics**: Mission-specific `volumetric_nebula` settings are consumed by the deferred lighting pass in the Vulkan backend.

## Common Pitfalls

*   **SEXP Complexity**: Deeply nested SEXP trees in mission goals can become performance bottlenecks if evaluated every frame. Use `interval` or `timestamp` to stagger evaluations.
*   **Net Signatures**: In multiplayer, `net_signature` must be perfectly synced between the mission file and all clients to avoid "ghost" objects.
*   **Persistence Leaks**: Adding too many persistent variables to a campaign can bloat the pilot save file (`.plr`).

## Wiki Recommendation
The `.fs2` file format and the behavior of specific mission flags are extensive. A dedicated **Mission Design Wiki** is recommended for documenting the interaction between FRED (the editor) and these code-level structures.
