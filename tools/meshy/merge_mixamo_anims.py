# merge_mixamo_anims.py - combine a set of single-clip Mixamo FBX exports into ONE
# multi-clip glTF (.gltf + .bin + external PNGs) that the engine's AnimatedGltf loader
# can read, with each animation named by its gameplay role (idle/walk/run/attack/hit/death).
#
# Mixamo exports one action per download (always named "mixamo.com") and, with "With Skin",
# re-exports the same rigged mesh each time. We import the FIRST FBX as the skinned base,
# then from every other FBX we lift ONLY its newly-created action onto that base armature,
# rename it to its role, and export every action as a separate glTF animation.
#
# IMPORTANT: download every clip "With Skin" FROM THE SAME rigged character so all FBX share
# one skeleton scale, and tick "In Place" for walk/run (the engine drives enemy position;
# root motion would walk the rig off its gameplay spot). ASCII only.
#
# Usage:
#   blender -b --python merge_mixamo_anims.py -- <out.gltf> <role1=clip1.fbx> [<role2=clip2.fbx> ...]
# Example:
#   ... -- assets/meshy/enemies/husk_anim/husk.gltf idle=dl/husk_idle.fbx walk=dl/husk_walk.fbx \
#         run=dl/husk_run.fbx attack=dl/husk_attack.fbx hit=dl/husk_hit.fbx death=dl/husk_death.fbx
import bpy, sys, os

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
if len(argv) < 2:
    print("MERGE_ERROR: need <out.gltf> and at least one role=clip.fbx")
    sys.exit(1)
out = argv[0]
pairs = []
for spec in argv[1:]:
    if "=" not in spec:
        print("MERGE_ERROR: expected role=path, got", spec)
        sys.exit(1)
    role, path = spec.split("=", 1)
    pairs.append((role.strip(), path.strip()))

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)

def import_one(path):
    before = set(bpy.data.objects)
    actions_before = set(bpy.data.actions)
    bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=True)
    new_objs = [o for o in bpy.data.objects if o not in before]
    new_acts = [a for a in bpy.data.actions if a not in actions_before]
    arm = next((o for o in new_objs if o.type == 'ARMATURE'), None)
    return new_objs, new_acts, arm

# 1) Base = first clip, kept with its mesh + armature.
base_role, base_path = pairs[0]
base_objs, base_acts, base_arm = import_one(base_path)
if base_arm is None:
    print("MERGE_ERROR: first FBX has no armature:", base_path)
    sys.exit(1)
if not base_acts:
    print("MERGE_ERROR: first FBX has no animation:", base_path)
    sys.exit(1)
base_acts[0].name = base_role
base_acts[0].use_fake_user = True
print("MERGE_CLIP %-8s <- %s" % (base_role, os.path.basename(base_path)))

# 2) Every other clip: lift its new action onto the base armature, drop its objects.
for role, path in pairs[1:]:
    new_objs, new_acts, _ = import_one(path)
    if new_acts:
        act = new_acts[0]
        act.name = role
        act.use_fake_user = True
        print("MERGE_CLIP %-8s <- %s" % (role, os.path.basename(path)))
    else:
        print("MERGE_WARN  no action in", path)
    for o in new_objs:
        bpy.data.objects.remove(o, do_unlink=True)

# 3) Export base mesh + armature with EVERY action as its own glTF animation.
bpy.ops.object.select_all(action='DESELECT')
for o in base_objs:
    if o.name in bpy.data.objects:
        o.select_set(True)
bpy.context.view_layer.objects.active = base_arm

os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.gltf(
    filepath=out,
    export_format='GLTF_SEPARATE',
    use_selection=True,
    export_skins=True,
    export_animations=True,
    export_animation_mode='ACTIONS',
    export_yup=True,
)
print("MERGE_EXPORTED %d clips -> %s" % (len(pairs), out))
