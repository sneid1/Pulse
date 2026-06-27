# Dump arm bone rest orientations (head/tail/axes) for posing planning.
# Run: blender.exe -b <file.blend> --python tools/dev/dump_bones.py
import bpy

arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
print("ARMATURE:", arm.name)


def v(x):
    return tuple(round(float(c), 3) for c in x)

want = ('collar', 'bicept', 'elbow', 'forearm', 'hand', 'index1', 'thumb1')
for b in arm.data.bones:
    if not any(w in b.name for w in want):
        continue
    # bone matrix columns = local x (roll), y (along bone), z axes in armature space
    mx = b.matrix_local
    xaxis = (mx[0][0], mx[1][0], mx[2][0])
    yaxis = (mx[0][1], mx[1][1], mx[2][1])
    zaxis = (mx[0][2], mx[1][2], mx[2][2])
    print(f"{b.name:18s} parent={b.parent.name if b.parent else None:>10}")
    print(f"    head={v(b.head_local)} tail={v(b.tail_local)} len={round(b.length,3)}")
    print(f"    x={v(xaxis)} y(along)={v(yaxis)} z={v(zaxis)}")
