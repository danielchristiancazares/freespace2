"""RenderDoc capture analysis script - run with renderdoccmd capture.rdc --python-script analyze_rdc.py"""

import renderdoc as rd
import os

def analyze_capture(controller):
    print("=" * 80)
    print("RenderDoc Capture Analysis")
    print("=" * 80)

    # Get API and driver info
    api_props = controller.GetAPIProperties()
    print(f"\nAPI: {api_props.pipelineType.name}")

    # Get list of all draw calls (actions)
    actions = controller.GetRootActions()

    total_draws = 0
    total_dispatches = 0

    def count_actions(action_list):
        nonlocal total_draws, total_dispatches
        for action in action_list:
            if action.flags & rd.ActionFlags.Drawcall:
                total_draws += 1
            if action.flags & rd.ActionFlags.Dispatch:
                total_dispatches += 1
            if len(action.children) > 0:
                count_actions(action.children)

    count_actions(actions)
    print(f"\nTotal draw calls: {total_draws}")
    print(f"Total dispatches: {total_dispatches}")

    # Get all textures
    textures = controller.GetTextures()
    print(f"\nTextures: {len(textures)}")

    # Categorize textures by type/usage
    render_targets = []
    depth_targets = []
    regular_textures = []

    for tex in textures:
        if tex.creationFlags & rd.TextureCategory.ColorTarget:
            render_targets.append(tex)
        elif tex.creationFlags & rd.TextureCategory.DepthTarget:
            depth_targets.append(tex)
        else:
            regular_textures.append(tex)

    print(f"  - Render targets: {len(render_targets)}")
    print(f"  - Depth targets: {len(depth_targets)}")
    print(f"  - Regular textures: {len(regular_textures)}")

    # List render targets with details
    if render_targets:
        print("\nRender Targets:")
        for tex in render_targets[:20]:  # Limit output
            fmt = str(tex.format.Name()) if hasattr(tex.format, 'Name') else str(tex.format)
            print(f"  [{tex.resourceId}] {tex.width}x{tex.height} {fmt}")

    # Get buffers
    buffers = controller.GetBuffers()
    print(f"\nBuffers: {len(buffers)}")

    # Print action tree structure (top-level markers/passes)
    print("\n" + "=" * 80)
    print("Action Structure (top-level passes):")
    print("=" * 80)

    def print_actions(action_list, depth=0):
        for action in action_list:
            indent = "  " * depth
            flags = []
            if action.flags & rd.ActionFlags.Drawcall:
                flags.append("DRAW")
            if action.flags & rd.ActionFlags.Dispatch:
                flags.append("DISPATCH")
            if action.flags & rd.ActionFlags.Clear:
                flags.append("CLEAR")
            if action.flags & rd.ActionFlags.PassBoundary:
                flags.append("PASS")
            if action.flags & rd.ActionFlags.SetMarker:
                flags.append("MARKER")
            if action.flags & rd.ActionFlags.PushMarker:
                flags.append("BEGIN")
            if action.flags & rd.ActionFlags.PopMarker:
                flags.append("END")

            flag_str = f" [{','.join(flags)}]" if flags else ""

            # Only print markers/passes at top levels, or all at deeper levels
            if depth < 2 or (action.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Clear)):
                name = action.customName if action.customName else f"EID {action.eventId}"
                if depth < 3:
                    print(f"{indent}{action.eventId}: {name}{flag_str}")

            if len(action.children) > 0 and depth < 3:
                print_actions(action.children, depth + 1)

    print_actions(actions)

    print("\n" + "=" * 80)
    print("Analysis complete")
    print("=" * 80)

def main():
    # When run via renderdoccmd, pyrenderdoc gives us the controller
    if 'pyrenderdoc' in dir():
        controller = pyrenderdoc.Replay().GetController()
        analyze_capture(controller)
    else:
        print("This script must be run via: renderdoccmd capture.rdc --python-script analyze_rdc.py")

if __name__ == "__main__":
    main()
