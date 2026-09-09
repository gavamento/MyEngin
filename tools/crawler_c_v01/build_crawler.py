"""Create the approved C creature, including deform rig and baked FBX actions.

Blender 4.2 background script. Writes only enemy_crawler_c_v01 assets.
"""
from pathlib import Path
import json
import math
import random
import sys

import bpy
import numpy as np
from mathutils import Vector, Matrix

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets/models/enemy_crawler_c_v01"
TEX = OUT / "textures"
PRE = OUT / "previews"
sys.path.insert(0, str(ROOT / "tools/lab_assets_v01"))
from build_lab import install_pbr_export, noise

random.seed(209)
SKIN = []
DETAIL = []
MATS = {}
BONES = {}
LIMBS = {}
RIG = None
BODY = None


def log(value):
    print("CRAWLER: " + value, flush=True)


def activate(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def ellipsoid(name, center, scale, detail=False, bone=None, material=None, orient=None):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32 if detail else 48,
                                       ring_count=20 if detail else 32, location=center)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    if orient is not None:
        obj.rotation_mode = "QUATERNION"
        obj.rotation_quaternion = Vector(orient).to_track_quat("Z", "Y")
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    for poly in obj.data.polygons:
        poly.use_smooth = True
    if detail:
        obj.data.materials.append(MATS[material])
        obj["rig_bone"] = bone
        DETAIL.append(obj)
    else:
        SKIN.append(obj)
    return obj


def tube(name, a, b, radius_a, radius_b, bulge=.10, flatten=1):
    a, b = Vector(a), Vector(b)
    direction = (b-a).normalized()
    ref = Vector((0,0,1)) if abs(direction.z)<.85 else Vector((0,1,0))
    u = direction.cross(ref).normalized()
    v = direction.cross(u).normalized()
    vertices, faces = [], []
    sides, rings = 32, 12
    for j in range(rings):
        t = j/(rings-1)
        center = a.lerp(b,t)
        radius = (radius_a*(1-t)+radius_b*t)*(1+bulge*math.sin(math.pi*t))
        for i in range(sides):
            angle = 2*math.pi*i/sides
            p = center+u*(math.cos(angle)*radius)+v*(math.sin(angle)*radius*flatten)
            vertices.append(p)
    faces.append(tuple(range(sides-1,-1,-1)))
    for j in range(rings-1):
        for i in range(sides):
            ni = (i+1)%sides
            faces.append((j*sides+i,j*sides+ni,(j+1)*sides+ni,(j+1)*sides+i))
    faces.append(tuple((rings-1)*sides+i for i in range(sides)))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices,[],faces)
    mesh.update()
    obj = bpy.data.objects.new(name,mesh)
    bpy.context.scene.collection.objects.link(obj)
    SKIN.append(obj)
    return obj


def bone(name, a, b, parent=None, deform=True):
    BONES[name] = {"head":Vector(a),"tail":Vector(b),"parent":parent,"deform":deform}


