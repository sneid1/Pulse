# merge_kaykit.py - assemble a KayKit character (mesh + Rig_Medium) plus selected clips from the
# KayKit animation library into ONE multi-clip glTF with role-named clips the engine state machine
# expects (idle/walk/run/walk_back/strafe_left/strafe_right/cast/cast_heavy/channel/lunge/hit/
# hit_heavy/death). All anim files share Rig_Medium, so actions transfer by bone name. GLTF_SEPARATE
# export also externalizes the embedded gradient-atlas texture (fixes the embedded-texture gotcha).
# ASCII only.
# Usage: blender -b --python merge_kaykit.py -- <out.gltf> <base.glb> <anim_dir> [role clip stem]...
import bpy, sys, os

argv = sys.argv
argv = argv[argv.index("--") + 1:] if "--" in argv else []
out, base, anim_dir = argv[0], argv[1], argv[2]
rest = argv[3:]
triples = [(rest[i], rest[i + 1], rest[i + 2]) for i in range(0, len(rest) - 2, 3)]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)

bpy.ops.import_scene.gltf(filepath=base)
base_objs = list(bpy.data.objects)
arm = next((o for o in base_objs if o.type == 'ARMATURE'), None)
assert arm, "base has no armature"
# the base import may bring a bind/T-pose action; drop any so only role clips export
for a in list(bpy.data.actions):
    bpy.data.actions.remove(a)
# the exporter's ACTIONS mode only emits animations for armatures that HAVE animation_data,
# so give the base rig an animation_data block to anchor the lifted clips to.
arm.animation_data_create()

# group requested clips by source file stem
groups = {}
for role, clip, stem in triples:
    groups.setdefault(stem, []).append((role, clip))

all_kept = []
for stem, items in groups.items():
    path = os.path.join(anim_dir, "Rig_Medium_" + stem + ".glb")
    before_objs = set(bpy.data.objects)
    before_acts = set(bpy.data.actions)
    bpy.ops.import_scene.gltf(filepath=path)
    new_objs = [o for o in bpy.data.objects if o not in before_objs]
    new_acts = [a for a in bpy.data.actions if a not in before_acts]
    kept = set()
    for role, clip in items:
        act = next((a for a in new_acts if a.name == clip), None)
        if act is None:
            print("MISS clip '%s' in %s" % (clip, stem)); continue
        act.name = role; act.use_fake_user = True; kept.add(act)
        all_kept.append(act)
        print("KAYKIT_CLIP %-12s <- %s/%s" % (role, stem, clip))
    for a in new_acts:
        if a not in kept:
            bpy.data.actions.remove(a)
    for o in new_objs:
        bpy.data.objects.remove(o, do_unlink=True)

# Push each clip to its own NLA track so the exporter emits ALL of them as named animations
# (ACTIONS mode silently drops whichever action is the armature's "active" one).
for act in all_kept:
    tr = arm.animation_data.nla_tracks.new()
    tr.name = act.name
    tr.strips.new(act.name, 0, act)
bpy.ops.object.select_all(action='DESELECT')
for o in base_objs:
    if o.name in bpy.data.objects:
        o.select_set(True)
bpy.context.view_layer.objects.active = arm
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.export_scene.gltf(filepath=out, export_format='GLTF_SEPARATE', use_selection=True,
                          export_skins=True, export_animations=True,
                          export_animation_mode='NLA_TRACKS', export_yup=True)
print("KAYKIT_EXPORTED", len(triples), "clips ->", out)
