"""Round-trip the generated FBXs and inspect preview pixels without engine writes."""
from pathlib import Path
import json
import bpy
import numpy as np

root = Path(__file__).resolve().parents[2]
out = root / "assets/models/underground_lab_v01"
manifest = json.loads((out / "asset_manifest.json").read_text(encoding="utf-8"))
report = {"fbx": [], "textures": [], "previews": [], "failures": []}

def check(condition, message):
    if not condition:
        report["failures"].append(message)

for asset in manifest["assets"]:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.import_scene.fbx(filepath=str(out / asset["file"]), use_anim=True)
    meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    triangles = sum(len(obj.data.polygons) for obj in meshes)
    check(triangles == asset["triangles"], asset["file"] + ": triangle count changed")
    for obj in meshes:
        check(len(obj.data.uv_layers) == 1, obj.name + ": UV count")
        check(all(len(p.vertices) == 3 for p in obj.data.polygons), obj.name + ": not triangulated")
        check(all(mat is not None for mat in obj.data.materials), obj.name + ": missing material")
    report["fbx"].append({"file": asset["file"], "triangles": triangles, "mesh_objects": len(meshes)})

for path in sorted((out / "textures").glob("*.png")):
    image = bpy.data.images.load(str(path), check_existing=False)
    check(tuple(image.size) == (2048, 2048), path.name + ": not 2K")
    report["textures"].append({"file": path.name, "size": list(image.size)})
    bpy.data.images.remove(image)

for path in sorted((out / "previews").glob("*.png")):
    image = bpy.data.images.load(str(path), check_existing=False)
    pixels = np.empty(len(image.pixels), dtype=np.float32)
    image.pixels.foreach_get(pixels)
    pixels = pixels.reshape((-1, 4))[:, :3]
    stats = {"file": path.name, "size": list(image.size),
             "mean": float(pixels.mean()), "std": float(pixels.std()),
             "nonblack_fraction": float((pixels.max(axis=1) > .02).mean())}
    check(stats["std"] > .035, path.name + ": insufficient image variation")
    check(stats["nonblack_fraction"] > .80, path.name + ": too dark or blank")
    report["previews"].append(stats)
    bpy.data.images.remove(image)

check(len(report["fbx"]) == 9, "Expected nine FBX files")
check(len(report["textures"]) == 39, "Expected thirteen 2K texture triplets")
check(len(report["previews"]) >= 5, "Missing review images")
(out / "validation_blender.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print("VALIDATION", json.dumps({"fbx": len(report["fbx"]), "textures": len(report["textures"]),
                               "previews": report["previews"], "failures": report["failures"]}), flush=True)
if report["failures"]:
    raise RuntimeError("Artifact verification failed")
