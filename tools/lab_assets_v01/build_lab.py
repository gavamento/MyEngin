"""Build the approved lab asset set in Blender 4.2, without touching engine files.

Run: blender --background --factory-startup --python build_lab.py
All generated outputs are confined to assets/models/underground_lab_v01.
"""
from pathlib import Path
import json
import math
import random
import sys

import bpy
import numpy as np
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets/models/underground_lab_v01"
TEX = OUT / "textures"
PREVIEW = OUT / "previews"
SEED = 74191
random.seed(SEED)
MATS = {}
ASSETS = {}
PARTS = {}
CURRENT = None


def log(message):
    print("LAB: " + message, flush=True)


def noise(size, cells, rng):
    grid = rng.random((cells, cells), dtype=np.float32)
    p = np.arange(size, dtype=np.float32) * cells / size
    a = p.astype(np.int32)
    f = p - a
    f = f * f * (3 - 2 * f)
    b = (a + 1) % cells
    lo = grid[a[:, None], a] * (1 - f) + grid[a[:, None], b] * f
    hi = grid[b[:, None], a] * (1 - f) + grid[b[:, None], b] * f
    return lo * (1 - f[:, None]) + hi * f[:, None]


def save_image(name, rgba, noncolor=False):
    h, w = rgba.shape[:2]
    im = bpy.data.images.new(name, width=w, height=h, alpha=True)
    im.colorspace_settings.name = "Non-Color" if noncolor else "sRGB"
    im.pixels.foreach_set(rgba.astype(np.float32).ravel())
    im.filepath_raw = str(TEX / (name + ".png"))
    im.file_format = "PNG"
    im.save()
    return im


def material(name, color, metallic, roughness, kind="paint", alpha=1):
    rng = np.random.default_rng(SEED + len(MATS) * 37)
    n = 2048
    broad = noise(n, 8, rng)
    mid = noise(n, 80, rng)
    fine = rng.random((n, n), dtype=np.float32)
    shade = .84 + .19 * broad + .075 * (mid - .5) + .025 * (fine - .5)
    height = .5 + .08 * (mid - .5) + .015 * (fine - .5)
    if kind == "paint":
        dirt = np.clip((.40 - noise(n, 23, rng)) * .20, 0, .065)
        shade = .94 + .07 * broad + .035 * (mid - .5) + .02 * (fine - .5) - dirt
    elif kind == "floor":
        grain = np.where(fine > .965, .18, np.where(fine < .028, -.16, 0))
        shade += grain
        height += grain * .12
    elif kind == "steel":
        shade += (noise(n, 256, rng) - .5) * .12
        shade += (rng.random((n, 1), dtype=np.float32) - .5) * .055
        height *= .1
    elif kind == "rust":
        shade = .48 + broad * .55 + mid * .25 + fine * .1
        height = mid * .24 + broad * .06 + fine * .025
    elif kind == "rubber":
        shade = .9 + .1 * mid
        height *= .15
    rgba = np.ones((n, n, 4), dtype=np.float32)
    rgba[:, :, :3] = np.clip(shade[:, :, None] * np.array(color, dtype=np.float32), 0, 1)
    rgba[:, :, 3] = 1
    albedo = save_image(name + "_BaseColor", rgba)
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 1.8
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 1.8
    inv = 1 / np.sqrt(1 + dx * dx + dy * dy)
    rgba[:, :, 0] = -.5 * dx * inv + .5
    rgba[:, :, 1] = -.5 * dy * inv + .5
    rgba[:, :, 2] = .5 * inv + .5
    rgba[:, :, 3] = 1
    normal = save_image(name + "_Normal", rgba, True)
    # D3D loader flips V. Its derivative TBN therefore needs the opposite Y component.
    rgba[:, :, 1] = 1 - rgba[:, :, 1]
    save_image(name + "_Normal_DX", rgba, True)
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (1, 1, 1, 1)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Alpha"].default_value = alpha
    base = mat.node_tree.nodes.new("ShaderNodeTexImage")
    base.image = albedo
    base.location = (-600, 160)
    mat.node_tree.links.new(base.outputs["Color"], bsdf.inputs["Base Color"])
    tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.name = "NormalTexture"
    tex.image = normal
    tex.location = (-600, -140)
    normal_node = mat.node_tree.nodes.new("ShaderNodeNormalMap")
    normal_node.location = (-300, -140)
    mat.node_tree.links.new(tex.outputs["Color"], normal_node.inputs["Color"])
    mat.node_tree.links.new(normal_node.outputs["Normal"], bsdf.inputs["Normal"])
    if alpha < 1:
        mat.surface_render_method = "DITHERED"
    MATS[name] = mat
    log("2K material: " + name)
    return mat


def asset(name, placement=(0, 0, 0)):
    global CURRENT
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    ASSETS[name] = {"collection": collection, "placement": Vector(placement)}
    CURRENT = name
    return collection


