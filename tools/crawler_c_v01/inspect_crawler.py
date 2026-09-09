"""Render action poses and round-trip the final FBX; no engine or prior asset writes."""
from pathlib import Path
import json
import math
import bpy
import numpy as np
from mathutils import Vector

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/"assets/models/enemy_crawler_c_v01"
PRE=OUT/"previews"
bpy.ops.wm.open_mainfile(filepath=str(OUT/"CrawlerC_Review.blend"))
scene=bpy.context.scene
rig=bpy.data.objects["CrawlerC_Rig"]
scene.cycles.samples=24
scene.render.resolution_x=1200
scene.render.resolution_y=800
scene.render.resolution_percentage=100
scene.camera=bpy.data.objects["01_ThreeQuarter"]
for label,action,frame in [
    ("05_Attack","05_Attack",28),
    ("06_LightFlinch","07_LightFlinch",38),
    ("07_Death","08_Death",110),
]:
    rig.animation_data.action=bpy.data.actions[action]
    scene.frame_set(frame)
    scene.render.filepath=str(PRE/(label+".png"))
    print("POSE_RENDER",label,flush=True)
    bpy.ops.render.render(write_still=True)

rig.animation_data.action=bpy.data.actions["10_WallCrawl"]
scene.frame_set(38)
rig.rotation_euler=(-math.pi/2,0,0)
rig.location=(0,0,1.14)
bpy.ops.mesh.primitive_plane_add(size=200,location=(0,-.006,1),rotation=(-math.pi/2,0,0))
wall=bpy.context.object
wall.name="InspectionWall"
wall.data.materials.append(bpy.data.materials["PreviewGroundMaterial"])
data=bpy.data.lights.new("WallInspectionLight","AREA")
data.energy=190
data.shape="DISK"
data.size=3
light=bpy.data.objects.new("WallInspectionLight",data)
scene.collection.objects.link(light)
light.location=(0,3,3)
light.rotation_euler=(Vector((0,0,1))-light.location).to_track_quat("-Z","Y").to_euler()
camera=scene.camera
camera.location=(2.5,3.1,2.0)
camera.rotation_euler=(Vector((0,.18,1.15))-camera.location).to_track_quat("-Z","Y").to_euler()
camera.data.lens=40
scene.render.filepath=str(PRE/"08_WallCrawl.png")
print("POSE_RENDER wall",flush=True)
bpy.ops.render.render(write_still=True)

# Reopen from FBX, so checks below cannot pass using the source scene alone.
bpy.ops.object.select_all(action="SELECT")
bpy.ops.object.delete(use_global=False)
for action in list(bpy.data.actions):
    bpy.data.actions.remove(action)
bpy.ops.import_scene.fbx(filepath=str(OUT/"Enemy_Crawler_C.fbx"),use_anim=True)
meshes=[o for o in scene.objects if o.type=="MESH"]
report={"mesh_objects":len(meshes),"triangles":sum(len(o.data.polygons) for o in meshes),
        "armatures":len([o for o in scene.objects if o.type=="ARMATURE"]),
        "animation_actions":[a.name for a in bpy.data.actions],"previews":[],"failures":[]}
def check(value,message):
    if not value: report["failures"].append(message)
check(len(meshes)==5,"Expected five meshes")
check(report["armatures"]==1,"Expected one armature")
check(len(bpy.data.actions)==11,"Expected eleven imported actions")
for obj in meshes:
    check(len(obj.data.uv_layers)==1,obj.name+": missing or multiple UV sets")
    check(all(len(p.vertices)==3 for p in obj.data.polygons),obj.name+": not triangulated")
    check(len(obj.vertex_groups)>0,obj.name+": no weights")
    for vertex in obj.data.vertices:
        total=sum(g.weight for g in vertex.groups)
        check(abs(total-1)<.0001,obj.name+": invalid weights")
for path in sorted((OUT/"textures").glob("*.png")):
    im=bpy.data.images.load(str(path),check_existing=False)
    check(tuple(im.size)==(2048,2048),path.name+": wrong resolution")
    bpy.data.images.remove(im)
for path in sorted(PRE.glob("*.png")):
    im=bpy.data.images.load(str(path),check_existing=False)
    pixels=np.empty(len(im.pixels),dtype=np.float32)
    im.pixels.foreach_get(pixels)
    rgb=pixels.reshape((-1,4))[:,:3]
    sd=float(rgb.std())
    check(sd>.025,path.name+": blank or insufficient contrast")
    report["previews"].append({"file":path.name,"size":list(im.size),"standard_deviation":sd})
    bpy.data.images.remove(im)
check(len(report["previews"])==8,"Expected eight preview images")
(OUT/"validation_blender.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
print("VALIDATION",json.dumps(report),flush=True)
if report["failures"]: raise RuntimeError("Crawler verification failed")
