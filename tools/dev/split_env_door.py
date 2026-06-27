# Split the "Modular Environment Parts" sci-fi door wall section into two GLBs so the Pulse door
# system can show the sliding door PANELS only while a doorway is sealed, and reveal the opening
# (frame + control + pipes stay) when it opens. One-shot headless task:
#   blender.exe --background --python tools/dev/split_env_door.py
import bpy

KIT = r"C:/Users/rq27/Pulse/assets/external/sketchfab_scifi/modular_env_parts"

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=KIT + "/scene.gltf")

meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
# Door panels = objects named "Door*" but NOT the static "DoorTrack" rail.
door = [o for o in meshes if o.name.startswith("Door") and not o.name.startswith("DoorTrack")]
frame = [o for o in meshes if o not in door]


def export(objs, path):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    if objs:
        bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", use_selection=True,
                              export_yup=True, export_apply=True)


export(door, KIT + "/env_door.glb")
export(frame, KIT + "/env_doorframe.glb")
print("SPLIT_OK door=%d frame=%d" % (len(door), len(frame)))
print("door names:", [o.name for o in door])