def texture(name, color, roughness, sensor=False, eye=False):
    n = 2048
    rng = np.random.default_rng(2071+len(MATS)*17)
    coarse = noise(n,13,rng)
    medium = noise(n,97,rng)
    pores = noise(n,620,rng)
    fine = rng.random((n,n),dtype=np.float32)
    yy,xx = np.mgrid[0:n,0:n].astype(np.float32)/n
    creases = np.exp(-np.square(np.sin(2*math.pi*(xx*32+.5*np.sin(yy*15))))*110)
    shade = .88+.15*coarse+.055*(medium-.5)+.025*(fine-.5)-.055*creases
    height = .02*pores+.008*fine-.026*creases
    if sensor:
        ridge = np.sin(2*math.pi*(yy*115+.55*np.sin(xx*16)))
        shade = .90+.06*coarse+.025*ridge
        height += .055*ridge
    if eye:
        shade = .96+.03*medium
        height *= .03
    rgba = np.ones((n,n,4),dtype=np.float32)
    rgba[:,:,:3] = np.clip(shade[:,:,None]*np.array(color),0,1)
    images = []
    for suffix,noncolor in [("BaseColor",False),("Normal",True),("Normal_DX",True)]:
        if suffix=="Normal":
            dx=(np.roll(height,-1,1)-np.roll(height,1,1))*2.1
            dy=(np.roll(height,-1,0)-np.roll(height,1,0))*2.1
            inv=1/np.sqrt(1+dx*dx+dy*dy)
            rgba[:,:,0]=.5-.5*dx*inv
            rgba[:,:,1]=.5-.5*dy*inv
            rgba[:,:,2]=.5+.5*inv
        elif suffix=="Normal_DX":
            rgba[:,:,1]=1-rgba[:,:,1]
        im=bpy.data.images.new(name+"_"+suffix,width=n,height=n,alpha=False)
        im.colorspace_settings.name="Non-Color" if noncolor else "sRGB"
        im.pixels.foreach_set(rgba.astype(np.float32).ravel())
        im.filepath_raw=str(TEX/(im.name+".png"))
        im.file_format="PNG"
        im.save()
        images.append(im)
    mat=bpy.data.materials.new(name)
    mat.use_nodes=True
    mat.diffuse_color=(*color,1)
    nt=mat.node_tree
    bsdf=nt.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value=(1,1,1,1)
    bsdf.inputs["Roughness"].default_value=roughness
    bsdf.inputs["Metallic"].default_value=0
    # Use only channels supported by MyEngine; no preview-only subsurface shader.
    base=nt.nodes.new("ShaderNodeTexImage")
    base.image=images[0]
    base.location=(-560,150)
    nt.links.new(base.outputs["Color"],bsdf.inputs["Base Color"])
    normaltex=nt.nodes.new("ShaderNodeTexImage")
    normaltex.name="NormalTexture"
    normaltex.image=images[1]
    normaltex.location=(-560,-160)
    normal=nt.nodes.new("ShaderNodeNormalMap")
    normal.location=(-300,-160)
    nt.links.new(normaltex.outputs["Color"],normal.inputs["Color"])
    nt.links.new(normal.outputs["Normal"],bsdf.inputs["Normal"])
    MATS[name]=mat
    log("2K textures: "+name)