def register(obj, name, mat, category="structure"):
    obj.name = name
    for c in list(obj.users_collection):
        c.objects.unlink(obj)
    ASSETS[CURRENT]["collection"].objects.link(obj)
    if mat:
        obj.data.materials.append(MATS[mat])
    if hasattr(obj.data, "uv_layers") and obj.data.uv_layers.active:
        obj.data.uv_layers.active.name = "UV0"
    obj["part_category"] = category
    PARTS.setdefault((CURRENT, category), []).append(obj)
    return obj


def planar_uv(obj, scale=1.0):
    mesh = obj.data
    uv = mesh.uv_layers.active or mesh.uv_layers.new(name="UV0")
    for poly in mesh.polygons:
        axis = max(range(3), key=lambda i: abs(poly.normal[i]))
        axes = [(1, 2), (0, 2), (0, 1)][axis]
        for li in poly.loop_indices:
            p = mesh.vertices[mesh.loops[li].vertex_index].co
            uv.data[li].uv = (p[axes[0]] * scale, p[axes[1]] * scale)


def apply_mod(obj, modifier):
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def box(name, location, size, mat, bevel=.012, category="structure", uvscale=1):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    obj = register(bpy.context.object, name, mat, category)
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    planar_uv(obj, uvscale)
    if bevel:
        mod = obj.modifiers.new("Manufactured edge", "BEVEL")
        mod.width = bevel
        mod.segments = 2
        apply_mod(obj, mod)
        mod = obj.modifiers.new("Face normals", "WEIGHTED_NORMAL")
        mod.keep_sharp = True
        apply_mod(obj, mod)
    return obj


def cylinder(name, a, b, radius, mat, vertices=16, category="services"):
    a, b = Vector(a), Vector(b)
    d = b - a
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=d.length,
                                       end_fill_type="NGON", location=(a + b) / 2)
    obj = register(bpy.context.object, name, mat, category)
    obj.rotation_mode = "QUATERNION"
    obj.rotation_quaternion = d.to_track_quat("Z", "Y")
    for p in obj.data.polygons:
        p.use_smooth = len(p.vertices) == 4
    mod = obj.modifiers.new("Edge", "BEVEL")
    mod.width = min(radius * .12, .004)
    mod.segments = 2
    apply_mod(obj, mod)
    return obj


def pipe(name, points, radius, mat="Steel", category="services"):
    curve = bpy.data.curves.new(name, "CURVE")
    curve.dimensions = "3D"
    curve.resolution_u = 8
    curve.bevel_depth = radius
    curve.bevel_resolution = 3
    sp = curve.splines.new("POLY")
    sp.points.add(len(points) - 1)
    for p, co in zip(sp.points, points):
        p.co = (*co, 1)
    obj = bpy.data.objects.new(name, curve)
    ASSETS[CURRENT]["collection"].objects.link(obj)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.convert(target="MESH")
    register(obj, name, mat, category)
    planar_uv(obj)
    for p in obj.data.polygons:
        p.use_smooth = True
    return obj


def label(text, location, rotation, size=.1, mat="Ink", category="details"):
    bpy.ops.object.text_add(location=location, rotation=rotation)
    obj = bpy.context.object
    obj.data.body = text
    obj.data.size = size
    obj.data.extrude = .0003
    obj.data.align_x = "CENTER"
    bpy.ops.object.convert(target="MESH")
    register(obj, "Mark_" + text.replace(" ", "_"), mat, category)
    planar_uv(obj)
    return obj


def wall_x(name, x0, x1, y):
    x, length = (x0 + x1) / 2, x1 - x0
    box(name + "_upper", (x, y, 2.08), (length, .22, 1.84), "Plaster", category="walls")
    box(name + "_lower", (x, y, .58), (length, .225, 1.16), "PaintGreen", category="walls")
    box(name + "_rail", (x, y, 1.16), (length, .25, .045), "Steel", .008, "walls")
    box(name + "_skirt", (x, y, .07), (length, .26, .14), "Rubber", .009, "walls")
    for sx in np.arange(x0 + 1.2, x1 - .1, 1.2):
        box("PanelJoint", (float(sx), y, 2.08), (.009, .224, 1.8), "Grime", .001, "walls")


def wall_y(name, y0, y1, x):
    y, length = (y0 + y1) / 2, y1 - y0
    box(name + "_upper", (x, y, 2.08), (.22, length, 1.84), "Plaster", category="walls")
    box(name + "_lower", (x, y, .58), (.225, length, 1.16), "PaintGreen", category="walls")
    box(name + "_rail", (x, y, 1.16), (.25, length, .045), "Steel", .008, "walls")
    box(name + "_skirt", (x, y, .07), (.26, length, .14), "Rubber", .009, "walls")
    for sy in np.arange(y0 + 1.2, y1 - .1, 1.2):
        box("PanelJoint", (x, float(sy), 2.08), (.224, .009, 1.8), "Grime", .001, "walls")


def floor_rect(x0, x1, y0, y1):
    box("FloorSubstrate", ((x0+x1)/2, (y0+y1)/2, -.115), (x1-x0, y1-y0, .15), "Grime", .002, "floor")
    for x in np.arange(x0, x1-.01, .5):
        for y in np.arange(y0, y1-.01, .5):
            w, d = min(.5, x1-x), min(.5, y1-y)
            box("FloorTile", (float(x+w/2), float(y+d/2), -.016),
                (float(w-.006), float(d-.006), .032), "Floor", .0025, "floor", 1.5)


