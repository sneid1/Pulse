import bpy, math, mathutils
# fresh file
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath='C:/Users/rq27/Pulse/assets/Untextured_3D_Weapons/OBJ/AK-47.obj')
objs=[o for o in bpy.data.objects if o.type=='MESH']
mins=[1e9]*3; maxs=[-1e9]*3
for o in objs:
    for v in o.data.vertices:
        w=o.matrix_world@v.co
        for i in range(3):
            mins[i]=min(mins[i],w[i]); maxs[i]=max(maxs[i],w[i])
dims=[round(maxs[i]-mins[i],3) for i in range(3)]
print("AK objs:",[o.name for o in objs],"count:",len(objs))
print("AK world bbox min",[round(x,3) for x in mins],"max",[round(x,3) for x in maxs])
print("AK dims (X,Y,Z):",dims,"-> longest axis:", "XYZ"[dims.index(max(dims))])
# render side (look along -X) and top (look along -Z)
sc=bpy.context.scene
sc.render.engine='BLENDER_WORKBENCH'
sc.display.shading.light='STUDIO'; sc.display.shading.color_type='SINGLE'; sc.display.shading.single_color=(0.6,0.6,0.62)
sc.render.resolution_x=700; sc.render.resolution_y=400; sc.render.image_settings.file_format='PNG'
center=mathutils.Vector([(mins[i]+maxs[i])*0.5 for i in range(3)])
size=max(dims)
cam=bpy.data.objects.new('c',bpy.data.cameras.new('c')); sc.collection.objects.link(cam); sc.camera=cam
def shot(path,d):
    eye=center+mathutils.Vector(d).normalized()*size*1.6
    cam.location=eye; cam.rotation_euler=(center-eye).to_track_quat('-Z','Y').to_euler()
    sc.render.filepath=path; bpy.ops.render.render(write_still=True); print("RENDERED",path)
shot('C:/Users/rq27/Pulse/tools/dev/ak_sideX.png',(1,0,0.05))
shot('C:/Users/rq27/Pulse/tools/dev/ak_sideY.png',(0,1,0.05))
shot('C:/Users/rq27/Pulse/tools/dev/ak_top.png',(0,0.05,1))
