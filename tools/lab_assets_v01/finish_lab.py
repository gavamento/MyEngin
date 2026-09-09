"""Re-export and render this generated asset set after final material checks."""
from pathlib import Path
import sys
import json
import bpy
import numpy as np
import bmesh

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_lab as lab

bpy.ops.wm.open_mainfile(filepath=str(lab.OUT / "Lab_Review.blend"))
placements = {
    "Lab_Room": (0, 0, 0), "Lab_Corridor": (0, 4, 0),
    "Lab_Bench": (-2.5, .25, 0), "Lab_Sink": (-3.6, 3.25, 0),
    "Lab_StorageShelf": (3.85, 3.35, 0), "Lab_MetalFloorPlate": (0, 7, 0),
    "Lab_GlassShards": (1.75, 2.5, 0), "Lab_WaterPuddle": (-3.15, 2.30, 0),
    "Lab_Door": (0, 4.11, 0),
}
for name, position in placements.items():
    lab.ASSETS[name] = {"collection": bpy.data.collections[name], "placement": lab.Vector(position)}
lab.ASSETS["Lab_Door"]["rig"] = bpy.data.objects["Lab_Door_Rig"]
if "--export-only" in sys.argv:
    manifest = json.loads((lab.OUT / "asset_manifest.json").read_text(encoding="utf-8"))
    lab.MATS.update({name: bpy.data.materials[name] for name in manifest["materials"]})
    lab.export_assets()
    lab.log("FBX-only export complete")
    sys.exit(0)
for name in ["Plaster", "PaintWhite", "PaintGreen", "Floor", "Steel", "Rust", "Rubber",
             "Grime", "Epoxy", "Ceramic", "Ink", "Glass", "Water"]:
    mat = bpy.data.materials[name]
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (1, 1, 1, 1)
    lab.MATS[name] = mat
    # The game multiplies texture alpha by the material scalar. Store opacity once.
    if name in {"Glass", "Water"}:
        im = bpy.data.images[name + "_BaseColor"]
        pixels = np.empty(len(im.pixels), dtype=np.float32)
        im.pixels.foreach_get(pixels)
        pixels[3::4] = 1
        im.pixels.foreach_set(pixels)
        im.filepath_raw = str(lab.TEX / (name + "_BaseColor.png"))
        im.save()

# Correct the initial build's coincident floor backing and consolidate mixed UV names.
for obj in list(bpy.context.scene.objects):
    if obj.type != "MESH":
        continue
    mesh = obj.data
    if len(mesh.uv_layers) > 1:
        layers = list(mesh.uv_layers)
        target = layers[0]
        for poly in mesh.polygons:
            for layer in layers:
                uv = [layer.data[li].uv.copy() for li in poly.loop_indices]
                area = abs((uv[1].x-uv[0].x)*(uv[2].y-uv[0].y)-(uv[1].y-uv[0].y)*(uv[2].x-uv[0].x))
                if area > 1e-10:
                    for li, co in zip(poly.loop_indices, uv):
                        target.data[li].uv = co
                    break
        for layer in layers[1:]:
            mesh.uv_layers.remove(layer)
    mesh.uv_layers.active.name = "UV0"
    if obj.get("lab_geometry_fixed"):
        continue
    bm = bmesh.new()
    bm.from_mesh(mesh)
    remaining = set(bm.verts)
    while remaining:
        first = remaining.pop()
        component = {first}
        pending = [first]
        while pending:
            v = pending.pop()
            for edge in v.link_edges:
                other = edge.other_vert(v)
                if other in remaining:
                    remaining.remove(other)
                    component.add(other)
                    pending.append(other)
        points = [v.co for v in component]
        span = [max(v[i] for v in points)-min(v[i] for v in points) for i in range(3)]
        if obj.get("part_category") == "floor" and span[0] > 1 and span[1] > 1:
            for v in component:
                v.co.z -= .04
        if obj.name == "Lab_Corridor_services" and span[0]>7 and span[1]>7 and min(v.x for v in points)>-.65:
            for v in component:
                v.co.y += .23 * min(1, max(0, (v.co.y-10.8)/.6))
        if obj.get("part_category") == "walls" and len(component)==4 and span[2]<.31:
            bmesh.ops.delete(bm, geom=list(component), context="VERTS")
    if obj.name == "Lab_GlassShards_prop":
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh)
    bm.free()
    obj["lab_geometry_fixed"] = True

# Regenerate only paint albedos with fine, restrained staining instead of large spots.
for name in ["Plaster", "PaintWhite", "PaintGreen", "Grime", "Epoxy", "Ceramic"]:
    color = {"Plaster":(.64,.65,.61),"PaintWhite":(.71,.73,.69),"PaintGreen":(.23,.32,.29),
             "Grime":(.15,.16,.13),"Epoxy":(.075,.085,.079),"Ceramic":(.77,.79,.75)}[name]
    rng = np.random.default_rng(1301 + sum(map(ord,name)))
    n = 2048
    broad, mid = lab.noise(n,8,rng), lab.noise(n,80,rng)
    fine = rng.random((n,n),dtype=np.float32)
    dirt = np.clip((.40-lab.noise(n,23,rng))*.20,0,.065)
    shade = .94+.07*broad+.035*(mid-.5)+.02*(fine-.5)-dirt
    rgba = np.ones((n,n,4),dtype=np.float32)
    rgba[:,:,:3] = shade[:,:,None]*np.array(color,dtype=np.float32)
    im = bpy.data.images[name+"_BaseColor"]
    im.pixels.foreach_set(rgba.ravel())
    im.filepath_raw = str(lab.TEX/(name+"_BaseColor.png"))
    im.save()

if bpy.data.actions.get("Door_Open"):
    bpy.data.actions["Door_Open"].name = "00_Door_Open"
if bpy.data.actions.get("Door_Close"):
    bpy.data.actions["Door_Close"].name = "01_Door_Close"
for name in ["00_Door_Open", "01_Door_Close"]:
    for curve in bpy.data.actions[name].fcurves:
        if curve.array_index == 1:
            for key in curve.keyframe_points:
                key.co.y = abs(key.co.y)
lab.ASSETS["Lab_Door"]["rig"].animation_data.action = bpy.data.actions["00_Door_Open"]

lab.export_assets()
bpy.ops.file.make_paths_relative()
bpy.context.preferences.filepaths.save_version = 0
bpy.ops.wm.save_as_mainfile(filepath=str(lab.OUT / "Lab_Review.blend"))
lab.log("Final material export complete")
if "--render" in sys.argv:
    preview = bpy.data.collections["PREVIEW_ONLY_NOT_EXPORTED"]
    cameras = {key:bpy.data.objects[value] for key,value in {
        "01_Room":"View_Room","02_Corridor":"View_Corridor", "03_Workstations":"View_Workstations",
        "04_Door":"View_Door","05_Layout":"View_Layout"}.items()}
    lab.render_previews(preview,cameras)
    bpy.ops.wm.save_as_mainfile(filepath=str(lab.OUT / "Lab_Review.blend"))