def ceiling_rect(x0, x1, y0, y1):
    box("CeilingBacking", ((x0+x1)/2, (y0+y1)/2, 3.055), (x1-x0, y1-y0, .11), "Grime", .002, "ceiling")
    for x in np.arange(x0, x1-.01, 1.0):
        for y in np.arange(y0, y1-.01, 1.0):
            w, d = min(1., x1-x), min(1., y1-y)
            box("CeilingPanel", (float(x+w/2), float(y+d/2), 3.015),
                (float(w-.018), float(d-.018), .025), "Plaster", .004, "ceiling")


def fixture(x, y, rotate=False):
    root = []
    root.append(box("LuminaireBody", (x, y, 2.9), (.36, 1.3, .11), "PaintWhite", .018, "ceiling"))
    root.append(box("LuminaireInset", (x, y, 2.837), (.30, 1.23, .025), "Grime", .01, "ceiling"))
    for dx in [-.085, .085]:
        root.append(cylinder("UnlitTube", (x+dx,y-.52,2.81), (x+dx,y+.52,2.81), .024, "Ceramic", 16, "ceiling"))
    for dy in np.linspace(-.51,.51,9):
        root.append(box("LuminaireLouver", (x,y+float(dy),2.79), (.29,.012,.05), "Steel", .002, "ceiling"))
    if rotate:
        for obj in root:
            p = obj.location.copy() - Vector((x,y,0))
            obj.location = Vector((-p.y, p.x, p.z)) + Vector((x,y,0))
            obj.rotation_mode = "XYZ"
            obj.rotation_euler.z += math.pi/2


def wall_grime(y, x0, x1, inward=-1):
    # Thin, irregular opaque deposits sit on the wall; no blended decal shader needed.
    for k in range(int((x1-x0)*4)):
        x = random.uniform(x0+.03, x1-.03)
        width = random.uniform(.018, .07)
        height = random.uniform(.045, .28)
        verts = [(x-width,y+inward*.114,.145), (x+width,y+inward*.114,.145),
                 (x+width*.4,y+inward*.114,height+.145),
                 (x-width*.2,y+inward*.114,height*.8+.145)]
        mesh = bpy.data.meshes.new("Deposit")
        mesh.from_pydata(verts, [], [(0,1,2,3)] if inward < 0 else [(3,2,1,0)])
        mesh.update()
        obj = bpy.data.objects.new("WallDeposit", mesh)
        ASSETS[CURRENT]["collection"].objects.link(obj)
        register(obj, "WallDeposit", "Grime", "walls")
        planar_uv(obj)


def architecture():
    asset("Lab_Room")
    floor_rect(-5,5,-4,4)
    ceiling_rect(-5,5,-4,4)
    wall_x("South", -5.11,5.11,-4.11)
    wall_y("West", -4,4,-5.11)
    wall_y("East", -4,4,5.11)
    wall_x("NorthLeft", -5.11,-.79,4.11)
    wall_x("NorthRight", .79,5.11,4.11)
    box("DoorLintel", (0,4.11,2.70), (1.58,.22,.60), "Plaster", category="walls")
    for x in [-2.5,2.5]:
        for y in [-2,1.8]:
            fixture(x,y)
    for x in [-4.7,-4.43]:
        cylinder("ServicePipe", (x,-3.8,2.69), (x,3.8,2.69), .055, "Steel")
        for y in np.arange(-3.5,4,1.25):
            cylinder("OxideCoupling", (x,float(y)-.06,2.69), (x,float(y)+.06,2.69), .063, "Rust")
            box("PipeBracket", (x,float(y),2.82), (.18,.045,.26), "Steel", .004,"services")
    box("CableTray", (4.60,0,2.72), (.32,7.8,.055), "Steel", .005, "services")
    for y in np.arange(-3.8,4,.25):
        box("TraySlot", (4.60,float(y),2.688), (.22,.10,.006), "Grime", .001,"services")
    for y in [-2,2]:
        box("VentFrame", (4.984,y,2.20), (.04,1.00,.42), "Steel", .008,"details")
        box("VentInterior", (4.958,y,2.20), (.009,.91,.34), "Rubber", .001,"details")
        for z in np.linspace(2.05,2.35,8):
            box("VentSlat", (4.94,y,float(z)), (.038,.90,.018), "Steel", .002,"details")
    box("RoomPlate", (1.30,3.981,1.78), (.54,.018,.25), "PaintWhite", .008,"details")
    label("LAB 01", (1.30,3.968,1.74), (math.pi/2,0,0), .106)
    asset("Lab_Corridor", (0,4,0))
    floor_rect(-1.25,1.25,4,12.75)
    floor_rect(1.25,7.5,10.25,12.75)
    ceiling_rect(-1.25,1.25,4,12.75)
    ceiling_rect(1.25,7.5,10.25,12.75)
    wall_y("CorridorWest",4.22,12.75,-1.36)
    wall_y("CorridorInner",4.22,10.25,1.36)
    wall_x("CorridorNorth",-1.47,7.72,12.86)
    wall_x("CorridorSouth",1.25,7.5,10.14)
    wall_y("CorridorEnd",10.25,12.75,7.61)
    for y in [5.8,9,11.5]:
        fixture(0,y)
    for x in [3,6]:
        fixture(x,11.5,True)
    # Rounded service elbows follow the turn; the path is one continuous tube.
    for offset in [-.80,-.57]:
        turn_y = 11.5 + (offset + .80)
        pts = [(offset,4.2,2.64),(offset,turn_y,2.64)]
        for a in np.linspace(math.pi,math.pi/2,10):
            pts.append((offset+.4+.4*math.cos(a),turn_y+.4*math.sin(a),2.64))
        pts.append((7.4,turn_y+.4,2.64))
        pipe("CorridorService", pts, .046)
        for y in [5,7,9,11]:
            cylinder("OxideCoupling",(offset,y-.055,2.64),(offset,y+.055,2.64),.057,"Rust")
            box("ServiceMount",(offset,y,2.81),(.15,.038,.33),"Steel",.003,"services")
    box("EndVentFrame",(7.482,11.5,1.9),(.035,.90,.55),"Steel",.008,"details")
    box("EndVentDark",(7.46,11.5,1.9),(.01,.80,.45),"Rubber",.001,"details")
    for z in np.linspace(1.70,2.10,10):
        box("EndVentSlat",(7.45,11.5,float(z)),(.03,.78,.016),"Steel",.002,"details")


