#!/usr/bin/env python3
"""Build PULSE enemy GLBs from per-enemy Mixamo FBX animation ZIPs.

Each *_anims.zip is expected to contain:
  - one skinned character FBX named after the enemy concept
  - several animation-only FBXs using the same Mixamo skeleton

The script keeps the skinned character mesh, imports animation FBXs as actions,
renames/copies them to the role names the game expects, and exports a single GLB.
"""

import os
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

import bpy


ROOT = Path.cwd()
ENEMY_ROOT = ROOT / "assets" / "meshy" / "enemy_inputs" / "meshy_generated_models" / "pulse_enemy_concepts"
OUT_DIR = ROOT / "assets" / "meshy" / "enemies" / "rigged_concepts"
WORK_DIR = ROOT / "build" / "mixamo_enemy_extract"


PACKS = [
    ("001_enemy_rusher_low_triangular_lunge_predator", "001_melee_anims.zip", "pulse_enemy_001_enemy_rusher_low_triangular_lunge_predator_animated.glb", "mutant"),
    ("003_enemy_tank_broad_frontal_shield_brute", "003_melee_anims.zip", "pulse_enemy_003_enemy_tank_broad_frontal_shield_brute_animated.glb", "mutant"),
    ("004_enemy_stalker_tall_flanking_pounce_hunter", "004_ranged_anims.zip", "pulse_enemy_004_enemy_stalker_tall_flanking_pounce_hunter_animated.glb", "standing"),
    ("006_enemy_foundry_warden_heavy_mech", "006_ranged_anims.zip", "pulse_enemy_006_enemy_foundry_warden_heavy_mech_animated.glb", "mutant"),
    ("008_enemy_reliquary_choir_elite", "008_ranged_anims.zip", "pulse_enemy_008_enemy_reliquary_choir_elite_animated.glb", "mutant"),
    ("009_enemy_fast_elite_rusher_speed_affix", "009_ranged_anims.zip", "pulse_enemy_009_enemy_fast_elite_rusher_speed_affix_animated.glb", "standing"),
    ("011_enemy_volatile_elite_bruiser_pressure_reactor", "011_ranged_anims.zip", "pulse_enemy_011_enemy_volatile_elite_bruiser_pressure_reactor_animated.glb", "mutant"),
    ("012_enemy_regen_elite_stalker_self_repair", "012_melee_anims.zip", "pulse_enemy_012_enemy_regen_elite_stalker_self_repair_animated.glb", "mutant"),
    ("013_enemy_obsidian_skeletal_husk_benchmark", "013_ranged_anims.zip", "pulse_enemy_013_enemy_obsidian_skeletal_husk_benchmark_animated.glb", "mutant"),
]


ROLE_CANDIDATES = {
    "mutant": {
        "idle": ["breathing idle", "mutant idle", "idle"],
        "walk": ["mutant walking", "walking"],
        "run": ["mutant run", "run"],
        "walk_back": ["mutant walking", "walking"],
        "strafe_left": ["mutant left turn 45", "left turn 45", "mutant walking"],
        "strafe_right": ["mutant right turn 45", "right turn 45", "mutant walking"],
        "lunge": ["mutant swiping", "mutant punch", "jump attack"],
        "cast": ["mutant roaring", "mutant flexing muscles", "mutant swiping"],
        "cast_heavy": ["mutant flexing muscles", "mutant roaring", "mutant swiping"],
        "cast_heavy2": ["mutant roaring", "mutant flexing muscles", "mutant swiping"],
        "hit": ["mutant punch", "mutant swiping", "mutant jumping (2)"],
        "hit_heavy": ["mutant jump attack", "jump attack", "mutant punch"],
        "death": ["mutant dying", "dying"],
    },
    "standing": {
        "idle": ["standing idle", "idle"],
        "walk": ["standing walk forward", "standing run forward"],
        "run": ["standing sprint forward", "standing run forward"],
        "walk_back": ["standing walk back", "standing run back"],
        "strafe_left": ["standing walk left", "standing run left"],
        "strafe_right": ["standing walk right", "standing run right"],
        "lunge": ["standing jump running", "standing jump"],
        "cast": ["standing idle"],
        "cast_heavy": ["standing jump", "standing idle"],
        "cast_heavy2": ["standing jump running", "standing idle"],
        "hit": ["standing land to standing idle", "standing jump"],
        "hit_heavy": ["standing jump running landing", "standing land to standing idle"],
        "death": ["standing land to standing idle", "standing idle"],
    },
}


