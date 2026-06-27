# Author the PULSE first-person viewmodel from FPS_Arms.blend (+ AK-47 OBJ).
# The arms rig is IK-driven (L/R.HandIk hand targets, L/R.ForearmIK elbow poles),
# so we pose by MOVING the IK control bones into a forward two-handed hold, place
# the weapon between the hands, then render previews (calib/render) or bake +
# export the 3 shared-space OBJs (export).
#
#   blender.exe -b assets/FPS_Arms/Fps_Arms.blend --python tools/dev/build_viewmodel.py -- calib
#   blender.exe -b assets/FPS_Arms/Fps_Arms.blend --python tools/dev/build_viewmodel.py -- render
#   blender.exe -b assets/FPS_Arms/Fps_Arms.blend --python tools/dev/build_viewmodel.py -- export
import bpy, sys, math, mathutils
V = mathutils.Vector
rad = math.radians

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
MODE = argv[0] if argv else 'render'
OUTDIR = 'C:/Users/rq27/Pulse/tools/dev/'
AK_OBJ = 'C:/Users/rq27/Pulse/assets/Untextured_3D_Weapons/OBJ/AK-47.obj'
MODELS = 'C:/Users/rq27/Pulse/assets/models/'

# IK control placements (armature space: +X right, +Y forward, +Z up).
# Right hand = grip/trigger (near, lower); left hand = foregrip (forward).
CONTROLS = {
    'R.HandIk':    (0.12, 0.36, -0.34),    # right/trigger hand: back, low, right
    'L.HandIk':    (-0.15, 0.60, -0.30),   # left/foregrip hand: forward, low, left
    'R.ForearmIK': (0.48, -0.12, -0.78),   # elbow poles low + back -> forearms from below
    'L.ForearmIK': (-0.48, -0.12, -0.78),
}
# Optional FK tweak of the wrists after IK (degrees, bone-local XYZ).
HAND_FK = {'L.hand': (0, 0, 0), 'R.hand': (0, 0, 0)}

# AK OBJ is already barrel-along-Y, up-along-Z, thin in X: no rotation needed
# (z flip 180 only if the muzzle ends up facing the camera, checked in-engine).
AK_ROT_EULER = (0.0, 0.0, 0.0)
AK_TARGET_LEN = 0.62
AK_FWD_OFFSET = (0.02, 0.0, -0.03)

arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')


def reset_pose():
    for pb in arm.pose.bones:
        pb.rotation_mode = 'XYZ'
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)
    bpy.context.view_layer.update()


def place_control(name, loc):
    pb = arm.pose.bones.get(name)
    if pb:
        pb.matrix = mathutils.Matrix.Translation(V(loc))
    bpy.context.view_layer.update()


def apply_pose(controls, hand_fk=None):
    for name, loc in controls.items():
        place_control(name, loc)
    if hand_fk:
        for name, (rx, ry, rz) in hand_fk.items():
            pb = arm.pose.bones.get(name)
            if pb:
                pb.rotation_mode = 'XYZ'
                pb.rotation_euler = (rad(rx), rad(ry), rad(rz))
        bpy.context.view_layer.update()


FINGERS = ['pinky1', 'pinky1.001', 'pinky2', 'pinky2.001',
           'ring1', 'ring1.001', 'ring2', 'ring2.001',
           'middle1', 'middle1.001', 'middle2', 'middle2.001',
           'index1', 'index1.001', 'index2', 'index2.001']
CURL_AXIS = 0   # 0=X 1=Y 2=Z (bone-local)
CURL_DEG = -50
THUMB_DEG = -28


def curl_fingers():
    for side in ('L', 'R'):
        for f in FINGERS:
            pb = arm.pose.bones.get(f'{side}.{f}')
            if pb:
                pb.rotation_mode = 'XYZ'
                e = [0, 0, 0]
                e[CURL_AXIS] = rad(CURL_DEG)
                pb.rotation_euler = e
        for t in ('thumb1', 'thumb2', 'thumb3'):
            pb = arm.pose.bones.get(f'{side}.{t}')
            if pb:
                pb.rotation_mode = 'XYZ'
                e = [0, 0, 0]
                e[CURL_AXIS] = rad(THUMB_DEG)
                pb.rotation_euler = e
    bpy.context.view_layer.update()


def hands_mid():
    L = arm.pose.bones['L.hand'].head
    R = arm.pose.bones['R.hand'].head
    return L.copy(), R.copy(), (L + R) * 0.5


def import_ak(mid):
    before = set(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=AK_OBJ)
    objs = [o for o in bpy.data.objects if o not in before and o.type == 'MESH']
    for o in objs:
        bpy.ops.object.select_all(action='DESELECT')
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.ops.object.origin_set(type='ORIGIN_GEOMETRY', center='BOUNDS')
    longest = max(max(o.dimensions) for o in objs) or 1.0
    s = AK_TARGET_LEN / longest
    for o in objs:
        o.scale = (s, s, s)
        o.rotation_euler = AK_ROT_EULER
        o.location = mid + V(AK_FWD_OFFSET)
    bpy.context.view_layer.update()
    return objs


def setup_workbench():
    sc = bpy.context.scene
    sc.render.engine = 'BLENDER_WORKBENCH'
    sh = sc.display.shading
    sh.light = 'STUDIO'; sh.color_type = 'SINGLE'
    sh.single_color = (0.55, 0.56, 0.6); sh.show_cavity = True; sh.cavity_type = 'BOTH'
    sc.render.resolution_x = 800; sc.render.resolution_y = 800
    sc.render.image_settings.file_format = 'PNG'
    if not sc.camera:
        cam = bpy.data.objects.new('pc', bpy.data.cameras.new('pc'))
        sc.collection.objects.link(cam); sc.camera = cam
    return sc, sc.camera