def bench():
    asset("Lab_Bench",(-2.5,.25,0))
    box("EpoxyTop",(0,0,.93),(2.4,.88,.075),"Epoxy",.025,"prop")
    box("TopEdge",(0,0,.885),(2.36,.85,.025),"Steel",.005,"prop")
    for x in [-1.06,1.06]:
        for y in [-.31,.31]:
            box("SquareLeg",(x,y,.435),(.06,.06,.87),"Steel",.009,"prop")
            box("RustFoot",(x,y,.05),(.067,.067,.10),"Rust",.006,"prop")
            box("FootPad",(x,y,.018),(.085,.085,.036),"Rubber",.006,"prop")
    box("LowerRack",(0,0,.22),(2.16,.65,.035),"PaintGreen",.008,"prop")
    box("UnderTopFrame",(0,.30,.79),(2.15,.05,.14),"PaintGreen",.006,"prop")
    for x in [-.61,.61]:
        box("DrawerBody",(x,0,.725),(1.13,.68,.28),"PaintWhite",.012,"prop")
        box("DrawerFront",(x,-.358,.73),(1.08,.028,.23),"PaintGreen",.009,"prop")
        for dx in [-.14,.14]:
            cylinder("HandleStem",(x+dx,-.38,.73),(x+dx,-.42,.73),.012,"Steel",12,"prop")
        cylinder("DrawerPull",(x-.14,-.42,.73),(x+.14,-.42,.73),.013,"Steel",16,"prop")


def sink():
    asset("Lab_Sink",(-3.6,3.25,0))
    box("SinkCabinet",(0,0,.45),(1.35,.68,.86),"PaintWhite",.016,"prop")
    box("ToeKick",(0,-.01,.07),(1.20,.62,.14),"Rubber",.005,"prop")
    for x in [-.326,.326]:
        box("CabinetDoor",(x,-.35,.49),(.634,.028,.68),"PaintGreen",.009,"prop")
        cylinder("CabinetPull",(x+(.2 if x<0 else -.2),-.39,.44),
                 (x+(.2 if x<0 else -.2),-.39,.64),.012,"Steel",12,"prop")
    box("SinkRimFront",(0,-.30,.92),(1.44,.14,.065),"Steel",.013,"prop")
    box("SinkRimBack",(0,.30,.92),(1.44,.20,.065),"Steel",.013,"prop")
    for x in [-.52,.52]:
        box("SinkSideDeck",(x,-.025,.92),(.40,.41,.065),"Steel",.013,"prop")
    box("BasinBottom",(0,-.025,.745),(.65,.40,.028),"Steel",.04,"prop")
    for x in [-.34,.34]:
        box("BasinSide",(x,-.025,.83),(.032,.43,.18),"Steel",.014,"prop")
    for y in [-.235,.185]:
        box("BasinEnd",(0,y,.83),(.68,.03,.18),"Steel",.014,"prop")
    cylinder("Drain",(0,-.025,.76),(0,-.025,.765),.047,"Rubber",24,"prop")
    for a in np.linspace(0,2*math.pi,9)[:-1]:
        cylinder("DrainHole",(.03*math.cos(a),-.025+.03*math.sin(a),.766),
                 (.03*math.cos(a),-.025+.03*math.sin(a),.768),.005,"Steel",8,"prop")
    pts=[(0,.29,.94),(0,.29,1.18)]
    for a in np.linspace(0,math.pi,16):
        pts.append((0,.18+.11*math.cos(a),1.18+.11*math.sin(a)))
    pts.append((0,.07,1.12))
    pipe("GooseneckTap",pts,.019,"Steel","prop")
    cylinder("TapBase",(0,.29,.93),(0,.29,.975),.043,"Steel",24,"prop")
    for x in [-.16,.16]:
        cylinder("Valve",(x,.29,.94),(x,.29,1.00),.025,"Steel",16,"prop")
        box("ValveLever",(x,.29,1.00),(.11,.022,.025),"Ceramic",.006,"prop")
    box("Backsplash",(0,.365,1.08),(1.44,.045,.31),"Ceramic",.01,"prop")


