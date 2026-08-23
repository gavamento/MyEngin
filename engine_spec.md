# Custom Game Engine Specification

- **Engine Name**: [TBD]
- **Version**: Draft 0.1
- **Author**: Gavament
- **Last Updated**: 2026-07-19

---

## 1. Overview

### 1.1 Purpose

Develop a custom game engine in C++ / DirectX 11 with a Unity-style architecture.

The primary purpose of this engine is to serve as a job-hunting portfolio project and demonstrate the following capabilities to evaluators:

- Game engine architecture design skills (layer separation, dependency management, and data-oriented design)
- Low-level technical expertise (DirectX 11, Compute Shaders, DLL hot reloading, and SIMD)
- Commitment to quality assurance (mechanisms that guarantee consistent behavior between Debug and Release builds)
- Ability to articulate and explain design decisions (through this specification and the Architecture Decision Records)

### 1.2 Concept

> **“Unity’s usability × ECS performance × A development experience that does not break”**

- **Unity-style API**: Provide APIs and editor tools such as GameObject, Component, and Inspector that are intuitive for developers with Unity experience
- **ECS storage**: Store internal data in an archetype-based ECS and process it in cache-efficient batches
- **A development experience that does not break**: Support hot reloading for shaders, assets, C++ code, and scene data, allowing development to continue without restarting the engine. Eliminate behavioral differences between Debug and Release builds

### 1.3 Intended Audience

- Recruiters and technical interviewers (to review design decisions and their rationale)
- The developer (as an implementation reference)

### 1.4 Out of Scope

To prioritize the completeness of the project as a portfolio piece, the following items are excluded from the initial scope:

- Cross-platform support (Windows only)
- DirectX 12 / Vulkan support (the abstraction layer will only be designed with future extensibility in mind)
- Networking features
- A custom 3D physics engine ([TBD]: implement only simple collision detection / integrate an external library / exclude entirely)
- Embedded scripting languages such as Lua — scripts will be written in C++ as a logic DLL

---

## 2. Runtime and Build Environment

| Item | Specification |
|---|---|
| Target OS | Windows 10 / 11 (x64) |
| Development Environment | Visual Studio 2022 |
| Language Standard | C++20 [TBD: whether to use C++17 instead] |
| Graphics API | DirectX 11 (Feature Level 11_0) |
| Shaders | HLSL (Shader Model 5.0, including Compute Shaders) |
| GUI | Dear ImGui (docking branch) |
| Build System | [TBD: native Visual Studio solution / CMake / Premake] |

### 2.1 Build Configurations

| Configuration | Purpose | Notes |
|---|---|---|
| Debug | Development and debugging | No optimization; all validation enabled |
| Release | Distribution and performance measurement | Optimization enabled |

The behavioral consistency policy for both configurations is defined in “11. Debug/Release Consistency.”

---

## 3. Architecture

### 3.1 Layer Structure

Higher-level layers may depend only on lower-level layers. Reverse dependencies are prohibited.

```
┌─────────────────────────────────────┐
│ Editor      (ImGui editor and tool windows)      │
├─────────────────────────────────────┤
│ GameLogic   (user scripts — hot-reloadable DLL)  │
├─────────────────────────────────────┤
│ Engine      (scenes, GameObject API, particles,  │
│              asset management, hot-reload control)│
├─────────────────────────────────────┤
│ Renderer    (DX11 abstraction, render passes,    │
│              shader management)                  │
├─────────────────────────────────────┤
│ Core        (ECS, math, memory, jobs, logging,   │
│              file monitoring, serialization)     │
├─────────────────────────────────────┤
│ Platform    (Win32, input, time, DLL loading)    │
└─────────────────────────────────────┘
```

### 3.2 Module and Build Artifact Structure

| Artifact | Type | Contents |
|---|---|---|
| Engine.lib | Static library | Platform / Core / Renderer / Engine layers |
| Editor.exe | Executable | Engine and editor; host process during development |
| GameLogic.dll | Dynamic library | User scripts. **Target of hot reloading** |
| Runtime.exe | Executable | Standalone game runtime without the editor (for distribution) [TBD: whether to include in the initial scope] |

**Design Decision**: Keep the engine itself statically linked and restrict hot reloading to GameLogic.dll. Compared with making the entire engine a DLL, this significantly reduces the complexity of preserving state and maintaining vtable compatibility during reloads.

---

## 4. Object Model (Hybrid ECS)

### 4.1 Policy

**Use a Unity-style API as the external interface and an archetype-based ECS as the internal storage layer.**

| Layer | Public Representation | Actual Implementation |
|---|---|---|
| API layer | `GameObject` / `GetComponent<T>()` | Lightweight handle wrapping an EntityID |
| Storage layer | Private | SoA arrays organized by archetype |

**Design Decision**: Combine the development efficiency of a Unity-compatible workflow with the cache efficiency and batch-processing performance of an ECS. Based on experience with the pure ECS implementation used in the previous project, BOMB DRIFT, the handle-based facade addresses usability issues in the API.

### 4.2 EntityID (Generational Handle)

```cpp
struct EntityID {
    uint32_t index;      // Slot index
    uint32_t generation; // Generation number, incremented when the slot is reused
};
```

- Slots belonging to destroyed entities may be reused, but their generation number is incremented, allowing stale handles to be detected as invalid
- `GameObject::operator bool()` can be used to check whether the object is still alive, reproducing Unity-style null checks after `Destroy`
- Access to invalid handles uses the same error handling in both Debug and Release builds. Logic must not branch through Debug-only assertions, in accordance with the consistency policy in Chapter 11

### 4.3 GameObject API (Excerpt)

```cpp
GameObject obj = scene.CreateGameObject("Player");
obj.AddComponent<MeshRenderer>();
Transform* tf = obj.GetComponent<Transform>();
obj.Destroy();                  // Actual deletion is deferred until the end of the frame
if (obj) { /* Check whether the object is alive */ }
```

- `GetComponent<T>` resolution: EntityID → archetype reference → component array offset. Time complexity is O(1) through the entity record table
- Returned pointers are valid **only within the current frame**, because moving an entity between archetypes may invalidate them. EntityID must be used for references retained across frames

### 4.4 Transform Hierarchy

- Parent-child relationships are represented by a `Hierarchy` component containing a parent EntityID and a child list
- World matrices are updated every frame by iterating from the beginning of a **linear array sorted by hierarchy depth**. Recursion is not used, prioritizing cache efficiency
- Parent changes through `SetParent` are applied together during the structural-change phase at the end of the frame

### 4.5 Deferred Structural Changes

Operations that alter archetype composition, including AddComponent, RemoveComponent, Destroy, and SetParent, are queued in a command buffer and applied together at the end of the frame.

- Prevents iterator invalidation during update processing
- Makes the application order deterministic, contributing to Debug/Release consistency and replay reproducibility

---

## 5. Update Model (Two-Layer Structure)

### 5.1 Policy

| Layer | Target | Execution Model |
|---|---|---|
| System layer | Built-in engine features such as Transform updates, particles, rendering, and animation | Each system processes archetypes in batches using a data-oriented approach |
| Script layer | User scripts containing game logic | MonoBehaviour-style lifecycle functions such as `Start()` and `Update()` |

**Design Decision**: A faithful Unity-style implementation in which every component has a virtual `Update()` would undermine the cache efficiency of ECS storage. Therefore, built-in features, which account for most processing, are handled in system-level batches, while only game logic that requires flexible authoring uses a MonoBehaviour-style model.

### 5.2 Script Component Structure (Designed for Hot Reloading)

**Separate data from logic.**

- Script state, equivalent to member variables, is stored as reflection-registered POD data in the **engine-side ECS storage**
- GameLogic.dll contains only logic functions such as Update and does not own state
- As a result, script state does not need to be backed up and restored when the DLL is reloaded (see Section 8.4)

```cpp
// Example code in GameLogic.dll
struct PlayerController : Script<PlayerController> {
    // State stored on the engine side and registered for reflection
    float moveSpeed = 5.0f;
    int   jumpCount = 0;

    void Update(UpdateContext& ctx) {
        // Logic stored in the DLL and replaced during reload
    }
};
REGISTER_SCRIPT(PlayerController, FIELDS(moveSpeed, jumpCount));
```

### 5.3 Frame Phases

```
1. Update time / acquire input
2. Detect and apply hot reloads at a safe point
3. Script layer: Start for newly created scripts → Update
4. System layer: animation → particles → Transform hierarchy update
5. Script layer: LateUpdate
6. Rendering through Renderer
7. Apply all structural changes
8. Render ImGui / Present
```

- [TBD] Introduce a fixed timestep equivalent to FixedUpdate. This is recommended if replay reproducibility in Section 11.3 is prioritized
- Script execution order within the same phase is deterministic and follows registration order. Undefined ordering is not permitted under the consistency policy

---

## 6. Renderer Specification

### 6.1 Scope

Resolved: **both paths are implemented and switchable at runtime** (View > Render Path).
The original Option A / Option B trade-off was settled by sharing the lighting functions
in `common.hlsli` between the two paths, which keeps their output visually identical while
letting the Deferred path carry the effects that need a G-Buffer. See ADR-007.

- **Forward** — opaque + transparent + particles in one pass. Also used as the transparent
  tail of the Deferred path, so the two never diverge in lighting
- **Deferred** — 5 render targets (albedo / world normal / world position /
  metal-roughness-emissive / screen-space velocity, M55c).
  SSAO, screen-space effects and the ray-traced lanes of §6.4 hook in here
  - **Velocity (RT4, `R16G16_FLOAT`)** holds `current UV - previous UV`, so a consumer
    reads history at `uv - velocity`. The previous position comes from a per-view store of
    the world matrix that was **actually drawn last frame** — not the previous *tick*
    snapshot used for render interpolation, which would overshoot by up to one tick.
    The current clip position is jittered (§ TAA), so the jitter is subtracted back out
    before differencing; the stored previous view-projection is jitter-free.
    **Not covered in v1**: there is no previous-frame bone palette, so a skinned mesh
    reports only camera and object-transform motion — the bone deformation itself
    contributes zero velocity. Consumed by TAA (M55d), motion blur (M55e) and the
    ray-traced temporal lane (M55f).
    `View > Rendering > Velocity Buffer` (or `--velocity-debug`) visualizes it

**Temporal antialiasing (M55d, default off).** `CameraPostFx.taaOn` — or the global
`View > Rendering > TAA` / `--taa` — turns the camera jitter and the TAA resolve on *together*.
They are deliberately one switch: jittering without resolving is just a frame that shakes by half
a pixel. The resolve runs at the head of the post-processing chain, ahead of depth of field, and
blends the current HDR frame with the previous TAA result reprojected through the velocity buffer
(`prevUv = uv - velocity`, so the jitter is already subtracted out). The reprojected history is
clamped into the colour box formed by the current 3x3 neighbourhood, which is what throws history
away on a pixel whose occluder has just moved. History is keyed by **view key**, not by
resolution, so the Scene view and the Game view cannot eat each other's history at equal sizes;
it is dropped when the draw serial skips or the view resizes, and a dropped history emits the
current frame unchanged (the first frames of a capture therefore behave, rather than needing a
converged history).
**Not covered in v1**: TAA is Deferred-only, since velocity is a G-Buffer target. Asking for it
on the Forward path is a no-op down to the jitter, so that image stays bit-identical.

