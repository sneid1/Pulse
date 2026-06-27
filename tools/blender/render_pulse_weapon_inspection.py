import math
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "blender" / "pulse_ak47_hands_near_side.png"
OUT.parent.mkdir(parents=True, exist_ok=True)


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def show_only_model():
    hidden_names = {"matte display plinth", "rear shadow card"}
    for obj in bpy.context.scene.objects:
        if obj.name in hidden_names:
            obj.hide_render = True
            obj.hide_viewport = True
        if obj.type == "ARMATURE":
            obj.hide_render = True


def brighten_hands():
    for mat in bpy.data.materials:
        lower = mat.name.lower()
        if (
            "glove" not in lower
            and "sleeve" not in lower
            and "gunmetal" not in lower
            and "polymer" not in lower
            and "charcoal" not in lower
            and "rubber" not in lower
        ):
            continue
        mat.use_nodes = True
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if not bsdf:
            continue
        if "glove" in lower:
            bsdf.inputs["Base Color"].default_value = (0.24, 0.34, 0.52, 1.0)
        if "sleeve" in lower:
            bsdf.inputs["Base Color"].default_value = (0.04, 0.07, 0.12, 1.0)
        if "gunmetal" in lower:
            bsdf.inputs["Base Color"].default_value = (0.52, 0.56, 0.58, 1.0)
        if "polymer" in lower or "charcoal" in lower or "rubber" in lower:
            bsdf.inputs["Base Color"].default_value = (0.035, 0.045, 0.060, 1.0)
        bsdf.inputs["Roughness"].default_value = 0.62


def setup_camera_and_lights():
    bpy.context.scene.render.resolution_x = 1280
    bpy.context.scene.render.resolution_y = 720
    try:
        bpy.context.scene.render.engine = "BLENDER_EEVEE_NEXT"
        bpy.context.scene.eevee.taa_render_samples = 64
    except Exception:
        bpy.context.scene.render.engine = "BLENDER_WORKBENCH"
    try:
        bpy.context.scene.display.shading.light = "STUDIO"
        bpy.context.scene.display.shading.color_type = "MATERIAL"
        bpy.context.scene.display.shading.studio_light = "studio.exr"
    except Exception:
        pass

    bpy.context.scene.view_settings.view_transform = "Filmic"
    bpy.context.scene.view_settings.look = "Medium High Contrast"
    bpy.context.scene.view_settings.exposure = 0.15
    bpy.context.scene.view_settings.gamma = 1.0

    for obj in list(bpy.context.scene.objects):
        if obj.type == "LIGHT":
            bpy.data.objects.remove(obj, do_unlink=True)

    bpy.ops.object.light_add(type="AREA", location=(1.9, 3.4, 2.8))
    key = bpy.context.object
    key.name = "inspection large key"
    key.data.energy = 760
    key.data.size = 3.2

    bpy.ops.object.light_add(type="AREA", location=(-2.2, 2.4, 1.5))
    fill = bpy.context.object
    fill.name = "inspection low fill"
    fill.data.energy = 320
    fill.data.size = 4.8

    camera = bpy.context.scene.camera
    if camera is None:
        bpy.ops.object.camera_add()
        camera = bpy.context.object
        bpy.context.scene.camera = camera
    camera.location = (5.40, 7.25, 0.95)
    look_at(camera, (-0.48, 0.38, -0.12))
    camera.data.lens = 42
    camera.data.dof.use_dof = False


def main():
    show_only_model()
    brighten_hands()
    setup_camera_and_lights()
    bpy.context.scene.render.filepath = str(OUT)
    bpy.ops.render.render(write_still=True)
    print(f"PULSE_WEAPON_OTHER_SIDE_RENDER={OUT}")


if __name__ == "__main__":
    main()