def anatomy():
    bone("Root",(0,0,0),(0,0,.12),deform=False)
    bone("Pelvis",(0,.37,.41),(0,.16,.46),"Root")
    bone("Spine",(0,.16,.46),(0,-.10,.49),"Pelvis")
    bone("Chest",(0,-.10,.49),(0,-.27,.45),"Spine")
    bone("Neck",(0,-.27,.45),(0,-.43,.43),"Chest")
    bone("Head",(0,-.43,.43),(0,-.67,.39),"Neck")
    bone("Jaw",(0,-.52,.32),(0,-.72,.27),"Head")
    ellipsoid("Ribcage",(0,-.065,.455),(.19,.27,.145))
    ellipsoid("NarrowAbdomen",(0,.17,.40),(.118,.19,.094))
    ellipsoid("PelvisMass",(0,.35,.40),(.178,.155,.105))
    ellipsoid("NeckMass",(0,-.32,.438),(.095,.17,.097))
    ellipsoid("Cranium",(0,-.51,.448),(.100,.179,.114))
    ellipsoid("Midface",(0,-.64,.392),(.079,.10,.075))
    ellipsoid("Mandible",(0,-.635,.314),(.084,.123,.06))
    ellipsoid("ChinSensorBase",(0,-.70,.274),(.079,.09,.049))
    for y in np.linspace(.29,-.20,9):
        z=.56 if y<.1 else .515
        ellipsoid("SubcutaneousSpine",(0,float(y),z),(.034,.025,.023))
    for side in [-1,1]:
        s="L" if side<0 else "R"
        ellipsoid("Scapula"+s,(side*.136,-.08,.538),(.065,.126,.026))
        for y in np.linspace(-.19,.08,5):
            ellipsoid("RibContour"+s,(side*.147,float(y),.448),(.040,.016,.076))
        shoulder=(side*.19,-.20,.455)
        elbow=(side*.46,-.065,.31)
        radial=(side*.57,-.38,.195)
        wrist=(side*.49,-.695,.102)
        palm=(side*.49,-.805,.066)
        bone("UpperArm."+s,shoulder,elbow,"Chest")
        bone("ForeArm."+s,elbow,radial,"UpperArm."+s)
        bone("Carpal."+s,radial,wrist,"ForeArm."+s)
        bone("Hand."+s,wrist,palm,"Carpal."+s)
        bone("IK_Hand."+s,wrist,palm,"Root",False)
        LIMBS["Hand."+s]={"target":"IK_Hand."+s,"end":"Carpal."+s,"end_effector":"Hand."+s,"point":Vector(wrist),"phase":0 if side<0 else .5}
        for name,a,b,ra,rb in [("Upper",shoulder,elbow,.064,.034),("Fore",elbow,radial,.038,.029),("Wrist",radial,wrist,.028,.024)]:
            tube(name+s,a,b,ra,rb,.23)
            ellipsoid(name+"Joint"+s,b,(rb*1.12,rb*1.12,rb*1.12))
        ellipsoid("Deltoid"+s,shoulder,(.081,.085,.069))
        tube("Palm"+s,wrist,palm,.043,.047,.10,.42)
        for finger in range(5):
            if finger==0:
                a=Vector((side*.458,-.755,.070))
                p1=Vector((side*.386,-.815,.052))
                p2=Vector((side*.348,-.871,.028))
                p3=Vector((side*.338,-.915,.023))
                radius=.014
            else:
                spread=(finger-2.5)*.029
                length=[0,.215,.255,.243,.194][finger]
                a=Vector((side*(.49+spread),-.804,.063))
                p1=a+Vector((side*spread*.5,-length*.43,-.012))
                p2=a+Vector((side*spread*.88,-length*.78,-.036))
                p3=a+Vector((side*spread*1.1,-length,-.040))
                radius=.0145 if finger in [2,3] else .013
            points=[a,p1,p2,p3]
            parent="Hand."+s
            for j in range(3):
                bn=f"Finger{finger}_{j+1}.{s}"
                bone(bn,points[j],points[j+1],parent)
                tube(bn,points[j],points[j+1],radius*(1-j*.14),radius*(.91-j*.14),.18)
                ellipsoid("Knuckle",points[j],(radius*1.16,radius*1.20,radius))
                parent=bn
            ellipsoid("TactilePad",p3,(radius*1.25,.027,.010),True,parent,"Sensor")
            ellipsoid("Nail",p3+Vector((0,.009,.012)),(radius*.8,.022,.003),True,parent,"Keratin")
        hip=(side*.135,.335,.4)
        knee=(side*.407,.60,.335)
        hock=(side*.48,.845,.195)
        ankle=(side*.352,.955,.073)
        toe=(side*.352,1.012,.035)
        bone("Thigh."+s,hip,knee,"Pelvis")
        bone("Shin."+s,knee,hock,"Thigh."+s)
        bone("Tarsal."+s,hock,ankle,"Shin."+s)
        bone("Foot."+s,ankle,toe,"Tarsal."+s)
        bone("IK_Foot."+s,ankle,toe,"Root",False)
        LIMBS["Foot."+s]={"target":"IK_Foot."+s,"end":"Tarsal."+s,"end_effector":"Foot."+s,"point":Vector(ankle),"phase":.5 if side<0 else 0}
        for name,a,b,ra,rb in [("Thigh",hip,knee,.076,.041),("Shin",knee,hock,.040,.024),("Tarsal",hock,ankle,.025,.020)]:
            tube(name+s,a,b,ra,rb,.25)
            ellipsoid(name+"Joint"+s,b,(rb*1.15,rb*1.15,rb*1.15))
        tube("Foot"+s,ankle,toe,.037,.035,.06,.5)
        for t in range(3):
            a=Vector((side*(.352+(t-1)*.031),1.00,.041))
            mid=a+Vector((side*(t-1)*.010,.065,-.017))
            end=mid+Vector((side*(t-1)*.006,.042,-.005))
            parent="Foot."+s
            for j,(aa,bb) in enumerate([(a,mid),(mid,end)]):
                bn=f"Toe{t}_{j+1}.{s}"
                bone(bn,aa,bb,parent)
                tube(bn,aa,bb,.014-j*.002,.012-j*.002,.13)
                parent=bn
            ellipsoid("ToePad",end,(.015,.023,.010),True,parent,"Sensor")
        # Small, clouded eyes beneath substantial brow folds. No functional pupil.
        ellipsoid("Brow"+s,(side*.062,-.666,.443),(.038,.033,.019))
        ellipsoid("Cheek"+s,(side*.062,-.628,.363),(.027,.065,.034))
        ellipsoid("EyeSocket"+s,(side*.058,-.691,.415),(.014,.009,.008),True,"Head","Crease")
        ellipsoid("VestigialEye"+s,(side*.058,-.699,.415),(.007,.004,.0045),True,"Head","CloudedEye")
        ellipsoid("LidFold"+s,(side*.058,-.692,.424),(.021,.012,.007))
    ellipsoid("NasalRidge",(0,-.699,.378),(.019,.025,.036))
    ellipsoid("ClosedMouth",(0,-.732,.329),(.068,.008,.005),True,"Jaw","Crease")
    ellipsoid("ChinVibrationOrgan",(0,-.728,.263),(.068,.070,.035),True,"Jaw","Sensor")
    for j in range(7):
        ellipsoid("ChinRidge",(0,-.778+j*.013,.260),(.062-abs(j-3)*.006,.004,.031),True,"Jaw","Sensor")