**Motion blur (M44d, extended in M55e, default off).** `CameraPostFx.motionBlurIntensity` — or the
global `--motion-blur N` — smears each pixel along its screen-space velocity with an 8-tap average,
clamped to `mbMaxPixels`. The velocity source is chosen **per pixel**: a pixel that has G-Buffer
geometry (`depth < 1`) reads the velocity target, which already carries camera *and* object motion,
so a spinning object blurs under a stationary camera; every other pixel — the sky, the background,
and the whole Forward path — falls back to the M44d depth reprojection, which is camera-only.
The fallback is not a leftover: the background never writes the G-Buffer, so its velocity stays
zero, and reading it there would freeze the sky while the camera pans. Both branches use the
jitter-free projection at both ends. Motion blur is forced off in the Scene view, because a smear
that follows the editor camera fights with editing.

**Volumetric fog (froxel, M57, default off).** `CameraPostFx.froxelOn` — or the global
`--froxel` — fills a 160x90x64 view-aligned 3D grid (exponentially distributed slices, jittered
per frame and reprojected against the previous frame) with the participating medium and the
in-scattering of every local light, sampling the same shadow atlas the surfaces do, so a spot
light casts a shadowed shaft. A second pass integrates each Z column front-to-back into
`(accumulated in-scatter, transmittance)` and every surface that has a depth composites it as
`scene * T + inScatter`: the Deferred light pass (opaque, `t15`), the Forward family
(`forward_lit` / `_instanced` / `_skinned` / `_terrain`, `t7` — which is also what the Deferred
transparent tail runs), the skybox, and CPU particles (`t3`). Additive particles get the
transmittance **only**: the surface behind them already added the in-scatter once, so adding it
again would scale the fog with the number of overlapping billboards. Pixels with no depth at all
(the skybox, and the clear-colour background of a scene without one) sample the far end of the
grid, which is what keeps the horizon from stepping between the fogged ground and an unfogged sky.

**Three separate mechanisms model atmospheric scattering, so M57d split them by range instead
of summing them** — added naively the fog is applied three times over:

| Mechanism | Owns | Behaviour when the froxel volume is on |
|---|---|---|
| Froxel volume (M57) | `[near, grid far]` — the range where beams, local lights and shadowed shafts are actually visible | The only source of fog in that range |
| `ApplyFog` (`common.hlsli`, M29d distance fog + M43a height fog / sun in-scatter) | Everything beyond the grid | Its **origin is pushed out** to the point where the view ray leaves the grid, so the two ranges never overlap. For a surface inside the grid that pushed-out origin *is* the surface, which makes the call exactly the identity |
| God rays (`postfx_godray_*`, M43b) | A screen-space radial blur of a sky-only occlusion mask | **Automatically disabled.** It is the low-spec simplification of the froxel volume — the same phenomenon with a cruder occluder — so running both counts the sun's scattering twice |

The hand-off fraction is a pure function shared by the C++ mirror and the HLSL, and
`RenderSelfTest` asserts that the two ranges add up to the whole ray with neither a gap nor an
overlap; a golden screenshot (`demo_render_froxel`, local-only at tol=0, the same treatment
FXAA and TAA get) pins the composited image.
**Not covered in v1**: an orthographic view skips the grid entirely, because the froxel depth
slices assume a perspective frustum; the GPU particle backend has never applied any fog and is
left alone; the analytic fog beyond the grid is *not* applied to the sky (it never was before
M57, and adding it would sink a skybox into flat fog colour as soon as the density rises); and
distortion particles (`blendMode=2`) write UV offsets rather than colour, so there is nothing to
attenuate.

### 6.2 Feature Scope

| Feature | Status | Notes |
|---|---|---|
| Static mesh rendering | Implemented | Frustum culling + instancing |
| Materials / shader variants | Implemented | `.mat.json`, hot-reloadable |
| Textures (2D / mipmaps) | Implemented | GUID-keyed, async decode, BCn cooking |
| Lighting | Implemented | Directional / Point / Spot (max 16) + ambient, Cook-Torrance PBR |
| Particle rendering | Implemented | See Chapter 7 |
| Shadow mapping | Implemented | 3-cascade CSM with PCF |
| Post-processing | Implemented | HDR, bloom, tonemap, FXAA, TAA, DoF, motion blur, auto-exposure, LUT |
| Skeletal animation | Implemented | 128-bone palette, glTF / FBX skinning |
| Image-based lighting | Implemented | Irradiance + prefiltered specular + BRDF LUT |
| Ray-traced secondary rays | Implemented | See §6.4 (default off) |
| Decals (projector boxes) | Implemented | See §6.6. **Deferred path only** in v1 |
| Hierarchical Z-buffer (min-Z pyramid) | Implemented | See §6.7. **Deferred path only**, built on demand |
| Reflection probe capture | Implemented | See §6.9. Explicit bake only; nothing consumes it until M56f |
| Volumetric fog (froxel) | Implemented | 160x90x64 view grid + local lights + shadow atlas, temporally reprojected. Composited on opaque / transparent / terrain / sky / CPU particles in both paths, default off (§6.1) |

### 6.3 DirectX 11 Abstraction

- The device and context are encapsulated within the Renderer layer. Raw DirectX 11 types are not exposed to the Engine layer or higher layers
- Rendering follows a “collect render items → sort → submit” model. No immediate-mode rendering API is provided, preserving room for future multithreading and graphics API replacement

### 6.4 Hybrid Ray Tracing (default off)

Primary visibility stays rasterized; the **secondary rays** are replaced by a compute-shader
ray tracer that walks a software BVH. `GraphicsDevice` requires only Feature Level 11_0, so
DXR / `RayQuery` are unavailable — the traversal is written by hand in `cs_5_0`.
Design rationale and measured cost: **ADR-009**.

| Lane | Replaces | Resolution | Denoiser |
|---|---|---|---|
| Diffuse GI (1 spp, cosine-importance + NEE) | The diffuse environment term (IBL irradiance / constant ambient) | 1/2 | Temporal accumulation → variance estimation → A-Trous ×3 |
| Directional shadow (sun cone, 0.265°) | The CSM lookup | Full | Separable spatial filter only (no history — shadows must not ghost) |
| Specular reflection (GGX VNDF, 1 ray) | The prefiltered IBL specular term, fading back to IBL above roughness 0.6 | 1/2 | Same chain as GI, tuned shorter (history 8, A-Trous ×2) |

- **Acceptance criterion**: with every lane off, the output is *bit-identical* to the build
  before the ray tracer existed. Each milestone verified this by comparing screenshots
  with `fc /b` against the previous commit's binary, on both render paths
- **Scene representation**: BLAS reuses the deterministic median-split builder from the
  physics mesh collider; only the TLAS is new. Instances carry `worldToLocal` only and the
  ray direction is deliberately left unnormalized so `t` stays in world units
- **Radiometry**: the GI and reflection buffers hold *demodulated incident radiance*
  (no albedo applied), which puts them in the same units as the IBL terms they replace.
  This is what makes the swap free of brightness steps
- **Emissive surfaces are area lights.** `Material::emissiveIntensity` (a scalar multiplying
  the base color) drives both the raster look — packed into the free `b` channel of the
  metal-roughness G-Buffer, normalized by `kEmissiveMaxIntensity` — and `RtMaterial::emissive`,
  which the path tracer picks up on bounce hits
- **Determinism**: this is a render-only lane. Randomness is a stateless PCG3D hash of
  (pixel, frame index), never read back to the CPU, and never hashed into the world state —
  the same exemption ADR-008 grants GPU particles
- **Reprojection (M55f)**: the temporal lane reads its history at `uv - velocity` (§6.2 RT4),
  which carries camera *and* object motion, so a moving object no longer drags its history
  behind it. Where the velocity target is unavailable — the frame that has no previous-frame
  matrices — it falls back to the original M46d path, projecting the current world position
  through the previous view-projection (camera motion only). The disocclusion test still
  compares the *current* world position's distance to the previous camera, because a 2D
  velocity cannot restore the previous camera distance: an object that changes its distance
  to the camera by more than 5% in one frame drops its history and falls back to 1 spp
- **Not covered in v1**: skinned meshes and transparents are absent from the BVH, secondary
  hits shade from material constants only (no bindless textures), and local lights cast no
  ray-traced shadows

### 6.5 Deliberate Non-Goals of the Rendering Roadmap (M54-M58)

The M54-M58 roadmap (`plans/radiant-shimmering-lumen.md`) adds local-light shadows, TAA,
decals, SSR, reflection probes, froxel volumetrics and terrain. Three neighbouring features
were considered and **deliberately left out**. They are non-goals of v1, not oversights.

| Non-goal | Why it is out |
|---|---|
| **Ray-traced shadows for local lights** | The RT lane is default-off *and* excluded from the screenshot regression (`tools\shot_verify.bat`: the RT demo is too slow under WARP), so the feature would carry permanently zero automated coverage. The M54 shadow atlas produces the same image on a lane CI does exercise. The §6.4 v1 limitation "local lights cast no ray-traced shadows" therefore stands. |
| **Diffuse SH probe grid** | Two implementations of diffuse ambient already exist (IBL irradiance, and RT diffuse GI + SVGF). An SH grid would be a third, lower in quality than the RT lane, and would require a whole bake infrastructure. M56 ships *specular* reflection probes only. |
| **Terrain collision** | Terrain (M58) is a render-only lane: `TerrainComponent` is `kComponentNoHash` and nothing it does reaches the simulation. A heightfield collider would move terrain into the hashed lane, requiring a fifth scene pair in `tools\replay_verify.bat` and an ABI bump for height/normal queries. Deferred to M59. |

### 6.6 Decals (M56a, M56b)

`DecalComponent` projects a texture (or a flat tint) onto whatever the G-Buffer already
holds inside an oriented box. The box is the entity's world matrix applied to the unit cube
`[-0.5, 0.5]^3`, and the **projection direction is local +Z** — the same rule
`LightComponent` uses, so a decal is aimed exactly like a spotlight.

- **Where it runs**: right after the deferred geometry pass (terrain included) and *before*
  SSAO / the ray-traced lane / the light pass. The decal therefore participates in ambient
  occlusion, shadowing and every lighting term, exactly like painted-on albedo.
- **How the surface is found**: G-Buffer RT2 already stores the world position, so no depth
  un-projection is needed. The pixel's world position is pushed through the decal's inverse
  matrix; anything outside `|local| <= 0.5` is discarded, and `RT2.a < 0.5` (no geometry)
  is discarded first — the G-Buffer is zero-cleared, so a box containing the world origin
  would otherwise "catch the sky".
- **Angle fade**: `saturate((dot(N, -projDir) - cos(angleFadeDeg)) / (1 - cos(angleFadeDeg)))`.
  The default 90° collapses to a plain cosine fade, which is what stops a downward decal
  from smearing down the vertical faces of whatever it lands on.
- **Render targets**: the pass binds **RT0 (albedo)** always, plus **RT1 (normal)** and
  **RT3 (material)** on any frame where at least one decal asks for them (M56b). RT2
  (position, the SSAO / RT / SSR input) and RT4 (velocity, the TAA input) are never bound
  and the pixel shader has no `SV_Target2` / `SV_Target4` to write them with.
