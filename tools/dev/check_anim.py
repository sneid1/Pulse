import bpy
arm = next(o for o in bpy.data.objects if o.type=='ARMATURE')
ad = arm.animation_data
print("ARM anim_data:", ad)
if ad:
    print("  action:", ad.action)
    print("  nla_tracks:", [t.name for t in ad.nla_tracks])
    if ad.drivers: print("  drivers:", len(ad.drivers))
print("pose bones constraints:")
for pb in arm.pose.bones:
    if pb.constraints:
        print("  ", pb.name, [(c.name,c.type) for c in pb.constraints])
# check mesh armature modifiers + vertex groups
for o in bpy.data.objects:
    if o.type=='MESH':
        mods=[(m.name,m.type, getattr(m,'object',None).name if getattr(m,'object',None) else None) for m in o.modifiers]
        print("MESH", o.name, "mods:", mods, "vgroups:", len(o.vertex_groups))