def sculpt_body():
    global BODY
    bpy.ops.object.select_all(action="DESELECT")
    for obj in SKIN:
        obj.select_set(True)
    bpy.context.view_layer.objects.active=SKIN[0]
    bpy.ops.object.join()
    BODY=bpy.context.object
    BODY.name="CrawlerC_Skin"
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    BODY.data.remesh_voxel_size=.004
    BODY.data.use_remesh_preserve_volume=True
    log("Fusing continuous skin")
    bpy.ops.object.voxel_remesh()
    smooth=BODY.modifiers.new("TissueSmoothing","SMOOTH")
    smooth.factor=.62
    smooth.iterations=14
    bpy.ops.object.modifier_apply(modifier=smooth.name)
    decimate=BODY.modifiers.new("GameTopology","DECIMATE")
    decimate.ratio=.18
    bpy.ops.object.modifier_apply(modifier=decimate.name)
    BODY.data.update()
    # Shallow folds around bending joints are geometry, so they survive FBX shading.
    folds=[BONES[n] for n in BONES if n.startswith(("ForeArm.","Carpal.","Shin.","Tarsal.","Neck"))]
    for vertex in BODY.data.vertices:
        p=vertex.co.copy()
        depth=0.
        for joint in folds:
            axis=(joint["tail"]-joint["head"]).normalized()
            delta=p-joint["head"]
            axial=delta.dot(axis)
            distance=delta.length
            envelope=math.exp(-((distance/.08)**2))*math.exp(-((axial/.036)**2))
            depth+=.0016*envelope*(.5+.5*math.cos(axial*490))**6
        vertex.co-=vertex.normal*depth
    BODY.data.update()
    for p in BODY.data.polygons:
        p.use_smooth=True
    BODY.data.materials.clear()
    BODY.data.materials.append(MATS["Skin"])
    log("Skin surface: "+str(len(BODY.data.polygons))+" faces")
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=1.05,island_margin=.006,area_weight=.6)
    bpy.ops.object.mode_set(mode="OBJECT")
    BODY.data.uv_layers.active.name="UV0"