def shelf():
    asset("Lab_StorageShelf",(3.85,3.35,0))
    for x in [-.72,.72]:
        for y in [-.23,.23]:
            box("Upright",(x,y,1.02),(.038,.045,2.04),"PaintGreen",.004,"prop")
            box("OxideBase",(x,y,.07),(.044,.05,.14),"Rust",.004,"prop")
            for z in np.arange(.25,1.95,.12):
                box("UprightSlot",(x,y-.023,float(z)),(.012,.003,.025),"Rubber",.001,"prop")
    for z in [.16,.58,1.,1.42,1.84]:
        box("ShelfDeck",(0,0,z),(1.50,.56,.035),"Steel",.007,"prop")
        box("ShelfLip",(0,-.27,z-.025),(1.50,.025,.075),"PaintGreen",.004,"prop")
    cylinder("CrossBrace",(-.71,.255,.17),(.71,.255,1.93),.01,"Steel",12,"prop")
    cylinder("CrossBrace",(.71,.26,.17),(-.71,.26,1.93),.01,"Steel",12,"prop")


def metal_plate():
    asset("Lab_MetalFloorPlate",(0,7,0))
    box("Plate",(0,0,.018),(2.25,1.45,.036),"Steel",.012,"prop")
    for x in np.arange(-1.05,1.08,.115):
        for y in np.arange(-.64,.67,.115):
            rib=box("CheckerTread",(float(x),float(y),.039),(.071,.018,.009),"Steel",.004,"prop")
            rib.rotation_euler.z=math.pi/4 if int(round((x+1.05)/.115))%2 else -math.pi/4
    for x in [-1.03,1.03]:
        for y in [-.61,.61]:
            cylinder("FastenerWasher",(x,y,.036),(x,y,.044),.033,"Rust",16,"prop")
            cylinder("HexBolt",(x,y,.044),(x,y,.052),.022,"Steel",6,"prop")


def glass_shards():
    asset("Lab_GlassShards",(1.75,2.5,0))
    for i in range(34):
        x,y=random.uniform(-.67,.67),random.uniform(-.50,.50)
        r=random.uniform(.025,.12)
        count=random.choice([3,4,5])
        angles=sorted(random.uniform(0,2*math.pi) for _ in range(count))
        verts=[(x+r*math.cos(a),y+r*math.sin(a),.006+random.uniform(0,.009)) for a in angles]
        verts += [(vx,vy,vz-.004) for vx,vy,vz in verts]
        faces=[tuple(range(count)),tuple(range(2*count-1,count-1,-1))]
        faces.extend((k+count,(k+1)%count+count,(k+1)%count,k) for k in range(count))
        mesh=bpy.data.meshes.new("Shard")
        mesh.from_pydata(verts,[],faces)
        mesh.update()
        obj=bpy.data.objects.new("GlassShard",mesh)
        ASSETS[CURRENT]["collection"].objects.link(obj)
        register(obj,"GlassShard","Glass","prop")
        planar_uv(obj)


def puddle():
    asset("Lab_WaterPuddle",(-3.15,2.30,0))
    count=80
    verts=[(0,0,.004)]
    for i in range(count):
        a=i*2*math.pi/count
        r=1+.11*math.sin(a*5)+.075*math.cos(a*9)
        verts.append((.82*r*math.cos(a),.48*r*math.sin(a),.004))
    faces=[(0,i+1,(i+1)%count+1) for i in range(count)]
    mesh=bpy.data.meshes.new("PuddleSurface")
    mesh.from_pydata(verts,[],faces)
    mesh.update()
    obj=bpy.data.objects.new("PuddleSurface",mesh)
    ASSETS[CURRENT]["collection"].objects.link(obj)
    register(obj,"PuddleSurface","Water","prop")
    planar_uv(obj)


def door():
    asset("Lab_Door",(0,4.11,0))
    for x in [-.76,.76]:
        box("FrameJamb",(x,0,1.185),(.065,.22,2.37),"Steel",.01,"frame")
        box("FrameSeal",(x*.943,-.027,1.15),(.015,.025,2.30),"Rubber",.003,"frame")
    box("FrameHeader",(0,0,2.365),(1.58,.22,.07),"Steel",.008,"frame")
    box("Threshold",(0,0,.012),(1.5,.23,.024),"Steel",.005,"frame")
    box("Leaf",(0,0,1.165),(1.43,.065,2.26),"PaintGreen",.014,"leaf")
    for y in [-.039,.039]:
        box("KickPlate",(0,y,.29),(1.36,.012,.46),"Steel",.005,"leaf")
        box("UpperPanel",(0,y,1.72),(1.29,.012,.98),"PaintWhite",.009,"leaf")
        box("HandleBackplate",(.53,y*1.25,1.04),(.09,.018,.25),"Steel",.007,"leaf")
        cylinder("HandleStem",(.53,y*1.25,1.07),(.53,y*2.9,1.07),.019,"Steel",16,"leaf")
        cylinder("Lever",(.34,y*2.9,1.07),(.53,y*2.9,1.07),.018,"Steel",16,"leaf")
    for z in [.27,1.16,2.05]:
        cylinder("Hinge",(-.72,0,z-.09),(-.72,0,z+.09),.027,"Steel",20,"frame")
        box("HingeOxide",(-.70,-.037,z),(.038,.008,.14),"Rust",.003,"leaf")
    label("LAB 01",(0,-.051,1.78),(math.pi/2,0,0),.14,"Ink","leaf")
    label("PUSH",(.5,-.052,1.30),(math.pi/2,0,0),.055,"Ink","leaf")