- **Surface writes (M56b)**: `normalStrength` and `roughnessStrength` are not shader
  branches — **they are the hardware blend factors**, carried as the alpha of each render
  target's own pixel-shader output under `IndependentBlendEnable`. Strength `0` therefore
  degenerates to `src*0 + dst*1` and leaves the G-Buffer *bit-identical*, which is what
  makes the feature free for decals that only paint albedo. RT1 is masked to RGB and RT3 to
  **green only**, so a decal can never turn a surface metallic or emissive by accident.
- **Tangent frame**: `PerturbNormal` (the derivative-based TBN in `common.hlsli`) cannot be
  used here — `ddx/ddy` of the projector box's own surface has nothing to do with the
  receiving surface. The frame comes from the **decal's own OBB basis**: `T` = local +X,
  `B` = local −Y (the UV generator flips `v`), `N` = −projection direction. The three axes
  are normalized on the CPU in `FillDecalTransform`, so the shader never normalizes a basis.
- **Reading RT1 while writing RT1**: the angle fade needs the receiving normal, so on frames
  that bind RT1 as a render target the pass first copies it to an SRV-only scratch texture
  and samples the copy (a resource may not be bound for read and write at once). Frames with
  no surface-writing decal skip the copy entirely.

**v1 limitations:**

| Limitation | Why |
|---|---|
| **Forward path is not supported** | Forward has no G-Buffer, so there is no "already-shaded surface" to overwrite. A forward decal would have to be a second pass over the receiving geometry with its own clip volume, which is a different feature. Decals silently do nothing on the forward path and in asset thumbnails (`AssetPreviewCache` is forward-only). |
| **Metallic and emissive are never touched** | The material target is write-masked to its green (roughness) channel. A "rust" decal that also changes metalness would need a second masked channel and a second blend factor, which the single alpha output per target cannot carry. |
| **Normals blend in encoded space** | The hardware lerps the `*0.5+0.5` encoding, and the light pass renormalizes. This is exact for the endpoints and close enough in between; it is not a slerp. |
| **No decals on transparent surfaces** | Transparent geometry never reaches the G-Buffer, so there is nothing for the projector to land on. |
| **Sampler is LINEAR/CLAMP** | The reservation for M56 adds no new sampler states, and CLAMP is what keeps a decal from bleeding its opposite edge. `uvScale`/`uvOffset` therefore select an atlas sub-rect rather than tiling. |
| **No frustum culling** | Off-screen decals still rasterize their 12 triangles. Decal counts are expected to be small; this becomes worth fixing only when it shows up in a profile. |

### 6.7 Hierarchical Z-Buffer (M56c)

`HzbPass` builds a **min-Z pyramid** from the finished scene depth: texel *(x, y)* of level
*n* holds the depth of the **closest** surface anywhere in the screen region it covers. A
ray marcher can then read one texel of a coarse level and, if the ray is in front of that
minimum, skip the whole region in a single step. It exists for the screen-space reflections
of M56d; nothing consumes it yet.

- The pyramid is a **private `ID3D11Texture2D` (`R32_FLOAT`, full mip chain)**, not a
  `RenderTexture`. `RenderTexture` hard-codes `MipLevels = 1` and is shared by the G-Buffer,
  the post-processing intermediates, the SceneView target and the ray-tracing passes;
  teaching it about mips would put every one of those on the same code path for the sake of
  one consumer. The HZB needs neither an RTV nor a DSV — a compute shader writes it through
  a UAV and reads it back through an SRV — so owning the texture outright is both cheaper
  and narrower.
- One compute shader (`hzb_reduce.cs.hlsl`) does every level. Level 0 reads the depth buffer
  with source and destination the same size, which degenerates the reduction footprint to a
  single texel — i.e. a copy — so no separate blit shader is needed.
- **Odd extents are covered by widening, not by rounding.** Folding 15 texels into 7 with a
  plain 2x2 box drops the last row and column, and the only symptom is a ray passing through
  a wall that the HZB claims is empty. The footprint of output *i* is
  `[floor(i*src/dst), ceil((i+1)*src/dst) - 1]`, which overlaps on odd levels; overlap is
  harmless under `min`. `HzbReduceSpan` is the CPU mirror of that formula and `HzbSelfTest`
  checks the no-gap property for every level of a set of awkward resolutions.
- Depth stays **non-linear device depth**. Linearization is the consumer's job
  (`LinearizeDepth` in `common.hlsli`). `min` means "closest" because this engine does not
  use reversed-Z — every `ClearDepthStencilView` clears to 1.0.
- Each level is built with its own single-mip SRV and UAV. Reading through the full-chain
  SRV while writing the next level would bind the same subresource for read and write.
- Build cost is one dispatch per level. Measured at 960x540 (10 levels): **0.078 ms on an
  RTX 3060, 3.3-5.5 ms under WARP** — under the software rasterizer this is a real cost that
  M56d has to weigh against the tracing it saves, and it is the reason the pyramid is only
  built when someone asks for it.

**v1 limitations:**

| Limitation | Why |
|---|---|
| **Deferred path only** | It is built from the deferred depth target, and no forward-path consumer exists. On the forward path and in asset thumbnails (`AssetPreviewCache`, whose `depthSRV` is deliberately null) the pass disables itself. |
| **Built only on demand** | Until SSR lands, the only thing that asks for the pyramid is the debug view (`View > Rendering > HZB` or `--hzb-debug N`, where `N` shows level `N-1`). With it off, nothing is allocated and no dispatch is issued, so the rendered image is bit-identical to the previous milestone. |
| **Whole-screen only** | There is no partial or incremental update; the entire pyramid is rebuilt every frame it is requested. |

### 6.8 Screen-Space Reflections (M56d)

`SsrPass` marches the HZB of §6.7 through screen space and feeds what is already on screen
back into the image as reflections. It runs on the deferred path only, after the light pass
and the skybox, and is **off by default** (`CameraPostFxComponent::ssrOn`, the global
`View > Rendering > SSR` toggle, or `--ssr`). Turning it on is also what makes the HZB get
built at all — the debug view and SSR are the two things that can ask for the pyramid.

- **What is added is a difference, not a reflection.** The light pass has already added a
  specular environment term (`iblPrefiltered * ao * (F0*brdf.x + brdf.y)`). Adding a raw
  reflection on top would count the same light twice, so the pass adds
  `(reflection - iblSpecular) * envBRDF * weight`, which lands on exactly the value
  `ApplyLightingHybrid` would have produced had it lerped the two — the same "swap one
  radiance for another of the same dimension" rule the ray-traced reflection uses. Weight 0
  therefore adds a hard zero, which is why a scene with SSR off is bit-identical.
- **Roughness fade**: `1 - smoothstep(maxRoughness * 2/3, maxRoughness, roughness)`. At the
  default cutoff of 0.6 this is *numerically identical* to `RtReflWeight(roughness)` with
  `kRtReflFadeStart = 0.4`, so a surface never shows a seam between the SSR and the
  ray-traced reflection lane. `SsrSelfTest` pins that equality.
- **Fog**: the light pass fogs the pixel *after* adding the environment term, so the
  difference added afterwards is scaled by the same transmittance `1 - f`. The factor is
  obtained by calling the shared `ApplyFog` with black over white rather than by
  reimplementing the fog curve.
- **Positions come from depth, never from G-Buffer RT2.** RT2 is `R16G16B16A16_FLOAT`; away
  from the origin its steps are coarse enough in world units to make a mirror image
  visibly jitter. The pass un-projects HZB level 0 (which *is* the depth buffer) with the
  inverse of **the jittered `view * proj`** — the very matrix that rasterized the depth. Using
  the un-jittered projection here would displace every reconstructed position by half a pixel.