def make_rig():
    global RIG
    data=bpy.data.armatures.new("CrawlerC_Skeleton")
    RIG=bpy.data.objects.new("CrawlerC_Rig",data)
    bpy.context.scene.collection.objects.link(RIG)
    activate(RIG)
    bpy.ops.object.mode_set(mode="EDIT")
    for name,info in BONES.items():
        b=data.edit_bones.new(name)
        b.head=info["head"]
        b.tail=info["tail"]
        b.use_deform=info["deform"]
        if info["parent"]:
            b.parent=data.edit_bones[info["parent"]]
    bpy.ops.object.mode_set(mode="OBJECT")
    for b in RIG.pose.bones:
        b.rotation_mode="XYZ"
    for name,limb in LIMBS.items():
        ik=RIG.pose.bones[limb["end"]].constraints.new("IK")
        ik.name="SurfaceReach"
        ik.target=RIG
        ik.subtarget=limb["target"]
        ik.chain_count=3
        ik.use_stretch=False
        ik.iterations=80
        copy=RIG.pose.bones[limb["end_effector"]].constraints.new("COPY_ROTATION")
        copy.target=RIG
        copy.subtarget=limb["target"]
        copy.owner_space="POSE"
        copy.target_space="POSE"
    log("Binding continuous skin")
    deform=[name for name,info in BONES.items() if info["deform"]]
    points=np.array([v.co for v in BODY.data.vertices],dtype=np.float32)
    distances=[]
    for name in deform:
        a=np.array(BONES[name]["head"],dtype=np.float32)
        b=np.array(BONES[name]["tail"],dtype=np.float32)
        d=b-a
        t=np.clip(((points-a)*d).sum(axis=1)/(d*d).sum(),0,1)
        distances.append(np.linalg.norm(points-a-t[:,None]*d,axis=1))
    distances=np.array(distances).T
    # Limit blending to the nearest bone's anatomical neighbours, avoiding crossed limbs.
    nearest=distances.argmin(axis=1)
    allowed=np.zeros((len(deform),len(deform)),dtype=bool)
    for i,name in enumerate(deform):
        allowed[i,i]=True
        parent=BONES[name]["parent"]
        for j,other in enumerate(deform):
            if other==parent or BONES[other]["parent"]==name:
                allowed[i,j]=True
    distances=np.where(allowed[nearest],distances,np.inf)
    order=np.argsort(distances,axis=1)[:,:4]
    best=np.take_along_axis(distances,order,axis=1)
    weights=np.exp(-(best-best[:,0:1])/.014)
    weights/=weights.sum(axis=1,keepdims=True)
    groups=[BODY.vertex_groups.new(name=name) for name in deform]
    for vi in range(len(points)):
        for idx,w in zip(order[vi],weights[vi]):
            if w>0:
                groups[int(idx)].add([vi],float(w),"REPLACE")
    BODY.parent=RIG
    mod=BODY.modifiers.new("SkinDeform","ARMATURE")
    mod.object=RIG
    for obj in DETAIL:
        group=obj.vertex_groups.new(name=obj["rig_bone"])
        group.add(list(range(len(obj.data.vertices))),1,"REPLACE")
        obj.parent=RIG
        mod=obj.modifiers.new("SkinDeform","ARMATURE")
        mod.object=RIG
        obj.data.uv_layers.active.name="UV0"
    # Keep separate eyes and sensory surfaces editable, but merge by material for export.
    by_material={mat:[o for o in DETAIL if o.data.materials[0].name==mat]
                 for mat in ["Sensor","Keratin","Crease","CloudedEye"]}
    for mat,objects in by_material.items():
        activate(objects[0])
        for obj in objects:
            obj.select_set(True)
        bpy.ops.object.join()
        bpy.context.object.name="CrawlerC_"+mat


def move(name, world_delta):
    b=RIG.pose.bones[name]
    b.location=RIG.data.bones[name].matrix_local.to_3x3().inverted()@Vector(world_delta)


CLIPS=[
    ("00_Idle",180,True), ("01_Patrol_Crawl",144,True), ("02_Alert",60,False),
    ("03_Search",180,True), ("04_Chase",54,True), ("05_Attack",72,False),
    ("06_Hit",60,False), ("07_LightFlinch",90,False), ("08_Death",120,False),
    ("09_LightBoundary",144,True), ("10_WallCrawl",120,True),
]