def merge_parts():
    for (name, category), objects in PARTS.items():
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.select_set(True)
        bpy.context.view_layer.objects.active=objects[0]
        bpy.ops.object.join()
        obj=bpy.context.object
        obj.name=name+"_"+category
        # Collapse identical slots so each material is a single FBX material part.
        old=list(obj.data.materials)
        unique=list(dict.fromkeys(old))
        remap=[unique.index(m) for m in old]
        indices=[remap[p.material_index] for p in obj.data.polygons]
        obj.data.materials.clear()
        for mat in unique:
            obj.data.materials.append(mat)
        for poly,idx in zip(obj.data.polygons,indices):
            poly.material_index=idx
        obj["part_category"]=category
        obj["lab_geometry_fixed"] = True
        bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
        mod=obj.modifiers.new("GameTriangles","TRIANGULATE")
        mod.keep_custom_normals=True
        apply_mod(obj,mod)


def rig_door():
    data=ASSETS["Lab_Door"]
    arm_data=bpy.data.armatures.new("Lab_Door_Skeleton")
    arm=bpy.data.objects.new("Lab_Door_Rig",arm_data)
    data["collection"].objects.link(arm)
    bpy.ops.object.select_all(action="DESELECT")
    arm.select_set(True)
    bpy.context.view_layer.objects.active=arm
    bpy.ops.object.mode_set(mode="EDIT")
    root=arm_data.edit_bones.new("DoorRoot")
    root.head=(0,0,0)
    root.tail=(0,0,.2)
    hinge=arm_data.edit_bones.new("DoorHinge")
    hinge.head=(-.72,0,0)
    hinge.tail=(-.72,0,2.3)
    hinge.parent=root
    bpy.ops.object.mode_set(mode="OBJECT")
    for obj in list(data["collection"].objects):
        if obj.type!="MESH":
            continue
        group=obj.vertex_groups.new(name="DoorHinge" if obj["part_category"]=="leaf" else "DoorRoot")
        group.add(list(range(len(obj.data.vertices))),1.,"REPLACE")
        obj.parent=arm
        mod=obj.modifiers.new("DoorSkin","ARMATURE")
        mod.object=arm
    bone=arm.pose.bones["DoorHinge"]
    bone.rotation_mode="XYZ"
    arm.animation_data_create()
    for name,start,end in [("00_Door_Open",0,math.pi/2),("01_Door_Close",math.pi/2,0)]:
        action=bpy.data.actions.new(name)
        arm.animation_data.action=action
        # Bone local Y is vertical. Bake each frame for a predictable 60 fps clip.
        for frame in range(61):
            t=frame/60
            angle=start+(end-start)*(t*t*(3-2*t))
            bone.rotation_euler=(0,angle,0)
            bone.keyframe_insert(data_path="rotation_euler",frame=frame,group="DoorHinge")
        for fc in action.fcurves:
            for key in fc.keyframe_points:
                key.interpolation="LINEAR"
        action.use_fake_user=True
    arm.animation_data.action=bpy.data.actions["00_Door_Open"]
    bpy.context.scene.frame_set(0)
    data["rig"]=arm


def place_props():
    for name,data in ASSETS.items():
        if name in {"Lab_Room","Lab_Corridor"}:
            continue
        for obj in data["collection"].objects:
            if not obj.parent:
                obj.location += data["placement"]