def norm_name(path_or_name):
    name = Path(path_or_name).stem.lower()
    name = re.sub(r"\s+", " ", name)
    return name.strip()


def reset_scene():
    bpy.ops.wm.read_homefile(use_empty=True)
    for action in list(bpy.data.actions):
        bpy.data.actions.remove(action)


def imported_objects_before_after(fn):
    before = set(bpy.data.objects)
    fn()
    return [obj for obj in bpy.data.objects if obj not in before]


def import_fbx(path):
    return imported_objects_before_after(lambda: bpy.ops.import_scene.fbx(filepath=str(path)))


def find_armature(objects=None):
    src = objects if objects is not None else bpy.data.objects
    arms = [obj for obj in src if obj.type == "ARMATURE"]
    if arms:
        return arms[0]
    return next((obj for obj in bpy.data.objects if obj.type == "ARMATURE"), None)


def mesh_objects():
    return [obj for obj in bpy.data.objects if obj.type == "MESH"]


def concept_texture(concept_dir, stem):
    p = concept_dir / stem
    return str(p) if p.exists() else ""


def image_node(nodes, path, colorspace="sRGB"):
    if not path:
        return None
    img = bpy.data.images.load(path, check_existing=True)
    img.colorspace_settings.name = colorspace
    node = nodes.new(type="ShaderNodeTexImage")
    node.image = img
    return node


def socket(node, *names):
    for name in names:
        s = node.inputs.get(name)
        if s:
            return s
    return None


def assign_concept_material(concept_dir):
    mat = bpy.data.materials.new("PulseMeshyMaterial")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    if bsdf is None:
        bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")

    base = image_node(nodes, concept_texture(concept_dir, "texture_0_base_color.png"), "sRGB")
    if base and socket(bsdf, "Base Color"):
        links.new(base.outputs["Color"], socket(bsdf, "Base Color"))

    emis = image_node(nodes, concept_texture(concept_dir, "texture_0_emission.png"), "sRGB")
    if emis and socket(bsdf, "Emission Color", "Emission"):
        links.new(emis.outputs["Color"], socket(bsdf, "Emission Color", "Emission"))
        strength = socket(bsdf, "Emission Strength")
        if strength:
            strength.default_value = 1.6

    normal = image_node(nodes, concept_texture(concept_dir, "texture_0_normal.png"), "Non-Color")
    if normal and socket(bsdf, "Normal"):
        nmap = nodes.new(type="ShaderNodeNormalMap")
        links.new(normal.outputs["Color"], nmap.inputs["Color"])
        links.new(nmap.outputs["Normal"], socket(bsdf, "Normal"))

    rough = image_node(nodes, concept_texture(concept_dir, "texture_0_roughness.png"), "Non-Color")
    if rough and socket(bsdf, "Roughness"):
        links.new(rough.outputs["Color"], socket(bsdf, "Roughness"))

    metal = image_node(nodes, concept_texture(concept_dir, "texture_0_metallic.png"), "Non-Color")
    if metal and socket(bsdf, "Metallic"):
        links.new(metal.outputs["Color"], socket(bsdf, "Metallic"))

    for obj in mesh_objects():
        obj.data.materials.clear()
        obj.data.materials.append(mat)


def copy_action_for_role(source_action, role):
    action = source_action.copy()
    action.name = role
    action.use_fake_user = True
    return action