def animation_pose(clip,t):
    for b in RIG.pose.bones:
        b.location=(0,0,0)
        b.rotation_euler=(0,0,0)
        b.scale=(1,1,1)
    breath=math.sin(t*2*math.pi)
    move("Chest",(0,0,.003*breath))
    if clip in {"01_Patrol_Crawl","03_Search","04_Chase","09_LightBoundary","10_WallCrawl"}:
        stride={"01_Patrol_Crawl":.20,"03_Search":.11,"04_Chase":.37,"09_LightBoundary":.13,"10_WallCrawl":.24}[clip]
        duty=.72 if clip!="04_Chase" else .57
        for name,limb in LIMBS.items():
            p=(t+limb["phase"])%1
            if p<duty:
                forward=-stride/2+stride*p/duty
                lift=0
            else:
                swing=(p-duty)/(1-duty)
                forward=stride/2-stride*(swing*swing*(3-2*swing))
                lift=(.07 if clip!="04_Chase" else .10)*math.sin(math.pi*swing)**1.4
            dx=forward if clip=="09_LightBoundary" else 0
            dy=0 if clip=="09_LightBoundary" else forward
            move(limb["target"],(dx,dy,lift))
        move("Pelvis",(.008*math.sin(2*math.pi*t),0,.008*math.cos(4*math.pi*t)))
        RIG.pose.bones["Spine"].rotation_euler.y=.035*math.sin(2*math.pi*t)
        if clip=="03_Search":
            RIG.pose.bones["Head"].rotation_euler.z=.28*math.sin(2*math.pi*t)
            RIG.pose.bones["Jaw"].rotation_euler.x=.035*math.sin(8*math.pi*t)
        if clip=="04_Chase":
            move("Chest",(0,-.025,.008*math.cos(4*math.pi*t)))
        if clip=="10_WallCrawl":
            move("Pelvis",(0,0,-.025))
            RIG.pose.bones["Head"].rotation_euler.x=-.12
        if clip=="09_LightBoundary":
            RIG.pose.bones["Head"].rotation_euler.z=-.25
    elif clip=="02_Alert":
        hold=min(1,t/.16)
        move("Chest",(0,0,.018*hold))
        RIG.pose.bones["Head"].rotation_euler.x=-.15*hold
        RIG.pose.bones["Head"].rotation_euler.z=.16*hold
    elif clip=="05_Attack":
        lunge=math.sin(math.pi*min(1,t/.75))**2 if t<.75 else 0
        move("Chest",(0,-.14*lunge,.05*lunge))
        move("Head",(0,-.055*lunge,0))
        RIG.pose.bones["Jaw"].rotation_euler.x=.23*lunge
        for s in ["L","R"]:
            move("IK_Hand."+s,(0,-.19*lunge,.075*lunge))
    elif clip=="06_Hit":
        recoil=math.sin(math.pi*t)**2
        move("Chest",(.04*recoil,.06*recoil,-.025*recoil))
        RIG.pose.bones["Head"].rotation_euler.z=.35*recoil
        RIG.pose.bones["Spine"].rotation_euler.y=.17*recoil
    elif clip=="07_LightFlinch":
        envelope=min(1,t/.15)*max(0,min(1,(1-t)/.30))
        move("Pelvis",(0,.065*envelope,-.045*envelope))
        move("Chest",(0,.10*envelope,-.01*envelope))
        RIG.pose.bones["Head"].rotation_euler.x=.34*envelope
        for s,sign in [("L",-1),("R",1)]:
            move("IK_Hand."+s,(-sign*.25*envelope,.10*envelope,.255*envelope))
    elif clip=="08_Death":
        fall=min(1,t/.65)
        fall=fall*fall*(3-2*fall)
        move("Pelvis",(.05*fall,0,-.12*fall))
        move("Chest",(.055*fall,0,-.08*fall))
        RIG.pose.bones["Spine"].rotation_euler.y=.25*fall
        RIG.pose.bones["Head"].rotation_euler.z=.20*fall
    else:
        RIG.pose.bones["Head"].rotation_euler.z=.02*breath
        RIG.pose.bones["Jaw"].rotation_euler.x=.02*math.sin(4*math.pi*t)