def render_to(sc, cam, path, eye, target, angle=72):
    cam.data.angle = rad(angle)
    cam.location = eye
    cam.rotation_euler = (V(target) - V(eye)).to_track_quat('-Z', 'Y').to_euler()
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print('RENDERED', path)


if MODE == 'calib':
    sc, cam = setup_workbench()
    # Vary forward reach (fy) and convergence to find a natural hold.
    cands = {
        'a': {'R.HandIk': (0.05, 0.40, -0.24), 'L.HandIk': (-0.11, 0.60, -0.19),
              'R.ForearmIK': (0.42, 0.02, -0.55), 'L.ForearmIK': (-0.42, 0.02, -0.55)},
        'b': {'R.HandIk': (0.08, 0.50, -0.20), 'L.HandIk': (-0.10, 0.70, -0.16),
              'R.ForearmIK': (0.42, 0.05, -0.55), 'L.ForearmIK': (-0.42, 0.05, -0.55)},
        'c': {'R.HandIk': (0.02, 0.34, -0.30), 'L.HandIk': (-0.14, 0.52, -0.24),
              'R.ForearmIK': (0.40, -0.05, -0.55), 'L.ForearmIK': (-0.40, -0.05, -0.55)},
        'd': {'R.HandIk': (0.06, 0.45, -0.16), 'L.HandIk': (-0.08, 0.66, -0.10),
              'R.ForearmIK': (0.45, 0.10, -0.50), 'L.ForearmIK': (-0.45, 0.10, -0.50)},
    }
    for label, ctrl in cands.items():
        reset_pose()
        apply_pose(ctrl)
        L, R, mid = hands_mid()
        print(f'CAND {label}: L={tuple(round(c,2) for c in L)} R={tuple(round(c,2) for c in R)} mid={tuple(round(c,2) for c in mid)}')
        render_to(sc, cam, OUTDIR + f'vm_c{label}.png', (0.0, -0.7, 0.05), (0, 1, -0.03))
        render_to(sc, cam, OUTDIR + f'vm_c{label}_34.png', mid + V((0.7, -0.8, 0.55)), mid)
    print('CALIB_OK')

elif MODE in ('render', 'export'):
    reset_pose()
    apply_pose(CONTROLS, HAND_FK)
    curl_fingers()
    L, R, mid = hands_mid()
    print('HANDS L=', tuple(round(c, 3) for c in L), 'R=', tuple(round(c, 3) for c in R))
    ak = import_ak(mid)

    if MODE == 'render':
        sc, cam = setup_workbench()
        # Side-on (see the gun + grip silhouette), and a viewmodel-style 3/4 from
        # behind-left looking forward-right (how the game frames the weapon).
        render_to(sc, cam, OUTDIR + 'vm_side.png', mid + V((1.3, 0.05, 0.05)), mid, angle=45)
        render_to(sc, cam, OUTDIR + 'vm_vm.png', mid + V((-0.55, -0.7, 0.35)), mid + V((0.25, 0.4, -0.05)), angle=60)
        render_to(sc, cam, OUTDIR + 'vm_fps.png', (0.0, -0.7, 0.05), (0, 1, -0.03))
        print('RENDER_OK')
    else:
        deps = bpy.context.evaluated_depsgraph_get()
        # Keep the model anchored at the Blender origin (= the eye/neck): a real FPS
        # viewmodel emanates from behind the camera, so weaponXf places this anchor
        # at the camera and the arms reach forward to the gun (shoulders clip off the
        # bottom). Orient into the engine's cameraSpace (+X right, +Y up, +Z into the
        # screen): muzzle (Blender +Y) -> +Z (points away), up (Blender +Z) -> +Y.
        ENGINE_FIX = (mathutils.Matrix.Rotation(rad(180), 4, 'Y')
                      @ mathutils.Matrix.Rotation(rad(-90), 4, 'X'))

        def export_group(objs, out_path):
            made = []
            for o in objs:
                me = bpy.data.meshes.new_from_object(o.evaluated_get(deps))
                no = bpy.data.objects.new(o.name + '_baked', me)
                no.matrix_world = ENGINE_FIX @ o.matrix_world
                bpy.context.scene.collection.objects.link(no)
                made.append(no)
            bpy.ops.object.select_all(action='DESELECT')
            for o in made:
                o.select_set(True)
            bpy.context.view_layer.objects.active = made[0]
            # Identity remap (output == Blender coords) so ENGINE_FIX is the sole
            # orientation control and we don't double-rotate.
            bpy.ops.wm.obj_export(filepath=out_path, export_selected_objects=True,
                                  export_materials=True, export_triangulated_mesh=True,
                                  forward_axis='NEGATIVE_Z', up_axis='Y')
            for o in made:
                bpy.data.objects.remove(o, do_unlink=True)
            print('EXPORTED', out_path)

        export_group([bpy.data.objects[n] for n in ('LeftHand.001', 'LeftSleeve.001')],
                     MODELS + 'pulse_left_hand_viewmodel.obj')
        export_group([bpy.data.objects[n] for n in ('RightHand.001', 'RightSleeve.001')],
                     MODELS + 'pulse_right_hand_viewmodel.obj')
        export_group(ak, MODELS + 'pulse_ak47_viewmodel.obj')
        print('EXPORT_OK')
