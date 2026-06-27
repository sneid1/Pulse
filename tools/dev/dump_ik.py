import bpy
arm = next(o for o in bpy.data.objects if o.type=='ARMATURE')
def vv(x): return tuple(round(float(c),3) for c in x)
print("=== ALL BONES ===")
for b in arm.data.bones:
    print(b.name, "| head", vv(b.head_local), "tail", vv(b.tail_local), "| parent", b.parent.name if b.parent else None)
print("=== IK / CONSTRAINTS ===")
for pb in arm.pose.bones:
    for c in pb.constraints:
        info = {"on":pb.name,"type":c.type}
        for attr in ("target","subtarget","chain_count","pole_target","pole_subtarget","pole_angle"):
            if hasattr(c,attr):
                val=getattr(c,attr)
                info[attr]=val.name if hasattr(val,'name') else val
        print(info)
