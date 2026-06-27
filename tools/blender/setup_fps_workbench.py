#!/usr/bin/env python3
# PULSE FPS viewmodel workbench builder.
#
# Builds a Blender scene that pairs the existing bumstrum FPS ARMS rig (rights-cleared,
# animated) with a Quaternius ANIMATED gun (its own Fire/Reload rig), so the grip/pose can
# be fine-tuned by hand and re-exported to glTF for the engine viewmodel path.
#
# What it does:
#   - starts an empty scene
#   - imports the arms rig (glTF)         -> collection "ARMS (...)"
#   - hides the arms rig's ORIGINAL gun mesh (kept as a size/placement reference)
#   - imports the Quaternius gun (FBX)     -> collection "GUN (... - animated)"
#   - parents the Quaternius gun to the arms' HAND bone (keep-transform) so it rides the
#     hand during the arm animation, while its own armature still animates slide/mag/trigger
#   - prints + writes a report (object names, armature bones, bbox sizes, actions)
#   - saves the .blend
#
# Run it either way:
#   GUI (recommended, opens ready to tune):  blender --python tools/blender/setup_fps_workbench.py
#   or paste into Blender's Scripting tab and Run.
#
# Retarget to another pairing by editing ARMS_GLTF / GUN_FILE / OUT_BLEND below (e.g. the
# two-handed fps_ak_animated arms with Rifle.fbx).

import bpy
import os
from mathutils import Vector


def project_root():
    try:
        here = os.path.dirname(os.path.abspath(__file__))
        return os.path.abspath(os.path.join(here, "..", "..")).replace("\\", "/")
    except Exception:
        return "C:/Users/rq27/Pulse"


ROOT = project_root()
# --- the pairing to build (edit these to make other workbenches) ----------------------
# Look comes from the detailed Modular Sci-Fi guns (glTF, cohesive with the Quaternius world);
# animation comes from the bumstrum ARM rig the gun is parented to. Static gun mesh is fine - the
# arm motion + engine recoil/sway carry the feel.
ARMS_GLTF = ROOT + "/assets/bumstrum/fps_pistol_animated/scene.gltf"           # one-handed arms
GUN_FILE  = ROOT + "/assets/quaternius/Animated FPS Guns/Animated FPS Guns - Jun 2018/FBX/Pistol.fbx"
OUT_BLEND = ROOT + "/tools/blender/fps_workbench_pistol.blend"
# --------------------------------------------------------------------------------------

_log = []
def P(msg):
    _log.append(str(msg))
    print(str(msg))


def new_collection(name):
    c = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(c)
    return c


def move_to(coll, objs):
    for o in objs:
        for c in list(o.users_collection):
            c.objects.unlink(o)
        coll.objects.link(o)


def import_objects(path):
    before = set(bpy.data.objects)
    low = path.lower()
    if low.endswith(".gltf") or low.endswith(".glb"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif low.endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=True)
    else:
        raise RuntimeError("unsupported import: " + path)
    return [o for o in bpy.data.objects if o not in before]


def bbox_size(objs):
    mn = Vector((1e9, 1e9, 1e9))
    mx = Vector((-1e9, -1e9, -1e9))
    found = False
    for o in objs:
        if o.type != 'MESH':
            continue
        found = True
        for corner in o.bound_box:
            w = o.matrix_world @ Vector(corner)
            for i in range(3):
                mn[i] = min(mn[i], w[i])
                mx[i] = max(mx[i], w[i])
    return (mx - mn) if found else Vector((0, 0, 0))


def bbox_minmax(objs):
    mn = Vector((1e9, 1e9, 1e9))
    mx = Vector((-1e9, -1e9, -1e9))
    for o in objs:
        if o.type != 'MESH':
            continue
        for corner in o.bound_box:
            w = o.matrix_world @ Vector(corner)
            for i in range(3):
                mn[i] = min(mn[i], w[i])
                mx[i] = max(mx[i], w[i])
    return mn, mx


