# Physics Simulation — `code/physics`

This subsystem provides the engine's motion simulation, handling linear and rotational physics for all dynamic objects. It translates control inputs (from players or AI) and external forces (impacts, shockwaves) into spatial updates.

## Architecture

The physics system operates on a per-object basis, primarily using the `physics_info` structure attached to each `object`.

### 1. Core Data Structures (`physics.h`)
*   **`physics_info`**: Stores the physical identity and state of an object.
    *   **Static Properties:** `mass`, `I_body_inv` (inverse moment of inertia tensor), and damping constants (`rotdamp`, `side_slip_time_const`).
    *   **Dynamic State:** `vel` (linear velocity), `rotvel` (rotational velocity), `speed`, and `acceleration`.
    *   **Control Inputs:** `desired_vel` and `desired_rotvel` act as the targets for the simulation's damping and acceleration logic.
*   **`control_info`**: A normalized representation (-1.0 to 1.0) of intent from the player or AI, covering 6 degrees of freedom (pitch, yaw, roll, and XYZ translation).

### 2. Simulation Logic (`physics.cpp`)
The main simulation loop uses a damped spring-mass model to achieve smooth, "space-like" movement while maintaining gameplay responsiveness.
*   **`physics_sim()`**: The primary entry point. Orchestrates the simulation of position and orientation.
*   **`physics_sim_vel()`**: Updates linear velocity. Handles acceleration towards `desired_vel` while respecting `max_vel` and damping.
*   **`physics_sim_rot()`**: Updates rotational velocity. Manages banking and turn rates.
*   **Newtonian Mode:** Support for `PF_NEWTONIAN_DAMP` allows for more realistic inertia-based movement compared to the default "Descent-style" physics.

### 3. State Management & Interpolation (`physics_state.h`)
To support smooth rendering and networked play, the system can "snapshot" physical states.
*   **`physics_snapshot`**: A lightweight struct containing position, orientation, and velocities.
*   **`physics_interpolate_snapshots()`**: Performs linear interpolation between two snapshots. This is critical for rendering smooth motion when the simulation tick rate differs from the display refresh rate.

### 4. External Forces
*   **Whacks:** `physics_apply_whack()` applies instantaneous impulses to objects, often from collisions or weapon impacts.
*   **Shockwaves:** `physics_apply_shock()` simulates the pressure wave from large explosions, affecting multiple objects in a radius.

## Interaction with Vulkan

The physics subsystem is the authoritative source of the data that Vulkan renders:
*   **Interpolated Transforms:** The renderer typically uses `physics_interpolate_snapshots` to get the sub-frame position and orientation for drawing.
*   **Uniform Buffers:** These interpolated values are packed into `matrix4` format and uploaded to the GPU via the Vulkan uniform ring buffers (`VulkanRingBuffer`).
*   **Debug Spew:** Physics state (velocities, thrust vectors) is often visualized in the Vulkan backend when debug flags are enabled.

## Common Pitfalls

*   **Damping Inconsistencies:** Changing `rotdamp` or `side_slip_time_const` without understanding the underlying polynomial/exponential models can lead to jittery or unresponsive movement.
*   **Massive Forces:** Applying extremely large "whacks" can result in objects exceeding the `Highest_ever_object_index` or flying out of the valid world boundaries, potentially breaking collision detection.
*   **Fixed-Point Legacy:** Be mindful of `fix` (32-bit fixed point) types when interacting with older parts of the simulation; most modern additions use `float`.
