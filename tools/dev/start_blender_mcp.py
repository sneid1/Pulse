# Start the BlenderMCP bridge so the MCP client (Claude) can drive Blender.
#
# Enables the blender_mcp addon (its register() auto-starts a socket server on
# localhost:9876) and turns on the Poly Haven asset integration (no API key
# needed). Sketchfab and Hyper3D-Rodin need API keys set in the BlenderMCP
# sidebar panel (View3D > N > BlenderMCP) before use.
#
# Launch (keep the window open while authoring assets):
#   tools\blender\blender-5.1.2-windows-x64\blender.exe -P tools\dev\start_blender_mcp.py
#
# Must run in GUI mode, NOT --background: the addon services the socket from a
# bpy.app.timers callback that only fires while Blender's event loop runs.
import bpy


def _enable_addon():
    try:
        bpy.ops.preferences.addon_enable(module="blender_mcp")
        print("BLENDERMCP_ADDON_ENABLED")
    except Exception as exc:  # noqa: BLE001 - report and continue
        print("BLENDERMCP_ADDON_ENABLE_FAILED", exc)


def _enable_polyhaven():
    scene = getattr(bpy.context, "scene", None)
    if scene is None:
        print("BLENDERMCP_NO_SCENE")
        return
    try:
        scene.blendermcp_use_polyhaven = True
        print("BLENDERMCP_POLYHAVEN_ON")
    except Exception as exc:  # noqa: BLE001 - property may be absent if addon failed
        print("BLENDERMCP_POLYHAVEN_FAILED", exc)


_enable_addon()
_enable_polyhaven()
print("BLENDERMCP_STARTUP_DONE")