def find_hand_bone(arm):
    if not arm or arm.type != 'ARMATURE':
        return None
    names = [b.name for b in arm.data.bones]
    low = [n.lower() for n in names]
    # prefer an explicit RIGHT hand/wrist, then any hand/wrist/palm
    for pat in ("hand_r", "r_hand", "right_hand", "hand.r", "wrist_r", "r_wrist",
                "righthand", "hand", "wrist", "palm"):
        for n, l in zip(names, low):
            if pat in l:
                return n
    return None


# ---- start clean ---------------------------------------------------------------------
bpy.ops.wm.read_homefile(use_empty=True)
P("PULSE FPS workbench")
P("root: " + ROOT)

# ---- arms ----------------------------------------------------------------------------
P("\n[arms] " + ARMS_GLTF)
arms_objs = import_objects(ARMS_GLTF)
arms_coll = new_collection("ARMS (bumstrum)")
move_to(arms_coll, arms_objs)
arms_arm = next((o for o in arms_objs if o.type == 'ARMATURE'), None)
for o in arms_objs:
    P("  obj: %s [%s]" % (o.name, o.type))
if arms_arm:
    P("  armature '%s' bones=%d" % (arms_arm.name, len(arms_arm.data.bones)))

# Move the arms rig's ORIGINAL gun parts into a toggleable REFERENCE layer (left VISIBLE on
# purpose: it is correctly held, so it is the target you match the new gun's grip/size to; hide
# this collection once the new gun is seated). Arm/hand meshes stay in ARMS. Proxy helpers
# (Icosphere etc.) go to a hidden helpers layer.
gun_kw = ("gun", "pistol", "weapon", "revolver", "rifle", "smg", "slide", "slidecatch", "barrel",
          "trigger", "mag", "clip", "base", "lid", "muzzle", "bullet", "hammer", "cylinder",
          "grip", "stock", "sight", "scope", "ammo")
arm_kw = ("arm", "hand", "wrist", "sleeve", "glove", "finger", "forearm", "palm")
ref_coll = new_collection("ORIGINAL GUN (reference - match to this, then hide)")
help_coll = new_collection("HELPERS (hidden)")
moved_ref, moved_help = [], []
for o in list(arms_objs):
    if o.type != 'MESH':
        continue
    n = o.name.lower()
    if "icosphere" in n or "sphere" in n or "proxy" in n:
        move_to(help_coll, [o]); o.hide_set(True); o.hide_render = True; moved_help.append(o.name)
    elif any(k in n for k in gun_kw) and not any(k in n for k in arm_kw):
        move_to(ref_coll, [o]); moved_ref.append(o.name)        # kept visible as the reference
P("  original-gun parts -> reference layer: " + (", ".join(moved_ref) if moved_ref else "(none matched)"))
P("  helpers hidden: " + (", ".join(moved_help) if moved_help else "(none)"))

# ---- gun -----------------------------------------------------------------------------
P("\n[gun] " + GUN_FILE)
gun_objs = import_objects(GUN_FILE)
gun_coll = new_collection("GUN (Quaternius Sci-Fi)")
move_to(gun_coll, gun_objs)
gun_arm = next((o for o in gun_objs if o.type == 'ARMATURE'), None)
for o in gun_objs:
    P("  obj: %s [%s]" % (o.name, o.type))
P("  actions in file: " + ", ".join(sorted(a.name for a in bpy.data.actions)))

arms_dim = bbox_size(arms_objs)
gun_dim = bbox_size(gun_objs)
P("  arms bbox: [%.3f, %.3f, %.3f]" % (arms_dim.x, arms_dim.y, arms_dim.z))
P("  gun  bbox: [%.3f, %.3f, %.3f]" % (gun_dim.x, gun_dim.y, gun_dim.z))

# ---- rough-fit the gun to the hand + parent ------------------------------------------
gun_root = gun_arm if gun_arm else (gun_objs[0] if gun_objs else None)
hand = find_hand_bone(arms_arm)
P("\n[fit] hand bone: %s   gun root: %s" % (str(hand), gun_root.name if gun_root else None))