def animate():
    scene=bpy.context.scene
    RIG.animation_data_create()
    for name,length,loop in CLIPS:
        action=bpy.data.actions.new(name)
        action.use_fake_user=True
        RIG.animation_data.action=action
        for frame in range(length+1):
            animation_pose(name,frame/length)
            for b in RIG.pose.bones:
                b.keyframe_insert("location",frame=frame,group=b.name)
                b.keyframe_insert("rotation_euler",frame=frame,group=b.name)
        for fc in action.fcurves:
            for key in fc.keyframe_points:
                key.interpolation="LINEAR"
        action["loop"]=loop
        log("Animation: "+name)
    RIG.animation_data.action=bpy.data.actions["00_Idle"]
    scene.frame_set(0)


def meshes():
    return [o for o in bpy.context.scene.objects if o.type=="MESH" and o.parent==RIG]


def export():
    install_pbr_export()
    for name,mat in MATS.items():
        mat.node_tree.nodes["NormalTexture"].image=bpy.data.images[name+"_Normal_DX"]
    activate(RIG)
    for obj in meshes():
        obj.select_set(True)
    bpy.context.scene.frame_set(0)
    bpy.ops.export_scene.fbx(filepath=str(OUT/"Enemy_Crawler_C.fbx"),use_selection=True,
        object_types={"MESH","ARMATURE"},axis_forward="-Z",axis_up="Y",
        apply_unit_scale=True,apply_scale_options="FBX_SCALE_UNITS",use_space_transform=True,
        bake_space_transform=False,mesh_smooth_type="FACE",use_mesh_modifiers=True,
        use_triangles=True,add_leaf_bones=False,use_armature_deform_only=True,
        bake_anim=True,bake_anim_use_all_actions=True,bake_anim_use_nla_strips=False,
        bake_anim_simplify_factor=0,bake_anim_step=1,path_mode="RELATIVE",embed_textures=False)
    for name,mat in MATS.items():
        mat.node_tree.nodes["NormalTexture"].image=bpy.data.images[name+"_Normal"]
    vertices=[obj.matrix_world@v.co for obj in meshes() for v in obj.data.vertices]
    lo=[min(p[i] for p in vertices) for i in range(3)]
    hi=[max(p[i] for p in vertices) for i in range(3)]
    manifest={"file":"Enemy_Crawler_C.fbx","units":"meters","fps":60,
              "source_forward":"-Y","engine_forward":"-Z","engine_up":"+Y",
              "rest_bounds_blender":{"min":lo,"max":hi},
              "deform_bones":sum(i["deform"] for i in BONES.values()),
              "clips":[{"name":"CrawlerC_Rig|"+n,"duration_seconds":l/60,"loop":loop,
                        "root_motion":False} for n,l,loop in CLIPS],
              "wall_clip":"Orient the entity to the wall normal; local support plane is XY in Blender / XZ in engine.",
              "materials":{n:{"metallic":0,"roughness":m.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value,
                               "color":"textures/"+n+"_BaseColor.png","normal":"textures/"+n+"_Normal_DX.png"}
                           for n,m in MATS.items()}}
    (OUT/"asset_manifest.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
    log("FBX exported")


def preview_setup():
    scene=bpy.context.scene
    scene.render.engine="CYCLES"
    scene.cycles.samples=40
    scene.cycles.use_denoising=True
    scene.render.resolution_x=1600
    scene.render.resolution_y=1000
    scene.render.resolution_percentage=100
    scene.world.use_nodes=True
    scene.world.node_tree.nodes["Background"].inputs[0].default_value=(.19,.21,.22,1)
    scene.world.node_tree.nodes["Background"].inputs[1].default_value=.3
    scene.view_settings.view_transform="AgX"
    scene.view_settings.look="AgX - Medium High Contrast"
    scene.view_settings.exposure=.2
    coll=bpy.data.collections.new("PREVIEW_ONLY")
    scene.collection.children.link(coll)
    bpy.ops.mesh.primitive_plane_add(size=200)
    ground=bpy.context.object
    ground.name="PreviewGround"
    ground.location.z=-.005
    mat=bpy.data.materials.new("PreviewGroundMaterial")
    mat.diffuse_color=(.095,.11,.12,1)
    mat.use_nodes=True
    mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value=(.095,.11,.12,1)
    mat.node_tree.nodes["Principled BSDF"].inputs["Roughness"].default_value=.8
    ground.data.materials.append(mat)
    for c in list(ground.users_collection): c.objects.unlink(ground)
    coll.objects.link(ground)
    for name,loc,target,energy,color,size in [
        ("Key",(1,-2.5,3),(0,0,.3),210,(1,.98,.96),2.8),
        ("Fill",(-2,-.4,1.5),(0,0,.3),120,(.79,.88,1),2.4),
        ("Rim",(.5,2,2),(0,0,.3),240,(.86,.96,1),2.0)]:
        light=bpy.data.lights.new(name,"AREA")
        light.energy=energy
        light.color=color
        light.shape="DISK"
        light.size=size
        obj=bpy.data.objects.new(name,light)
        coll.objects.link(obj)
        obj.location=loc
        obj.rotation_euler=(Vector(target)-obj.location).to_track_quat("-Z","Y").to_euler()
    cameras={}
    for name,pos,target,lens in [
        ("01_ThreeQuarter",(2.25,-3.1,1.55),(0,-.05,.28),52),
        ("02_Front",(0,-3.2,.96),(0,0,.27),49),
        ("03_Side",(3.6,-.15,1.05),(0,.05,.30),53),
        ("04_HeadDetail",(.53,-1.57,.69),(0,-.65,.36),60),
    ]:
        data=bpy.data.cameras.new(name)
        obj=bpy.data.objects.new(name,data)
        coll.objects.link(obj)
        obj.location=pos
        obj.rotation_euler=(Vector(target)-obj.location).to_track_quat("-Z","Y").to_euler()
        data.lens=lens
        data.clip_start=.01
        scene.camera=obj
        cameras[name]=obj
    return cameras


def render(cameras):
    scene=bpy.context.scene
    RIG.animation_data.action=bpy.data.actions["00_Idle"]
    scene.frame_set(0)
    for name,cam in cameras.items():
        scene.camera=cam
        scene.render.filepath=str(PRE/(name+".png"))
        log("Rendering "+name)
        bpy.ops.render.render(write_still=True)
    scene.camera=cameras["01_ThreeQuarter"]


def main():
    if OUT.exists() and "--rebuild" not in sys.argv:
        raise RuntimeError("Output exists; use a new version instead of replacing user work")
    for p in [OUT,TEX,PRE]: p.mkdir(parents=True,exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    scene=bpy.context.scene
    scene.unit_settings.system="METRIC"
    scene.unit_settings.scale_length=1
    scene.render.fps=60
    scene.frame_start=0
    scene.frame_end=180
    bpy.context.preferences.filepaths.save_version=0
    for args in [("Skin",(.47,.485,.46),.67), ("Sensor",(.25,.265,.235),.80,True),
                 ("Keratin",(.30,.29,.255),.49), ("Crease",(.10,.095,.087),.84),
                 ("CloudedEye",(.68,.70,.67),.23,False,True)]:
        texture(*args)
    log("Sculpting anatomy")
    anatomy()
    sculpt_body()
    make_rig()
    animate()
    export()
    cameras=preview_setup()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"CrawlerC_Review.blend"))
    render(cameras)
    bpy.ops.file.make_paths_relative()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"CrawlerC_Review.blend"))
    log("BUILD COMPLETE")


if __name__=="__main__":
    main()