- **The trace compares against the cell exit, not the cell entry.** For each level the pass
  computes `tCell` (where the ray leaves the current HZB cell) and `tPlane` (where the ray
  reaches that cell's minimum depth). `tPlane > tCell` means the whole segment inside the
  cell is in front of everything there, so the ray jumps to `tCell` and goes one level
  coarser; otherwise it advances to `tPlane` and goes one level finer, and a crossing found
  at level 0 is a hit. Testing only the entry point would let a ray dive through a surface
  inside a coarse cell.
- **Every step advances.** `SsrCellAdvance` clamps its result to at least half a pixel and
  overshoots the cell boundary slightly, and it drops axis-parallel components instead of
  dividing by zero. Without that floor a ray sitting exactly on a cell edge burns its whole
  step budget in one place, and the only symptom is "the reflection is missing".
- **Reading the target it writes to**: the trace samples the light pass' own render target,
  so the pass copies it to an SRV-only scratch texture first (same trick as the RT1 copy in
  §6.6) and then blends the difference additively back into the original. One copy plus one
  full-screen pass is cheaper under WARP than tracing into a second target and compositing.
- Cost at 960x540 with 10 HZB levels: **0.11 ms trace + 0.08 ms HZB on an RTX 3060,
  11.0-12.0 ms trace + 3.2-3.5 ms HZB under WARP**. The screenshot suite pays this on one
  shot (`demo_render_ssr`); every other shot has SSR off and issues no instruction.

**v1 limitations:**

| Limitation | Why |
|---|---|
| **Deferred path only** | The pass needs the G-Buffer (albedo / normal / metallic-roughness) and the HZB, neither of which the forward path has. `--ssr` on the forward path is a no-op, proven by `demo_render_forward` staying bit-identical with the flag set. Asset thumbnails are forward-only too. |
| **Only what is on screen reflects** | A ray that leaves the viewport, or that never crosses a surface, contributes nothing and the pixel keeps its IBL specular. Hits fade out over the outer 12% of the screen so the reflection dissolves instead of being cut off. This is inherent to the technique; the local reflection probes of M56f are the intended fallback. |
| **Transparent surfaces are neither reflected nor reflective** | Transparency is drawn after SSR and writes no G-Buffer, so it cannot receive a reflection and cannot appear in one. |
| **One thickness for the whole scene** | Surfaces are treated as `kSsrThickness` (1 world unit) thick when deciding whether a ray that ended up behind the depth buffer actually hit. Geometry much thinner than that can be missed; geometry much thicker can catch a ray that should have passed behind it. |
| **No roughness-aware blur** | A single mirror ray is traced and the result is faded out by roughness rather than being blurred into a glossy lobe, so surfaces just under the cutoff reflect too sharply. A blurred variant needs its own history and denoiser, which is what the ray-traced reflection lane already provides. |
| **Reflections lag under TAA** | The velocity buffer describes the motion of surfaces, not of what they reflect, so a moving reflection is reprojected as if it were painted on the surface. |

### 6.9 Reflection probe capture (M56e)

`EnvMapBaker` can only bake *the sky*: its source is either a cubemap SRV or an analytic
gradient, and it has no notion of a position. A reflection probe needs "the view from this
point", so `ProbeBaker` (`src\Engine\Engine\ProbeBaker.{h,cpp}`) renders the scene into the
six faces of a 128² HDR cube and hands that cube to the *same* prefilter
(`EnvMapBaker::BakeFrom`). Not one line of the GGX prefilter or of the irradiance integral is
duplicated. Baking is always explicit: the `View > Rendering > Bake Reflection Probe Here`
menu item, or `--probe-bake X,Y,Z` on the command line.

- **Explicit only, never automatic.** A "bake whatever is visible" policy would make the
  result depend on what happened to be loaded and drawn that frame, which breaks the
  deterministic screenshot mode outright. No code path bakes on its own.
- **A dedicated `RenderSystem`.** `RenderSystem::Render` is not re-entrant (the render queue,
  the skin palettes, the per-view draw serials and the previous view-projections all live in
  the instance), so recursing into it six times would advance the temporal serial by six and
  throw away the TAA / RT history of the real view. `ProbeBaker` owns its own instance — the
  same solution `AssetPreviewCache` uses for thumbnails. Faces are drawn with `viewKey = 0`,
  the "no history" slot.
- **Forward path, post-processing off.** Capturing through the deferred path would resize the
  shared 5-target G-Buffer down to 128² and back on every bake, and at 128² neither SSAO nor
  SSR buys anything. With post-processing off the path writes linear radiance straight into
  the `R16G16B16A16_FLOAT` face, which is exactly what the prefilter wants — so the clear
  colour is converted to linear by the baker itself (the main path only does that when it has
  an HDR intermediate).
- **One table for the face basis.** `CubeFace()` in `EnvMapBaker.h` is read both by the bake
  shader (through `BakeCB`) and by `ProbeFaceView()`. Two tables would let the capture and the
  sampling disagree, which shows up as a reflection rotated 90° on some faces and is
  essentially impossible to diagnose from the picture. `ProbeBakerSelfTest` pins the invariant
  by un-projecting through the face camera and comparing against the direction the prefilter
  samples (worst error 1.1e-5 over 6 faces x 81 samples).
- **Seam check.** `--probe-bake` reads the six faces back, writes them to
  `tests\actual\probe_faces.png` as a cross (`+Y` / `-X +Z +X -Z` / `-Y`, so neighbouring faces
  are neighbours in the image too) and reports
  `seam ratio = mean texel step across a seam / mean texel step inside the faces`.
  Normalising against the faces' own texture rather than against scene brightness is what
  makes the number mean anything in a dark scene. Measured on `--render-demo` under WARP:
  **2.66 aligned, 6.06 with one face deliberately rotated 90°**; the CLI fails (exit 5) above
  4.0.
- Cost: **~0.4-0.6 s per bake under WARP** (Debug; 6 faces at 128² plus the 5-mip GGX
  prefilter and the 32² irradiance cube). Nothing pays it unless a bake is requested.

**v1 limitations:**

| Limitation | Why |
|---|---|
| **Ad-hoc captures feed nothing** | `Bake Reflection Probe Here` / `--probe-bake X,Y,Z` capture at an arbitrary point for diagnosis only: the result goes to the `Reflection Probe` preview window and to PNG. What the lighting consumes are the probes placed in the scene — §6.10. |
| **No persistence** | A bake lives in memory for the session; nothing is written to disk. |
| **Probes do not see each other** | A face is captured against whatever environment the scene already has (the sky IBL), so probe-to-probe inter-reflection does not exist. |
| **The seam check is scene-dependent** | Aligned and broken are only 2.3x apart on real content, so the threshold is a coarse tripwire for a gross orientation bug, not a quality metric. The PNG is the authoritative check. |

### 6.10 Local reflection probes (M56f)

`ReflectionProbeComponent` places one of the cubes of §6.9 in the world, and the deferred light
pass blends it into the **specular environment term**, producing the fallback chain
**SSR -> local probe -> global environment**. The component is `kComponentNoHash`, and — like
the capture itself — nothing happens until a bake is explicitly requested
(`View > Rendering > Bake All Reflection Probes`, or `--probe-bake-all`). A scene full of
probes that have never been baked renders bit-identically to one with no probes at all.

- **What is added is a difference, not a reflection** — the same construction as SSR (§6.8).
  The light pass has already added `iblPrefiltered * ao * envBRDF`, so the probe lane adds
  `(probe - ibl) * ao * envBRDF * weight * (1 - rtReflWeight)`, which lands exactly on
  "swap one radiance for another of the same dimension". Weight 0 adds a hard zero, and
  `ApplyLighting` / `ApplyLightingHybrid` keep their signatures — so the three forward shaders
  are untouched by construction.
- **SSR subtracts what the light pass actually added.** `ssr_trace.hlsl` receives the same
  probe array and uses `lerp(iblSpec, probeSpec, weight)` as the value it subtracts. Leaving it
  as the raw IBL would count the probe twice on every pixel where SSR also hit. The visible
  proof is the difference image between "SSR" and "SSR + probe": the reflected pillars — the
  pixels where SSR found a hit — are *black* in that diff, and only the pixels SSR missed move.
- **Box projection.** The reflection vector is extended to the inside wall of the probe's box
  and re-based on the capture point, so the mirror image slides correctly as the viewer moves
  inside the box; `boxProjection = false` selects the infinitely-distant-cubemap behaviour
  instead. `ReflProbeDir` floors the magnitude of each reflection-vector component before
  dividing, because a single NaN from an axis-parallel ray would poison the `min` and blank
  the pixel.
- **One probe per pixel** — the one the pixel is deepest inside, measured in units of that
  probe's blend distance. Ties go to the lower index, and probes are collected in `EntityID`
  order, so the choice never depends on archetype ordering (rule 7).
- **One `TextureCubeArray`**, bound at `t14` in the light pass and `t8` in SSR. The light pass
  has exactly one free SRV slot for this, so the prefiltered cubes are copied slice by slice
  into a single array (`kSpecMips * 6` `CopySubresourceRegion` calls per probe) instead of
  being bound one at a time. No new sampler — `s0` (LINEAR/CLAMP) is reused.
- **`ProbeBaker::Bake` has to unbind the render targets before prefiltering.** The last capture
  face can still be bound as an RTV when the same texture is handed to the prefilter as an SRV,
  and D3D11 responds by silently forcing the *SRV* to NULL. The result is a black cube for that
  probe with nothing in the log. M56e never hit it because it baked once; the second bake in a
  row hits it every time.
- Cost: **~33 ms per probe under WARP** once the IBL shaders are compiled; the first bake in a
  process also pays that compilation (measured 350-450 ms). Nothing pays anything unless a bake
  is requested.

**v1 limitations:**

| Limitation | Why |
|---|---|
| **Deferred path only** | The composition lives in the deferred light pass and in SSR; the forward path has no G-Buffer and nowhere to swap the environment term. Probes are simply absent there, and asset thumbnails (forward, own `RenderSystem`) never see them. |
| **Specular only** | A probe replaces the specular environment radiance, not the irradiance. Diffuse still comes from the sky IBL or the constant ambient, so a probe cannot supply a room's bounce light. |
| **Axis-aligned boxes** | Neither the influence volume nor the parallax box follows the entity's rotation or scale; only `extents` counts. A rotated box would push the parallax correction into box-local space and double the CPU/GPU mirror that has to be kept in step. |
| **No blending between probes** | The strongest probe wins outright, so two overlapping probes pop against each other where their weights cross. Blending needs a weighted sum over several cubes per pixel. |
| **At most `kMaxReflectionProbes` (8)** | The array lives in the light pass' constant buffer. Probes past the eighth are dropped with a warning rather than silently ignored. |
| **The baked state is frozen, and lives only in memory** | Position, box and intensity are captured *at bake time*; moving or editing a probe afterwards does not re-bake it (the picture and the box would then disagree). Nothing is written to disk, so a packaged build has no probes until something bakes them. |
| **Probes do not see each other** | Inherited from §6.9: each capture only sees the sky IBL, never another probe's reflection. |

---

## 7. Particle System Specification

### 7.1 Requirements

- Provide **both GPU and CPU implementations**, switchable at runtime through the editor GUI
- Both implementations must interpret the same emitter-definition data

### 7.2 Structure

```
ParticleEmitterComponent (data stored in ECS)
  └─ EmitterDesc (shared definition data)
       ├─ Emission: rate, bursts, shape (point/sphere/cone/box)
       ├─ Initial values: lifetime, velocity, size, color, rotation (all specified as ranges)
       ├─ Over-lifetime changes: size/color/velocity curves (keyframe arrays)
       ├─ Forces: gravity, constant wind, simple turbulence
       └─ Rendering: texture, blend mode, soft-particle option

IParticleBackend (switchable interface)
  ├─ CpuParticleBackend: SoA batch update using SIMD (SSE/AVX) → dynamic vertex buffer
  └─ GpuParticleBackend: update using Compute Shader → structured buffer + instanced rendering
```

### 7.3 Backend Specifications

| Item | CPU Implementation | GPU Implementation |
|---|---|---|
| Update | SoA layout + SIMD | Compute Shader (`Dispatch`) |
| Random numbers | Engine-provided deterministic RNG, as defined in Chapter 11 | Pre-generate a seed array and supply it through a buffer. Do not generate random numbers on the GPU, to preserve determinism |
| Alive-particle management | swap-and-pop | Free list or compaction [TBD] |
| Initial maximum count | 100,000 per emitter [TBD] | 1,000,000 per emitter [TBD] |
| Sorting for alpha blending | CPU sorting | Bitonic Sort [TBD: whether to include in the initial scope] |

### 7.4 Switching Behavior

- Select the backend using radio buttons in the editor’s Particle Settings window. The selection is saved as a project setting
- On switching, **discard all living particles and restart the emitter** in the initial implementation
  - [TBD] Whether to support preservation of living particles through GPU-to-CPU and CPU-to-GPU buffer transfer in a later milestone
- Provide a comparison mode that runs the same emitter on both backends in parallel, displays them side by side, and shows update time in milliseconds. This will serve as a portfolio showcase feature

### 7.5 Consistency

- Both backends should produce **logically equivalent results** from the same seed and input. Floating-point rounding differences are acceptable within a formally defined tolerance
- **Exception (M42e)**: depth-buffer collision (`ParticleEmitter.depthCollision`) is a **GPU-backend-only visual effect**; the CPU backend does not implement it and particles pass through geometry. This does not violate determinism: the GPU particle pool is not part of the world hash and is never read back to the CPU

---

## 8. Hot-Reloading Specification

As a shared foundation for all hot-reload targets, the Core layer provides **file monitoring through directory watching and change detection**. Detection occurs on a worker thread, while application occurs during Phase 2 of the main loop defined in Section 5.3.

### 8.1 Shaders

- Monitored path: `assets/shaders/*.hlsl`
- Detect an update → recompile in the background → replace the shader only if compilation succeeds
- **If compilation fails, retain the previous shader** and display the error in the Console window without stopping the engine
- When an included file changes, recompile every dependent shader using a maintained dependency graph

### 8.2 Assets (Textures / Models)

- Monitored paths: `assets/textures/`, `assets/models/`
- Detect an update → load the new resource → replace the object referenced by its AssetID handle
- References must access assets only through AssetID. Retaining raw pointers is prohibited so replacement remains transparent

### 8.3 Parameters / Scene Data

- Monitor scene files, prefabs, and project settings, all stored in a text format: [TBD: JSON / custom format]
- Detect edits made in an external editor and apply the differences to the running scene
- Inspector changes are applied immediately and are treated as normal editing rather than hot reloading

### 8.4 C++ Code (GameLogic.dll) — Core Engine Feature

**Procedure:**

```
1. Detect build completion by monitoring the DLL timestamp
2. Begin applying the update at a frame boundary during Phase 2
3. Invalidate the old DLL function table
4. Copy the new DLL under a unique name and load it as GameLogic_{counter}.dll
   Also copy the PDB under the same name to avoid debugger file locks and preserve breakpoints
5. Compare REGISTER_SCRIPT metadata and rebind the function table
6. Unload the old DLL
```

**State Preservation:**

- Script state is already stored in the engine-side ECS, as described in Section 5.2, so **backup and restoration are generally unnecessary**
- If the script’s field layout changes through additions, removals, or type changes, compare old and new reflection metadata. Preserve only fields whose names and types match, and initialize new fields to their default values

**DLL Boundary Rules:**

- Only C-style function tables declared with `extern "C"` may cross the DLL boundary. C++ class vtables, STL types, and exceptions must not cross the boundary
- Memory allocation and deallocation must always use the engine-side allocator and must not depend on the DLL-side CRT heap

**Explicit Limitations:**

- Only logic is reloadable. Values of unregistered global and static variables are not guaranteed to persist; project rules prohibit their use

---

## 9. Editor GUI Specification (ImGui)

| Window | Functionality |
|---|---|
| Hierarchy | Display the scene’s GameObject tree; select objects; modify parent-child relationships; create and delete objects |
| Inspector | Display and edit components on the selected GameObject; generated automatically through reflection |
| Scene View | Render the scene and provide translation, rotation, and scale gizmos [TBD: whether gizmos are included in the initial scope]. Camera authoring: the **selected** camera draws a frustum wire (aspect fixed at 16:9, far clipped for legibility by a toolbar slider) and a small preview of what it sees in the bottom-right corner; the Camera component has a **Pilot** button that redirects the scene view's fly controls (RMB look + WASDQE / wheel dolly / MMB pan) to that camera entity while the viewpoint stays put. Piloting rotates the pose as a quaternion delta — yaw about world up, pitch about the camera's own right axis — so an authored roll survives; decomposing to yaw/pitch would silently flatten it (`CameraPilotSelfTest`) |
| Game View | Display the game camera view |
| Console | Display logs, shader compilation errors, and hot-reload notifications |
| Profiler | Display frame time, phase timings, and particle update time for CPU and GPU implementations |
| Particle Settings | **Switch particle backends between GPU and CPU**, launch comparison mode, and configure maximum particle counts |
| Asset Browser | List files under `assets/` and display reload status |
| Build Settings | One-stop staged packaging (M51j): 1) script rebuild (C++ GameLogic + C# Roslyn, opt-out) → 2) asset cook warm-up → 3) package copy (Runtime.exe + GameLogic.dll + C# host + assets + boot scene + **sealed cooked cache**, §10.2) → 4) batch DDS texture cook (opt-in) → 5) zip (opt-in). Child processes (script build / `tar.exe`) are polled per frame so the UI stays live; each stage reports OK/NG in a list. The same pipeline runs from the CLI for CI: `Editor.exe --package <dir> [--package-dds] [--package-zip]` |

- Provide Play / Pause / Step controls. Editing policy during Play mode: [TBD: discard changes as Unity does / save changes]

### 9.1 Localization

The editor ships in **Japanese by default** and can be switched to English at runtime from
*View > Language*. See [ADR-010](docs/adr/ADR-010-editor-localization.md).

- Every UI string lives in `src/Engine/Core/LocalizationTable.inl`, one line per string. The
  `StrId` enum and both language tables are generated from that single file by the preprocessor,
  so a missing translation is a compile error rather than a runtime fallback
- Window and modal titles use the `"display###stable-id"` form. `ImHashStr` resets the hash at
  `###`, so the ImGui ID — and therefore `imgui.ini`, `DockBuilderDockWindow` and the saved
  layouts — stays identical across languages
- Inspector field labels come from `FieldDesc::displayName`, component labels from
  `EditorComponentCatalog`. The English `name` is a serialization key, a world-hash input and a
  DLL-reload migration key, and is never translated
- Only user-facing log messages are translated. Diagnostic logs and self-test output stay in
  English so they read alongside D3D debug-layer output and `HRESULT` values
- Rule 10 in §11.2 is enforced by `tools\check_rules.ps1`

---

## 10. Asset Pipeline

| Asset Type | Source Format | Runtime Format |
|---|---|---|
| Textures | png, tga, jpg, dds | Direct loading (stb / DDS); manual DDS cook in the Asset Browser (M39b). Batch DDS cook at packaging time (M51j, opt-in): images inside the package are cooked to `.dds` with their `.meta` import settings and the sources removed; `TextureLibrary::LoadFile` falls back to a same-stem `.dds` **only when the source image is missing**, under the same AssetID — development behavior is untouched |
| Models | fbx (ufbx), glTF/glb (cgltf) | Cooked binary cache `cache/cooked/*.mmdl` (M51b, §10.2); cold start parses and cooks |
| Audio | wav, ogg | wav decodes directly (near-memcpy); ogg decodes to a cooked PCM cache `cache/cooked/*.mpcm` (M51b) |
| Shaders | hlsl | Runtime compilation during development / precompiled for distribution |
| Scenes / Prefabs | Text format defined in Section 8.3 | Same as source |

- glTF is recommended because it avoids FBX SDK licensing and redistribution issues, while a custom parser can also demonstrate technical ability

### 10.1 Compose assets (`.actor.json`) — prefab 2.0

Implemented as an **extension of the M13 prefab**, not a parallel system; `.prefab.json` is read as
the subset with a single root, no parts and no override list. See
[ADR-011](docs/adr/ADR-011-compose-assets.md) for the reasoning and the v1 limits.

| Concept | Where it lives |
|---|---|
| Asset | `.actor.json` (`"actor": 1`) / `.prefab.json` (`"prefab": 1`), flat expanded subtree with local `fileId` |
| Identity | 64-bit path hash + `.meta` GUID (no UUIDs) |
| Placement | Expanded values **plus** a saved `"overrides"` key; non-overridden fields are refreshed from the base once at load |
| Structural overrides | `"+Component"` / `"-Component"` override keys (M50c). Scene document v3: a component missing without `-C` is re-added from the base at load. See [ADR-012](docs/adr/ADR-012-structural-overrides.md) |
| Part (socket) | `PartComponent { tag, joint, source }` on a child entity — no special syntax |
| Part lookup | `FindPart(root, "Hips/HandR")` / `FindPartsByTag(root, tag)` (ABI v9); attaching reuses `SetParent` |
| Part volumes | `PartBoundsComponent` (box/sphere) + `RaycastParts(root, tag, ray)` (ABI v10, M49); same function drives editor click-selection and the script lane |
| Bone following | `PartFollowSystem`, inserted after skinning and before physics/transform in the fixed tick |
| Schema components | `assets/schemas/*.component.schema.json` registered at startup, **after built-ins and before script types** |
| Generic field access | `GetComponentField` / `SetComponentField` by FNV-1a name hashes (ABI v11, M50d). Value copies only; the NoHash (C#) lane is blocked both ways for replay safety |
| Schema codegen | Generated from the runtime registry (no re-parse): `<project>/cache/Generated/SchemaComponents.gen.h` (layout mirror + typed accessors + `static_assert`s) and `assets/scripts/Generated/Schema.gen.cs` (constants + typed accessors). Name hashes are baked, TypeIds never are |
| Asset editing | Mini-scene edit mode: the asset is expanded into a private `Scene`; only `EditorApp::OnImGui`/`OnRenderViews` swap `ctx.scene`, so the tick path is untouched |

### 10.2 Cooked asset cache (`cache/cooked/`, M51b)

Startup used to re-parse every FBX/glTF and re-decode every ogg on each launch. The cooked cache
persists the parse results so warm starts skip the parsers entirely.

| Concept | Decision |
|---|---|
| Files | `<project>/cache/cooked/<guid 16hex>.mmdl` (models) / `.mpcm` (ogg PCM). Legacy launch and distributed builds use `<exeDir>/cache/cooked/` — the branch is on `projectRoot`, never on `assetsRoot` |
| Blob contents | The raw bytes handed to the resource libraries (vertices / indices / materials / skins / clips, and texture sources). Float bit patterns are preserved; replaying registers content bit-identical to a fresh parse (`CookedCacheSelfTest` enforces this, `replay_verify` records cold and verifies warm) |
| Insertion point | `RegisterAssets` (the startup scan via `RegisterAssetLibraries`, shared by Editor/Runtime) and the ogg branch of `LoadAudioFile`. `ModelLoader::Load` (drag & drop placement) always parses fresh. wav files are not cooked — their decode is near-memcpy |
| Invalidation | Header `{magic, kCookVersion, guid, srcSize, srcMtime, srcContentHash, srcPathKey, deps}`. size+mtime match → valid; mtime mismatch → re-hash the source, and if the content is unchanged the header mtime self-heals; content change → recook. A moved source recooks (sub-asset AssetIDs derive from the normalized path, so a fresh parse would register different keys). Recorded external texture paths (`deps`) are existence-checked; texture *content* stays live because replay re-reads the files |
| Textures | Embedded images are stored encoded and re-decoded on replay (so `.meta` import settings keep working); external files are re-loaded from the recorded resolved path |
| Escape hatch | `--no-cook-cache` (mirrors `--no-jobs` / `--no-sim-cache`): parse everything fresh, never read or write the cache |
| Sealed bundle (M51j) | A `.sealed` marker inside `cache/cooked/` (written only by Build Settings into the package) makes `ReadValidated` skip the pathKey / stat / content-hash / deps checks (magic / version / guid still apply). This is a **correctness** device, not an optimization: sub-asset AssetIDs derive from the packaging machine's absolute paths, so a relocated package that recooked would register different IDs and every scene reference to model meshes/materials would silently break. Replaying the sealed registrations reproduces the original IDs anywhere. External texture paths recorded in the blob are remapped onto the package's `assets/` root when missing (`ModelCook::Replay`) |

### 10.3 Physics material assets (`.physmat.json`, M59a1/M59a2)

Material properties for the rigid-body solver, stored as assets (`PhysMatLibrary`, SI units).
Schema: `density` / `staticFriction` / `dynamicFriction` / `restitution` / `rollingResistance` /
`dragCoefficient` (static friction, rolling resistance and Cd are parsed and stored but not
consumed until M59f2/M59b). Values are sanitized on load (non-finite → default, finite but out of
range → clamp) so NaN can never reach the pair-combination rules. Material values are **not**
world-hashed (same class as mesh collider data: replay assumes the same assets are present).

| Concept | Decision |
|---|---|
| Assignment | `Collider.physMaterial` (AssetRef). Empty = unassigned = the legacy component fields are read through the exact same code path — existing scenes stay bit-identical |
| Resolution priority | Per property: `Collider.materialOverrideBits` bit set → legacy component field / material assigned → material value / unassigned → legacy field. Resolution is a pure value selection at solver collection time (no fp arithmetic), shared by the solver and the script ABI (`SelectFriction` / `SelectRestitution` / `ResolveBodyMass` in `PhysicsSystem.h`) |
| Pair combination | Unchanged from M28b: `μ = sqrt(μa·μb)`, `e = min(ea, eb)` |
| Static restitution | **New capability in M59a2**: a static collider used to be structurally `e = 0` (no Rigidbody to store the field, so `e = min` killed every bounce off static geometry). A material assigned to a static collider now supplies its `e`. Unassigned static colliders keep `e = 0` |
| Mass from density | `Rigidbody.useDensity` (opt-in): mass = material `density` × world-scaled collider volume (sphere/box/capsule; scaling rules identical to `shapes::ApplyScaledExtents`). Falls back to the `mass` field when there is no material, no collider, a mesh collider (shape 3), or a degenerate volume. The ABI entry points (`AddForce` / `AddImpulse` / `AddTorque`) resolve mass through the same function, so mass is never two different values |

### 10.4 Physics environment, isotropic aerodynamics and buoyancy (M59b / M59b2)

Two opt-in components extend the rigid-body solver with a scene-wide environment and per-body air
forces. Both are world-hashed (they drive `velocity` / `angularVelocity`) and both are appended at
the end of the component registration order, so existing scenes are untouched.

| Concept | Decision |
|---|---|
| `PhysicsEnvironment` (TypeId 36) | `gravity` (vector) / `airDensity` / `windVelocity` / `waterPlaneY` / `waterDensity`. Consumed as the **active component with the lowest `entity.index`**, the same rule as Skybox and Fog |
| `Aero` (TypeId 37) | `enableDrag` / `enableAngularDrag` / `enableMagnus` plus `dragCoefficient` (≤ 0 = fall back to the physics material's Cd, else 0.47) / `areaScale` / `angularDragCoefficient` / `magnusCoefficient` |
| **Presence gate, not value gate** | A scene without `PhysicsEnvironment` runs the legacy `vy += kGravity * gravityScale * dt` statement unchanged; a body without `Aero` costs one component lookup and zero fp operations. Setting `gravity = (0, -9.81, 0)` is **not** promised to be bit-neutral — `-0.0f + 0.0f` is `+0.0f`, so an unconditional vector add can move the world hash of a body whose `vx` happens to be negative zero. **Attaching the component is opting in to the new formula.** Boolean fields inside `Aero` are a second, finer gate: an off flag skips the term rather than multiplying by zero (an all-off `Aero` is bit-neutral, and the self test asserts it) |
| Reference area | **Cauchy's mean projected area (convex surface area / 4)** — the only orientation-independent representative area with a physical basis. It reproduces `πr²` for a sphere and `1.5a²` for a cube of side `a`. `MeanProjectedAreaWorld` in `PhysicsSystem.h` is the single source; a missing or mesh collider falls back to the same "radius 0.5 sphere" default the inertia derivation uses. Orientation-aware integration (lift, stall, weathercock stability) is M59c/M59d |
| Drag | `F = ½ρ·Cd·A·|v_rel|·v_rel` solved in **closed-form implicit** shape `v ← v / (1 + (k|v|/m)·dt)`. Division only, so no coefficient can flip the sign or diverge. `v_rel` is measured against `windVelocity`, and the wind is added back after the update. Terminal speed converges to `sqrt(mg/k)`; the discrete fixed point `v(v + g·dt) = g/k` sits slightly below it (measured 6.5057 vs 6.5870 for a 1 kg, r = 0.5 sphere) |
| Angular drag | The same implicit form applied to `ω`, with the isotropic reading of the inverse inertia tensor (mean of the `invI` diagonal) as the scalar inertia. `freezeRotation` and kinematic bodies have a zero inverse tensor, so the term disables itself. Without this an object spun up by Magnus never slows down; `angularDamping` remains for compatibility but is a non-physical fixed rate and should be set to 0 on bodies that use `Aero` |
| Magnus | `F = S·(ω × v_rel)`, `S = magnusCoefficient · ½ρ·A·r`. This is the only explicit, direction-changing term, so it carries the same deterministic per-tick Δv clamp `SpringJoint` uses (100 m/s) as a divergence guard. Because the force stays perpendicular to velocity, explicit integration adds a small amount of energy per tick; in practice drag and angular drag absorb it |
| Wind | Uniform and steady in M59. Turbulence is reserved as "a dedicated PCG32 stream evaluated as a pure function of tick and cell coordinates" (rule 8) and is out of scope for M59 |
| `CharacterController` | **Does not follow the environment in M59.** It reads the `kGravity` constant directly and its ground test assumes the Y axis; an arbitrary gravity vector would break the controller's semantics rather than generalize it |
| `Buoyancy` (TypeId 38, M59b2) | Upward force `ρ_water · V_submerged · |g|` plus water drag, gated on the same opt-in rule. `volumeScale` ≤ 0 disables it; a body entirely above the surface is bit-identical to having no component at all. Without a `PhysicsEnvironment` the defaults (surface at y = 0, 1000 kg/m³) apply, matching how `Aero` treats air |
| Submerged volume | Spheres use the **spherical cap closed form** `V = π(R²t − t³/3 + 2R³/3)`, `M = π(R²t²/2 − t⁴/4 − R⁴/4)` with `t = planeY − centreY` — polynomials only, no trigonometry anywhere in the determinism-critical path. Boxes and capsules use the height ratio of the conservative AABB. `SubmergedFractionWorld` in `PhysicsSystem.h` is the single source |
| Water drag | Linear and prorated by the submerged fraction, in the same closed-form implicit shape as air drag. Note this makes a lightly-submerged floating body **very lightly damped** (measured: default drag 2 with 5 % submersion gives a damping ratio of ≈ 0.004) — that is the correct physics, not a bug |
| **No righting moment in v1** | The height-ratio approximation cannot see the horizontal shift of the centre of buoyancy, so a tilted raft keeps its tilt forever (only the angular water drag acts on it). The force is nonetheless applied *at the buoyancy centre* via `ApplyImpulse`, so the torque is provably zero today and starts working by itself once M59f1 adds a centre-of-mass offset. A true righting moment needs per-face pressure integration — that is M59c |
| Buoyancy direction | Always **+Y**, magnitude scaled by `|gravity|`. The water surface is an axis-aligned Y plane in M59, so tilting the gravity vector while keeping a horizontal surface has no coherent reading |

---

## 11. Debug/Release Consistency Policy

### 11.1 Purpose

Eliminate cases in which the engine works in Debug but fails in Release, or vice versa, through enforceable mechanisms. As a portfolio feature, the project will include not only rules but also an **automated verification system**.

### 11.2 Coding Rules and Prohibited Practices

| # | Rule |
|---|---|
| 1 | Logic branches based on `#ifdef _DEBUG` or `NDEBUG` are prohibited. Logging, visualization, and other behavior that does not affect state is permitted |
| 2 | Expressions with side effects inside `assert` are prohibited because the entire expression disappears in Release builds |
| 3 | Variables must always be initialized at declaration because uninitialized memory patterns differ between Debug and Release builds |
| 4 | Floating-point settings must use `/fp:precise` in all configurations. `/fp:fast` is prohibited |
| 5 | Container operations whose behavior changes because of `_ITERATOR_DEBUG_LEVEL`, such as using invalidated iterators, are prohibited. Compliance is enforced through static analysis and the boundary rules in Section 8.4 |
| 6 | Code that depends on unspecified evaluation order, such as multiple side effects in function arguments, is prohibited |
| 7 | Non-deterministic values such as pointer addresses and hash iteration order must not influence game logic. Sort order must use explicit deterministic keys |
| 8 | Only the engine-provided deterministic RNG with managed seeds may be used. Direct use of `rand()` or `std::random_device` is prohibited |
| 9 | Constants shared between C++ and HLSL must hold the same value on both sides. A mismatch corrupts constant buffers or traversal state and fails silently, so the values are compared by static analysis |
| 10 | UI strings live in `LocalizationTable.inl` and are read through `Tr()`. `Tr()` must never be the sole argument of a printf-style call, because a `%` inside a translation would then be read as a conversion specifier. Both languages of an entry must be non-empty, must agree on the identifier after `###`, and must use the same conversion specifiers in the same order — MSVC's `printf` does not support positional arguments, so word order cannot differ between languages |

### 11.3 Automated Verification: Replay Consistency Test

**Mechanism:**

1. Create a replay file that records input from the gamepad, keyboard, and mouse, together with time data such as delta time
2. Play the same replay in both Debug and Release builds
3. For every frame, calculate and record a hash of world state, including all entity Transforms, major components, and internal deterministic RNG state
4. Compare the hash sequences from both configurations. **The test fails if even one frame differs**, and reports the first divergent frame together with the differing state

- Introducing a fixed timestep, marked [TBD] in Section 5.3, is strongly recommended as a prerequisite
- CPU particles are included in the hash. GPU particles are excluded because they are rendering output; their behavior is verified separately through comparison mode without readback
- The test can run in CI through a command-line invocation such as `Editor.exe --replay-verify xxx.rep`
- `tools\replay_verify.bat` runs **four scene pairs**, each rebuilt from code before recording:
  the default demo (scripts, physics, particles, schema fields), the parts showcase
  (`--parts-demo`: skinned bones, part following, part raycasts), the game-flow showcase
  (`--flow-demo`, M51j: **LoadScene transitions across two scenes, TimeControl pause and
  50% time-scale windows, PersistStore carry-over, SaveGame writes and action-map evaluation**
  driven by a deterministic tick timeline in `FlowTitleDriver` / `FlowGameDriver`) and the
  local-multiplayer showcase (`--local-demo --local-players 2 --synth-input`, M52g: per-player
  input lanes). The flow pair is the aggregate proof that the M51 gameplay-flow features are
  replay-deterministic

**Field-level divergence diagnosis (M52a).** Knowing *which tick* broke is not the same as
knowing *what* broke. `HashWorld`, `HashWorldDetailed` and `HashWorldDump` are three exits of a
single walk — three separate implementations would eventually disagree and make the diagnosis
lie — and the third exit writes one tab-separated line per hashed unit
(`tick / entity / name / component / field / raw value in hex / folded hash`). The value column
covers exactly the byte range the hash reads (`FieldTypeSize`), so a `String64` difference past
the terminator stays visible instead of being hidden by string semantics.

- `--hash-dump PATH --hash-dump-tick T` writes the dump at the end of tick `T`, in record,
  verify or plain playback, from either `Editor.exe` or `Runtime.exe`
- A verify mismatch writes `<rep>.tick<N>.actual.dump` plus `<rep>.mismatch.txt` (the tick)
  without being asked
- `--hash-diff A B` compares two dumps and exits 1 if any leaf field differs. Roll-up rows
  (`#nameHash` / `#entity` / `#total`) are counted separately, so a one-field divergence is
  reported as exactly one row instead of drowning in the fold chain that follows it
- `tools\replay_verify.bat` calls this automatically **only when a pair fails**: it re-records
  the expected side in Debug at the mismatching tick and prints the field-level diff
- `tools\bisect_replay.bat` is a `git bisect run` wrapper (0 = good, 1 = bad, 125 = build
  failure) that verifies a golden replay recorded on a known-good commit

**Continuous integration (M52b).** `.github\workflows\ci.yml` is a single Windows job that calls
the *same* scripts a developer runs locally — no CI-only verification logic, so there is nothing
to keep in sync twice. It builds the managed host for both configurations, runs
`tools\replay_verify.bat` (eight project builds, four replay pairs, `check_rules.ps1`),
runs `--selftest` in both configurations, and finishes with a `--package` smoke test whose
success is reported through the process exit code. On failure the replay files and field dumps
are uploaded as artifacts, so a divergence found in CI can be diffed locally with `--hash-diff`.

CI-specific concerns are injected through three environment variables that the scripts append
verbatim, which is why the script bodies are identical locally and in CI:

| Variable | Value in CI | Purpose |
|---|---|---|
| `MYE_EXTRA_ARGS` | `--warp --no-audio` | appended to every `Editor.exe` invocation |
| `MYE_MSBUILD_ARGS` | `/p:MyeWarnAsError=true` | opt-in warnings-as-errors (off by default) |
| `MYE_DOTNET_ARGS` | `/p:TreatWarningsAsErrors=true` | the same for the C# host |

`GraphicsDevice::Init` tries `D3D_DRIVER_TYPE_HARDWARE` first and falls back to
`D3D_DRIVER_TYPE_WARP`, logging the adapter it actually adopted; `--warp` skips straight to WARP.
This is a configuration-value branch, not a `#ifdef _DEBUG` branch, so rule 1 still holds.
Because simulation is CPU-only, the world hash does not depend on the adopted driver: a replay
recorded on WARP verifies bit-identically on a discrete GPU and vice versa, which is what makes
a GPU-less runner an acceptable place to prove determinism. Golden screenshots, unlike hashes,
*are* driver-dependent and are therefore always captured with `--warp`.

**Screenshot regression (M52c).** Hashes prove that the *simulation* is reproducible; they say
nothing about what is drawn. `tools\shot_verify.bat` captures twelve deterministic screenshots with
`Runtime.exe` (no ImGui, so neither `imgui.ini` nor the cursor position can leak in) and compares
them against `tests\golden\*.png` pixel by pixel, writing a difference heat map next to any shot
that moved. `--update` re-records the golden set. Eight of the twelve gate CI; the other four
exist only to cover FXAA, TAA, SSR and froxel volumetrics -- all four compared at `--tol 0` and
all four skipped on the runner (`MYE_SHOT_SKIP_FXAA` / `_TAA` / `_SSR` / `_FROXEL`). Each is a
pass that **branches discretely**, so a 1-ULP difference flips the branch and throws a few dozen
pixels a long way: FXAA was measured at maxDiff 35 (M52c) and SSR at maxDiff 95 over just 30
pixels (M56d). No tolerance can cover that shape -- a genuine regression looks the same -- so the
runner does not shoot them at all and only bit-identity on the dev machine is claimed. A ninth,
`demo_terrain_deferred`, gates CI at `--tol 12` rather than 3: **anisotropic filtering is
implementation-defined** and the two WARP builds disagree by up to 8 levels on terrain viewed at
grazing angles (four splat layers x albedo+normal, amplified by the derivative-based TBN). Two of the eight
(`demo_render_forward` / `demo_render_deferred`, M54a) shoot the `--render-demo` showcase, which
is the only golden scene carrying spot and point lights -- without it every feature added by the
M54-M58 rendering roadmap would be pixel-invariant by default and land with zero coverage.
A further shot (`demo_terrain_deferred`, M58c) uses its own `--terrain-demo` scene rather than
extending `--render-demo`: terrain covers the whole frame, so folding it into the existing
showcase would have re-recorded goldens shared with other in-flight branches, and
`tests\golden\*.png` is binary and therefore unmergeable.

Determinism of a *frame* needs two guarantees that determinism of a *tick* does not:

- **Frame-to-tick coupling.** The tick loop consumes an accumulator fed by real elapsed time, so
  the number of ticks simulated by frame *N* normally depends on how fast the machine drew the
  previous frames. Passing `--screenshot` (without `--shot-every`) therefore switches the loop to
  a fixed frame delta equal to the tick length: the accumulator gains and loses exactly one tick
  per frame, making **frame index equal tick index**. `--shot-realtime` restores wall-clock pacing
- **Resource residency.** Textures decode on a worker thread and are published at a frame
  boundary (M23), so "did the decode finish in time" is another wall-clock dependency. The same
  capture mode drains the async queue before drawing

Three machine-dependent inputs are pinned rather than tolerated: the rasterizer (`--warp`, because
WARP and a discrete GPU differ by up to two levels per channel over most of the frame), the
font atlas (`--font-embedded`, because the atlas otherwise picks whichever Japanese TTF the
machine happens to have installed, and an English Windows Server runner has none), and the
antialiasing resolve (`--no-fxaa`).

The third one is not obvious and was measured, not guessed. Pinning the rasterizer to WARP is
*not* enough to make two machines agree: WARP ships in the OS, so a Windows 11 workstation
(`d3d10warp.dll` / `d3dcompiler_47.dll` 10.0.26100, shaders are compiled at runtime with no
on-disk cache) and a `windows-2022` runner (the same two DLLs at 10.0.20348) run different code.
Ablating the frame one stage at a time puts a number on each stage: raw raster and lighting differ
by **at most one level per channel**, tonemapping raises that to three in the deferred path, bloom
contributes nothing measurable — and FXAA amplifies the whole thing to **thirty-five**. That is the
expected behaviour of an edge filter that branches on neighbourhood luma: a one-ULP difference
flips a threshold and the blend weight changes with it, which is why the residual lands as speckle
along geometry edges rather than as a uniform shift. Dropping FXAA from the capture therefore buys
back a strict `--tol 3` comparison over the entire frame, instead of the `--tol 35` that keeping it
would demand — a bad trade, since a 35-level allowance would hide a real regression anywhere on
screen for the sake of one resolve pass. FXAA itself stays covered by `demo_forward_fxaa`, compared
at `--tol 0` on the developer's machine where bit-exactness does hold.

TAA (M55d) is held out of CI for the same reason, pre-emptively: its neighbourhood min/max clamp
is the same shape of computation as the FXAA luma threshold — a branch that a one-ULP difference
can flip — and unlike FXAA its result feeds the next frame's history, so any divergence
accumulates instead of staying local. The amplification factor has not been measured on a runner,
and putting the shot in CI before measuring it would only add a red image nobody can explain.
`demo_render_taa` is therefore local-only at `--tol 0` (`MYE_SHOT_SKIP_TAA`), captured with
`--no-fxaa` so that its whole difference from `demo_render_deferred` is TAA and nothing else.

Debug and Release produce bit-identical images, so the golden set is captured from Release only. The
comparison itself (`--img-diff A B [--tol N] [--fail-pixels N] [--diff-out PNG]`) distinguishes
*equal* (exit 0) from *different* (exit 1) from *not comparable* (exit 2) — folding a size
mismatch or an unreadable file into the success path would let a broken capture read as green.

**Snapshots and time travel (M52d / M52e).** Determinism is normally spent once, at verification
time. The same property also buys the ability to *re-enter* a past tick: `SimSnapshot` captures
the whole simulation lane — the World (archetype columns, entity records, free list, roots, RNG),
the Scene's `TimeControl` / `PersistStore` / file-id counter / source path / override table, the
CPU particle pools, the collision system's previous-pair sets, the script host's started set and
the loop's carried `prevTickInput` and audio-handle counter — into one blob, and restores it
bit-identically. Everything outside that boundary (the C# lane, GPU particles, trails, the
transform side table, audio) is *not* captured; it is either reset by the caller or recomputed.

- Snapshots may only be taken at a tick boundary where no structural change is pending, and the
  restore reads every small section into scratch before touching the world, so a corrupt blob
  fails without leaving a half-restored world behind
- `--snapshot-stress N` inserts a capture → restore → re-capture round trip every `N` ticks of a
  verify run and requires the byte-identical re-capture *and* the unchanged expected hash chain;
  `tools\replay_verify.bat` runs it on all four pairs
- The editor's **Timeline** window keeps a ring of snapshots (one per 30 simulated ticks, capped
  by count and bytes) plus the input of *every* tick, including paused ones — a paused tick still
  advances `prevTickInput`, so skipping it would desynchronise action edges. Seeking restores the
  nearest snapshot at or before the target and re-simulates forward through `RunOneTick`, the
  same function a live tick uses (a second implementation would drift from the first)
- A seek **verifies itself**: the hash after the re-simulation is compared against the hash
  recorded when those ticks first ran, and a mismatch is reported in the window rather than
  silently showing a past that never happened. C# script state is the expected cause, since it
  lies outside the snapshot boundary
- Scrubbing pauses the simulation *and* stops the tick loop entirely. Resuming branches from the
  scrub point and discards the recorded future, which is announced in the window because Unity
  has no equivalent behaviour
- Re-simulation suppresses the output lanes (audio playback, pad vibration, save writes) exactly
  as recording and verification do, but never the *input* lanes (`LoadScene`, `LoadGame`):
  suppressing a read would produce a different world and guarantee a hash mismatch
- `--timetravel-selftest [N]` is the machine-checkable form: it runs `N` ticks, seeks back by
  several distances, re-simulates forward, compares hashes, and then confirms on the live frame
  loop that scrubbing holds the tick index still and that resuming truncates the ring

**Crash bundles (M52f).** A shipped build that dies leaves nothing behind unless it was prepared
in advance, so both executables install four handlers at startup — the unhandled SEH filter,
`std::terminate`, the pure-virtual call handler and the CRT invalid-parameter handler — and write
`crash\<yyyyMMdd_HHmmss>\` containing `minidump.dmp`, `crash.txt` (exception code, faulting module
with its RVA and PE `TimeDateStamp`, build configuration, git hash, current tick, the original
command line and the tail of the log ring), `crash.rep` and, when the scene came from a file, a
copy of it as `scene.json`. `--no-crash-handler` disables the whole thing.

- **Nothing is allocated inside a handler.** The heap may already be corrupt and the crashing
  thread may hold a lock, so every output buffer is reserved at install time, all formatting goes
  through a fixed-capacity sink instead of `printf`, `dbghelp.dll` is resolved during install
  rather than under the loader lock, the faulting module is located with `VirtualQuery` instead of
  `GetModuleHandleEx`, and the log ring is read *without* taking its mutex — a garbled line is a
  far better outcome than a deadlocked report
- Nothing is placed on the stack either, because the worst case is a stack overflow, where only
  about a page remains. `--crash-test stackoverflow` proves the point: `crash.txt` and `crash.rep`
  are written normally, but `MiniDumpWriteDump` itself needs more stack than is left and faulted,
  leaving a zero-byte dump behind. The minidump is therefore written from a thread created for the
  purpose — a fresh stack — with the *crashing* thread's id in the exception information, a
  bounded wait, and deletion of the file if it fails. It is also the last file written, so the two
  that matter are already on disk before the heaviest step is attempted
- `crash.rep` is therefore not built when the crash happens: the engine keeps the finished `.rep`
  byte image alive at all times — one embedded snapshot plus every input since it — so the handler
  only has to `WriteFile` it. Appending a tick publishes itself with a single store to
  `tickCount`, a completed tick's hash is a single aligned 8-byte store, and the image is marked
  not-ready while a fresh snapshot is copied in, so no partially written file can escape
- The input of the tick that was *running* is recorded **before** the tick executes. Recording it
  afterwards would lose the very tick that crashed and make the replay stop one tick short of the
  fault. That tick has no hash yet, so **a `worldHash` of 0 is reserved to mean "no expected
  value"**: the verifier skips the comparison, counts the tick as unverified and says out loud
  that the crash did not reproduce this time. Writing a plausible-looking hash instead would turn
  "it did not reproduce" into a spurious mismatch. The reservation does not change the v4 layout
- Verifying `crash.rep` restores the embedded snapshot, so reproduction does not depend on the
  boot scene, and every tick up to the fault is checked hash-by-hash — that check, not the crash
  itself, is the evidence that the receiver's machine rebuilt the same world
- `--crash-test <av|purecall|terminate|invalidparam|stackoverflow> [--crash-at-tick N]` crashes on
  purpose; `tools\crash_verify.bat` runs all five and requires, for each, that the bundle is complete, that
  the `.rep` replays hash-identically, and that re-running it with the same trigger crashes again
  at the same tick. It is deliberately **not** part of CI, because a job that kills its own
  process cannot cleanly distinguish a staged failure from a real one. CI does, however, upload
  `bin\x64\*\crash\**` with its failure artifacts, so an *unplanned* crash on the runner arrives
  as a replayable bundle rather than as a bare exit code
- In Debug the CRT reports an invalid parameter through an assertion *before* calling the handler,
  and its default report target is a modal dialog — which would hang the process instead of
  producing a bundle. When no debugger is attached the report target is redirected so the handler
  is reached; in Release `crtdbg.h` compiles those calls away, so rule 1 is not involved
- `MYE_GIT_HASH` and `MYE_BUILD_CONFIG` come from a generated header written by an MSBuild target,
  not from a preprocessor definition on the compiler command line: the hash changes with every
  commit, and a changing command line would rebuild every translation unit

**Multiplayer input lanes (M52g).** The simulation consumes `kMaxPlayers = 4` `InputSnapshot`
lanes per tick instead of one. Lane 0 is exactly the historical single input — keyboard, mouse and
XInput slot 0 — and `EngineContext::Input()` is its alias, so every consumer that predates lanes
(editor hit-testing, the C++ and C# script `KeyDown` family) keeps reading it unchanged. Lane *n*
carries XInput slot *n* only; the keyboard is not split between players, because the key
assignment lives in one project-wide action map and per-player maps are out of scope here. The
`.rep` header field `playerCount`, reserved in v4, is what finally varies, so the format itself is
unchanged. `--local-players N` selects the lane count, and during verification **the `.rep` wins
over the flag**, since the tick record length is a property of the file.

`InputActions::Evaluate` now takes the lane array and evaluates the same map once per lane. Lanes
at or beyond `playerCount` are actively zeroed every tick rather than skipped: a skipped lane
would keep the last state a disconnected pad left behind, which reads as a button held forever.

Two pieces are what make this *verifiable* rather than merely present:

- **`PlayerInputComponent`** binds an entity to a lane and mirrors that lane's evaluated action map
  (four axis values, and held / pressed / released bit masks by definition index) into the ECS
  every tick, right after evaluation and before scripts run. Action state is otherwise a temporary
  that never reaches the world hash, so a mis-wired lane would break the recording and the
  verification *symmetrically* and the hashes would still agree. Mirroring it makes the wiring a
  hashed value. The mirror fields must therefore **not** be marked `kFieldNoSerialize`, since the
  hasher skips those. It also gives scripts a way in without new ABI: they read the mirror through
  the v11 `GetComponentField` slot, and the lane-aware slots are deferred to the v13 bundle
- **`--synth-input`** replaces the live capture with `SynthLaneInput(tick, lane)`, a pure function
  that gives each lane a different, block-quantised key and stick pattern. Headless runs otherwise
  have identically empty input on every lane, which no amount of lane plumbing can distinguish.
  It is injected at the same point verification substitutes recorded input, so the synthesised
  values are recorded normally and a `.rep` produced this way replays without the flag

`tools\replay_verify.bat` combines both in its fourth pair. The scene places all four players, so
running it with `--local-players 2` also pins "lanes beyond `playerCount` stay zero" in the same
hash chain. Forcing every entity to read lane 0 makes the pair fail at tick 0, and `--hash-diff`
names the offending `PlayerInput.axes` / `heldBits` / `pressedBits` fields directly.

### 11.4 Networked Determinism (2-player P2P, delay lockstep + predictive rollback)

M52h/M52i turn the replay determinism this chapter guarantees into a netcode. The claim being
cashed in is narrow and exact:

> **When a tick runs** may depend on wall-clock time, packet arrival and frame rate.
> **What a tick consumes** may depend on nothing but confirmed input.

Everything else follows. `NetSession` never writes a byte of simulation state; its only job is to
have every peer's lane for tick *T* in hand before *T* is consumed. `tools\net_verify.bat` proves
the separation held by recording a `.rep` on both peers and requiring that they are byte-identical
**and** identical to a single-process `--local-players 2 --synth-input` reference.

**Transport.** UDP, non-blocking, two peers only (mesh, relay and NAT traversal are out of scope).
There is no retransmission: every packet carries the last `kNetRedundancy = 8` ticks of the
sender's lane, which absorbs loss without any ordering or duplicate handling. A keep-alive every
50 ms is not decoration — while a peer stalls it submits no input, so a lost packet the other side
is waiting for would never be resent and both would deadlock. The handshake compares protocol
version, `MYE_API_VERSION`, `.rep` version, snapshot-blob version, lane count, input delay,
determinism-relevant launch options and **the starting world hash**, and refuses on the first
mismatch: the largest single cause of desync is stopped at the door rather than debugged later.

**Delay lockstep (M52h).** Each peer confirms its own lane for tick `T + inputDelay` immediately
before running tick `T` — once, structurally, per tick. Confirming from the frame head instead
would re-send a different value for the same target tick on frames that run no tick, and the peer
that already consumed the first value desyncs at once. During a session the C# lane is stopped and
`LoadGame` is refused (neither is snapshotted, so neither can be re-simulated); audio is **not**
suspended, because the reason record/verify suspends it is fast-forward playback, not determinism.

**Predictive rollback (M52i).** Waiting for a late lane is only acceptable while the wait is short.
Instead the missing lane is predicted — the newest confirmed value, repeated — the tick runs, and
the world is corrected once the truth arrives:

1. Every tick end captures a `SimSnapshot` of the state *before the next tick* and records what
   the tick consumed, its end-of-tick hash, and whether any lane was predicted
2. Each frame the arrived input is compared against what was actually fed. If they match, the
   speculative flag is simply cleared. If they differ at tick `B`, the snapshot taken before `B`
   is restored and ticks `B..now` are re-simulated through **the same `RunOneTick`** normal ticks
   and time-travel seeks use
3. Speculation is capped at `kNetMaxSpeculation = 8` ticks (133 ms); beyond that the session
   stalls, which is exactly the M52h behaviour. `--net-no-rollback` pins it there permanently

A tick that ran on a prediction is not final, so it is **not** written to the `.rep` and its hash is
**not** advertised to the peer. Recording moves out of `RunOneTick` and into the confirmation step,
where a tick is appended only once its inputs can no longer be overturned. That is why a rollback
run and a lockstep run produce the same file, and why `net_verify.bat` can compare them.

**Desync detection.** Every 8 confirmed ticks a peer advertises `(tick, world hash)`, piggybacked
on the input packets it already sends. The checkpoint interval is fixed rather than "whatever
arrived last" so that both peers converge on the *same* first disagreeing tick — comparing only the
newest advertisement makes the reported tick a function of arrival timing, and the two peers name
different ticks. On a mismatch each peer writes `crash\desync_<tick>_p<lane>\` containing the
`CrashRing` `.rep` (snapshot-embedded, replayable), a field-level hash dump, and the exact command
sequence to localise the divergence, then halts with exit code 4 (`--net-no-halt-on-desync` keeps
running for observation). Continuing silently is the worst option available: after a desync the two
worlds are simply different games that still look playable.

`--net-poke-tick N` corrupts one field of simulation state on purpose, inside the tick body so that
record, verify and networked play all reproduce it. `net_verify.bat` case E uses it end to end:
both peers halt at the same checkpoint, `--rep-diff` names the exact tick, and replaying the two
bundles at that tick and running `--hash-diff` names the single corrupted field.

**What is *not* deterministic here, and must not leak into the simulation.** The ABI v13 slots
`NetLocalPlayer` / `NetPlayerCount` / `NetIsConnected` / `NetPingMs` / `NetRollbackCount` all return
machine-dependent values. Scripts may read them for presentation — UI text, camera framing, debug
draw — but writing any of them into hashed state breaks the peers apart. The engine does not
prevent this; the checkpoint comparison catches it within eight ticks and produces a bundle, which
is the same bargain the rest of this chapter makes: rules plus mechanical verification, not
enforcement. `--net-demo` (`NetDuelDemo` + `NetHudDemo`) exists as the worked example of the split.

---

## 12. Milestones

| M | Scope | Completion Criteria |
|---|---|---|
| M0 | Foundation | Window creation, DirectX 11 initialization, main loop, logging, and ImGui rendering |
| M1 | ECS + object model | GameObject API, Transform hierarchy, and rendering from a triangle through to meshes |
| M2 | Editor foundation | Hierarchy / reflection-driven Inspector / Console |
| M3 | Hot Reloading I | Shader, asset, and scene-data reloading |
| M4 | Hot Reloading II | GameLogic.dll reloading, including state preservation |
| M5 | Particles | CPU implementation → GPU implementation → GUI switching + comparison mode |
| M6 | Consistency verification | Replay consistency tests running in CI and static enforcement of coding rules |
| M7 | Finalization | Demo scene, Profiler, documentation, and video recording |

The order is intentional: implementing the reload foundation in M3 first accelerates subsequent particle development through dogfooding.

---

## 13. Architecture Decision Records (ADR)

Record major design decisions so they can be clearly discussed during interviews.

| # | Decision | Rationale / Trade-off |
|---|---|---|
| ADR-1 | Adopt a hybrid ECS | Combines Unity-compatible usability with ECS performance. Resolves API usability issues encountered with the pure ECS used in the previous project. Trade-off: implementation cost of maintaining a dual structure |
| ADR-2 | Restrict hot reloading to GameLogic.dll | Significantly reduces complexity compared with making the entire engine a DLL. Trade-off: engine-side changes still require rebuilding |
| ADR-3 | Store script state on the engine side | Eliminates the need to back up state during DLL reloads. Trade-off: fields must be registered for reflection |
| ADR-4 | Guarantee consistency through rules and automated testing | Rules alone cannot prevent human error. Replay hash comparison provides mechanical verification |
| ADR-5 | Add further entries as decisions are made | |

The full records live in [`docs/adr/`](docs/adr/), one file per decision:
ADR-001 hybrid ECS / ADR-002 DLL-only hot reload / ADR-003 engine-side script state /
ADR-004 replay consistency / ADR-005 fixed tick, per-tick structural changes /
ADR-006 build system / ADR-007 dual render path / ADR-008 particle determinism /
ADR-009 hybrid path tracing (§6.4) / ADR-010 editor localization (§9.1) /
**ADR-011 compose assets (`.actor.json` = prefab 2.0)** (§10) /
ADR-012 structural prefab overrides / **ADR-013 predictive rollback netcode** (§11.4) /
**ADR-014 CI and pixel regression** (§11).

---

## Appendix A: List of Undecided Items

| Chapter | Item | Options |
|---|---|---|
| Cover | Engine name | — |
| 1.4 | Physics | Simple custom implementation / external library / out of scope |
| 2 | C++ standard | C++20 / C++17 |
| 2 | Build system | Native Visual Studio solution / CMake / Premake |
| 3.2 | Runtime.exe | Whether to include it in the initial scope |
| 5.3 | Fixed timestep | Introduce it (recommended) / variable timestep only |
| 6.1 | Rendering path | Forward / Deferred |
| 6.2 | Shadows, post-processing, and skinning | Priority of each feature |
| 7.3 | Maximum particle count, GPU alive-particle management, and sorting | — |
| 7.4 | Particle preservation when switching backends | Discard in the initial version / support later |
| 8.3 | Scene file format | JSON / custom format |
| 9 | Gizmos / Play-mode editing policy | — |
| 10 | Texture cache / model format | — |