def export_assets():
    install_pbr_export()
    manifest={"version":1,"units":"meters","source_axes":"Blender Z-up, +Y corridor",
              "engine_axes":"left-handed Y-up; position mapping (x,z,y)","fps":60,
              "room_clear_size_m":[10,8,3],"corridor_width_m":2.5,
              "corridor_centerline_length_m":15,"assets":[],"materials":{}}
    for name,mat in MATS.items():
        bsdf=mat.node_tree.nodes.get("Principled BSDF")
        manifest["materials"][name]={"metallic":bsdf.inputs["Metallic"].default_value,
            "roughness":bsdf.inputs["Roughness"].default_value,
            "opacity":bsdf.inputs["Alpha"].default_value,
            "base_color":"textures/"+name+"_BaseColor.png",
            "normal_engine":"textures/"+name+"_Normal_DX.png",
            "normal_blender":"textures/"+name+"_Normal.png"}
        normal_tex=mat.node_tree.nodes["NormalTexture"]
        image=bpy.data.images.get(name+"_Normal_DX")
        if image is None:
            image=bpy.data.images.load(str(TEX/(name+"_Normal_DX.png")),check_existing=True)
            image.colorspace_settings.name="Non-Color"
        normal_tex.image=image
    for name,data in ASSETS.items():
        log("Exporting " + name)
        bpy.ops.object.select_all(action="DESELECT")
        roots=[]
        for obj in data["collection"].objects:
            obj.hide_set(False)
            obj.select_set(True)
            if not obj.parent:
                roots.append((obj,obj.location.copy()))
                obj.location-=data["placement"]
        bpy.context.view_layer.objects.active=data.get("rig") or roots[0][0]
        bpy.context.scene.frame_set(0)
        bpy.context.view_layer.update()
        bpy.ops.export_scene.fbx(filepath=str(OUT/(name+".fbx")),use_selection=True,
            object_types={"MESH","ARMATURE"},axis_forward="-Z",axis_up="Y",
            apply_unit_scale=True,apply_scale_options="FBX_SCALE_UNITS",
            use_space_transform=True,bake_space_transform=False,
            mesh_smooth_type="FACE",use_mesh_modifiers=True,add_leaf_bones=False,
            use_armature_deform_only=True,bake_anim=name=="Lab_Door",
            bake_anim_use_all_actions=True,bake_anim_use_nla_strips=False,
            bake_anim_simplify_factor=0,bake_anim_step=1.0,
            path_mode="RELATIVE",embed_textures=False,use_custom_props=False)
        for obj,loc in roots:
            obj.location=loc
        bpy.context.view_layer.update()
        meshes=[o for o in data["collection"].objects if o.type=="MESH"]
        placement=data["placement"]
        manifest["assets"].append({"file":name+".fbx","mesh_objects":len(meshes),
            "triangles":sum(len(o.data.polygons) for o in meshes),
            "engine_position":[placement.x,placement.z,placement.y],
            "engine_rotation_degrees":[0,0,0],"engine_scale":[1,1,1],
            "clips":["Lab_Door_Rig|00_Door_Open","Lab_Door_Rig|01_Door_Close"] if name=="Lab_Door" else []})
    for name,mat in MATS.items():
        mat.node_tree.nodes["NormalTexture"].image=bpy.data.images.get(name+"_Normal")
    (OUT/"asset_manifest.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
    return manifest


def install_pbr_export():
    # ufbx's Blender PBR interpretation is opt-in and is not enabled by MyEngine.
    # Export the supported physical-material descriptor, retaining legacy texture links.
    # This patches only this process; Blender and engine installation files are untouched.
    from io_scene_fbx import export_fbx_bin as fbx
    from io_scene_fbx.fbx_utils import elem_props_set
    if getattr(fbx.fbx_data_material_elements, "lab_pbr", False):
        return
    original=fbx.fbx_data_material_elements
    def export_material(root, mat, scene_data):
        original(root, mat, scene_data)
        element=root.elems[-1]
        assert element.id==b"Material"
        props=next(e for e in element.elems if e.id==b"Properties70")
        for e in element.elems:
            if e.id==b"ShadingModel":
                e.props.clear()
                e.props_type.clear()
                e.add_string(b"PhysicalMaterial")
        for p in props.elems:
            if p.id==b"P" and p.props[0][4:]==b"ShadingModel":
                p.props.pop()
                p.props_type.pop()
                p.add_string(b"PhysicalMaterial")
        bsdf=mat.node_tree.nodes["Principled BSDF"]
        elem_props_set(props,"p_integer",b"3dsMax|ClassIDa",0x3d6b1cec)
        elem_props_set(props,"p_integer",b"3dsMax|ClassIDb",0xdeadc001-0x100000000)
        prefix=b"3dsMax|Parameters|"
        elem_props_set(props,"p_color",prefix+b"base_color",(1.,1.,1.))
        for key,value in [(b"base_weight",1.),(b"metalness",bsdf.inputs["Metallic"].default_value),
                          (b"roughness",bsdf.inputs["Roughness"].default_value),
                          (b"cutout",bsdf.inputs["Alpha"].default_value)]:
            elem_props_set(props,"p_number",prefix+key,value)
    export_material.lab_pbr=True
    fbx.fbx_data_material_elements=export_material


def light(collection,name,location,power,color,size=3,target=None):
    data=bpy.data.lights.new(name,"AREA")
    data.energy=power
    data.shape="DISK"
    data.size=size
    data.color=color
    obj=bpy.data.objects.new(name,data)
    collection.objects.link(obj)
    obj.location=location
    if target:
        obj.rotation_euler=(Vector(target)-obj.location).to_track_quat("-Z","Y").to_euler()
    return obj


def camera(collection,name,position,target,lens):
    data=bpy.data.cameras.new(name)
    obj=bpy.data.objects.new(name,data)
    collection.objects.link(obj)
    obj.location=position
    obj.rotation_euler=(Vector(target)-obj.location).to_track_quat("-Z","Y").to_euler()
    data.lens=lens
    data.clip_start=.04
    data.clip_end=100
    return obj


def setup_preview():
    scene=bpy.context.scene
    scene.render.engine="CYCLES"
    scene.cycles.samples=48
    scene.cycles.use_denoising=True
    scene.render.resolution_x=1600
    scene.render.resolution_y=1000
    scene.render.resolution_percentage=100
    scene.render.image_settings.file_format="PNG"
    scene.world.use_nodes=True
    scene.world.node_tree.nodes["Background"].inputs[0].default_value=(.12,.14,.17,1)
    scene.world.node_tree.nodes["Background"].inputs[1].default_value=.25
    scene.view_settings.view_transform="AgX"
    scene.view_settings.look="AgX - Medium High Contrast"
    scene.view_settings.exposure=.5
    preview=bpy.data.collections.new("PREVIEW_ONLY_NOT_EXPORTED")
    scene.collection.children.link(preview)
    for x in [-2.5,2.5]:
        for y in [-2,1.8]:
            light(preview,"InspectionRoomLight",(x,y,2.72),190,(.90,.96,1.0),2.5)
    for x,y in [(0,5.8),(0,9),(0,11.5),(3,11.5),(6,11.5)]:
        light(preview,"InspectionCorridorLight",(x,y,2.72),100,(.93,.97,1),1.7)
    light(preview,"RoomFill",(0,-3.5,1.9),120,(1,.93,.84),4,(0,2,1.2))
    cameras={
        "01_Room":camera(preview,"View_Room",(4.25,-3.45,1.68),(-1.0,2.5,1.3),22),
        "02_Corridor":camera(preview,"View_Corridor",(.76,4.85,1.68),(-.15,11.65,1.40),24),
        "03_Workstations":camera(preview,"View_Workstations",(-.75,-1.9,1.65),(-3.1,2.5,1.0),32),
        "04_Door":camera(preview,"View_Door",(1.85,1.8,1.55),(0,4.05,1.25),38),
        "05_Layout":camera(preview,"View_Layout",(19,-19,26),(0.5,4.4,0),43),
    }
    scene.camera=cameras["01_Room"]
    return preview,cameras


def render_previews(preview,cameras):
    scene=bpy.context.scene
    for name,cam in cameras.items():
        log("Rendering " + name)
        scene.camera=cam
        hidden=[]
        extra=[]
        if name=="05_Layout":
            for obj in scene.objects:
                if obj.type=="MESH" and obj.get("part_category")=="ceiling":
                    obj.hide_render=True
                    hidden.append(obj)
            # This view is a roof-off documentation view, not the gameplay camera.
            extra.append(light(preview,"LayoutSoftbox",(0,3,15),3200,(1,1,1),14))
        scene.frame_set(60 if name=="04_Door" else 0)
        scene.render.filepath=str(PREVIEW/(name+".png"))
        bpy.ops.render.render(write_still=True)
        for obj in hidden:
            obj.hide_render=False
        for obj in extra:
            obj.hide_render=True
    scene.frame_set(0)
    scene.camera=cameras["01_Room"]


def main():
    if OUT.exists() and "--replace-generated" not in sys.argv:
        raise RuntimeError("Output exists. Choose a new version or explicitly rebuild own generated files.")
    for path in [OUT,TEX,PREVIEW]:
        path.mkdir(parents=True,exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene
    scene.unit_settings.system="METRIC"
    scene.unit_settings.scale_length=1
    scene.render.fps=60
    scene.frame_start=0
    scene.frame_end=60
    palette=[
        ("Plaster",(.64,.65,.61),0,.88,"paint"),
        ("PaintWhite",(.71,.73,.69),0,.57,"paint"),
        ("PaintGreen",(.23,.32,.29),0,.65,"paint"),
        ("Floor",(.37,.40,.38),0,.73,"floor"),
        ("Steel",(.42,.45,.46),.90,.34,"steel"),
        ("Rust",(.27,.12,.057),0,.94,"rust"),
        ("Rubber",(.034,.038,.035),0,.85,"rubber"),
        ("Grime",(.15,.16,.13),0,.96,"paint"),
        ("Epoxy",(.075,.085,.079),0,.29,"paint"),
        ("Ceramic",(.77,.79,.75),0,.23,"paint"),
        ("Ink",(.046,.05,.045),0,.85,"rubber"),
    ]
    for args in palette:
        material(*args)
    material("Glass",(.36,.49,.46),0,.09,"steel",.58)
    material("Water",(.12,.17,.16),0,.06,"rubber",.55)
    log("Building architecture")
    architecture()
    log("Building separated props and door")
    bench()
    sink()
    shelf()
    metal_plate()
    glass_shards()
    puddle()
    door()
    log("Consolidating geometry")
    merge_parts()
    rig_door()
    place_props()
    manifest=export_assets()
    preview,cameras=setup_preview()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"Lab_Review.blend"))
    render_previews(preview,cameras)
    # Do not create .blend1 backup files during a controlled rebuild.
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"Lab_Review.blend"))
    log("COMPLETE: "+str(sum(a["triangles"] for a in manifest["assets"]))+" triangles")


if __name__=="__main__":
    main()