def nla_tracks_for_actions(arm, actions):
    arm.animation_data_create()
    arm.animation_data.action = None
    for track in list(arm.animation_data.nla_tracks):
        arm.animation_data.nla_tracks.remove(track)
    for action in actions:
        track = arm.animation_data.nla_tracks.new()
        track.name = action.name
        start = int(action.frame_range[0])
        strip = track.strips.new(action.name, start, action)
        strip.name = action.name
        strip.blend_type = "REPLACE"


def import_action(path):
    imported = import_fbx(path)
    arm = find_armature(imported)
    action = arm.animation_data.action if arm and arm.animation_data else None
    copied = action.copy() if action else None
    if copied:
        copied.name = norm_name(path)
        copied.use_fake_user = True
    for obj in imported:
        if obj.name in bpy.data.objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    for action in list(bpy.data.actions):
        if action.users == 0 and not action.use_fake_user:
            bpy.data.actions.remove(action)
    return copied


def role_source_for(role, action_by_name, pack_kind):
    for candidate in ROLE_CANDIDATES[pack_kind][role]:
        nc = norm_name(candidate)
        if nc in action_by_name:
            return action_by_name[nc]
        for name, action in action_by_name.items():
            if nc in name:
                return action
    return None


def export_pack(concept_name, zip_name, out_name, pack_kind):
    reset_scene()
    zip_path = ENEMY_ROOT / zip_name
    concept_dir = ENEMY_ROOT / concept_name
    extract_dir = WORK_DIR / Path(zip_name).stem
    if extract_dir.exists():
        shutil.rmtree(extract_dir)
    extract_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(extract_dir)

    fbx_files = sorted(extract_dir.glob("*.fbx"))
    base_fbx = next((p for p in fbx_files if p.stem.lower() == concept_name.lower()), None)
    if base_fbx is None:
        base_fbx = fbx_files[0] if fbx_files else None
    if base_fbx is None:
        raise RuntimeError(f"{zip_name} contains no FBX files")

    import_fbx(base_fbx)
    base_arm = find_armature()
    if base_arm is None:
        raise RuntimeError(f"{base_fbx} imported without an armature")

    for obj in mesh_objects():
        obj.name = "pulse_" + concept_name + "_body"
    base_arm.name = "pulse_" + concept_name + "_armature"
    assign_concept_material(concept_dir)

    action_by_name = {}
    for path in fbx_files:
        if path == base_fbx:
            continue
        action = import_action(path)
        if action:
            action_by_name[norm_name(path)] = action

    role_actions = []
    for role in ROLE_CANDIDATES[pack_kind]:
        source = role_source_for(role, action_by_name, pack_kind)
        if source is None:
            continue
        role_actions.append(copy_action_for_role(source, role))

    if not any(a.name == "idle" for a in role_actions):
        base_action = base_arm.animation_data.action if base_arm.animation_data else None
        if base_action:
            role_actions.insert(0, copy_action_for_role(base_action, "idle"))
    if not role_actions:
        raise RuntimeError(f"{zip_name} produced no usable actions")

    nla_tracks_for_actions(base_arm, role_actions)
    base_arm.data.pose_position = "POSE"

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    out_path = OUT_DIR / out_name
    bpy.ops.object.select_all(action="DESELECT")
    base_arm.select_set(True)
    for obj in mesh_objects():
        obj.select_set(True)
    bpy.context.view_layer.objects.active = base_arm
    bpy.ops.export_scene.gltf(
        filepath=str(out_path),
        export_format="GLB",
        use_selection=True,
        export_skins=True,
        export_animations=True,
        export_animation_mode="NLA_TRACKS",
        export_lights=False,
        export_cameras=False,
    )
    print(f"MIXAMO_ENEMY {concept_name} clips={len(role_actions)} -> {out_path}")


def main():
    if WORK_DIR.exists():
        shutil.rmtree(WORK_DIR)
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    for spec in PACKS:
        export_pack(*spec)
    print(f"MIXAMO_ENEMY_DONE {len(PACKS)} packs")


if __name__ == "__main__":
    main()