if gun_root and arms_arm and hand:
    longest = max(gun_dim.x, gun_dim.y, gun_dim.z, 1e-6)
    # Best starting fit: match the new gun's SIZE + POSITION to the reference gun (which is already
    # correctly held), so it lands roughly in the grip and you only refine rotation/offset. Falls
    # back to an arms-relative size + the wrist position if there is no reference gun.
    ref_mn, ref_mx = bbox_minmax(list(ref_coll.objects))
    ref_size = ref_mx - ref_mn
    have_ref = ref_size.length > 1e-3
    if have_ref:
        s = max(ref_size.x, ref_size.y, ref_size.z) / longest
    else:
        s = (max(arms_dim.x, arms_dim.y, arms_dim.z, 1e-6) * 0.18) / longest
    gun_root.scale = (gun_root.scale.x * s, gun_root.scale.y * s, gun_root.scale.z * s)
    if have_ref:
        gun_root.location = (ref_mn + ref_mx) * 0.5          # start overlapping the held reference gun
    else:
        hb = arms_arm.pose.bones.get(hand)
        if hb:
            gun_root.location = arms_arm.matrix_world @ hb.head
    bpy.context.view_layer.update()
    P("  fit to reference gun: %s (scale x%.4f)" % (have_ref, s))
    # Parent to the bone, keeping the current world transform (Ctrl+P -> Bone, Keep Transform).
    for o in bpy.context.selected_objects:
        o.select_set(False)
    gun_root.select_set(True)
    arms_arm.select_set(True)
    bpy.context.view_layer.objects.active = arms_arm
    try:
        arms_arm.data.bones.active = arms_arm.data.bones[hand]
        bpy.ops.object.parent_set(type='BONE', keep_transform=True)
        P("  parented gun to bone '%s' (keep-transform). Applied starting scale x%.4f." % (hand, s))
    except Exception as e:
        P("  parent failed (%s) - parent the gun to the hand bone manually (Ctrl+P > Bone)." % e)
else:
    P("  NOT auto-parented (no hand bone or gun root found) - parent manually.")

# ---- clean viewport: hide rig control widgets ----------------------------------------
# The bumstrum rig assigns an icosphere as a CUSTOM BONE SHAPE on its IK/control bones, so the
# armature draws a sphere blob at each control around the hand (they are rig handles, not the
# model, and show even when the source mesh is hidden). Switch to thin stick bones so you see a
# clean arms+gun. Re-enable per armature (Object Data > Viewport Display) if you want the controls.
for a in (arms_arm, gun_arm):
    if a and a.type == 'ARMATURE':
        a.data.display_type = 'STICK'
        a.data.show_bone_custom_shapes = False
        a.show_in_front = False
P("\n[clean] arm/gun armatures set to STICK display, custom bone shapes off (no more sphere blobs)")

# ---- a basic camera so the scene has a view ------------------------------------------
cam_data = bpy.data.cameras.new("FPS_Cam")
cam = bpy.data.objects.new("FPS_Cam", cam_data)
bpy.context.scene.collection.objects.link(cam)
center = Vector((0, 0, 0))
size = max(arms_dim.length, 0.5)
cam.location = center + Vector((0.0, -size * 1.6, size * 0.5))
cam.rotation_euler = (1.25, 0.0, 0.0)
bpy.context.scene.camera = cam

# ---- save ----------------------------------------------------------------------------
os.makedirs(os.path.dirname(OUT_BLEND), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=OUT_BLEND)
P("\n[saved] " + OUT_BLEND)

P("\nNEXT (fine-tune in the viewport):")
P("  1. Select the GUN root, tweak its Location/Rotation/Scale so the grip seats in the hand.")
P("  2. Scrub the arms' action + the gun's Fire/Reload actions (Action Editor) to check it holds")
P("     through the motion. Adjust the gun's bone-parent offset until it tracks the hand.")
P("  3. Delete the 'ORIGINAL GUN' collection once the new gun is seated.")
P("  4. Export: select arms armature + gun, File > Export > glTF 2.0 (with animations).")

try:
    with open(OUT_BLEND.replace(".blend", "_report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(_log) + "\n")
except Exception:
    pass
