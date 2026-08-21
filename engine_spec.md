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
- **Deferred** — 4 render targets (albedo / world normal / world position / metal-roughness-emissive).
  SSAO, screen-space effects and the ray-traced lanes of §6.4 hook in here

### 6.2 Feature Scope

| Feature | Status | Notes |
|---|---|---|
| Static mesh rendering | Implemented | Frustum culling + instancing |
| Materials / shader variants | Implemented | `.mat.json`, hot-reloadable |
| Textures (2D / mipmaps) | Implemented | GUID-keyed, async decode, BCn cooking |
| Lighting | Implemented | Directional / Point / Spot (max 16) + ambient, Cook-Torrance PBR |
| Particle rendering | Implemented | See Chapter 7 |
| Shadow mapping | Implemented | 3-cascade CSM with PCF |
| Post-processing | Implemented | HDR, bloom, tonemap, FXAA, DoF, motion blur, auto-exposure, LUT |
| Skeletal animation | Implemented | 128-bone palette, glTF / FBX skinning |
| Image-based lighting | Implemented | Irradiance + prefiltered specular + BRDF LUT |
| Ray-traced secondary rays | Implemented | See §6.4 (default off) |

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
- **Not covered in v1**: skinned meshes and transparents are absent from the BVH, secondary
  hits shade from material constants only (no bindless textures), moving objects ghost
  (no object motion vectors), and local lights cast no ray-traced shadows

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
| Scene View | Render the scene and provide translation, rotation, and scale gizmos [TBD: whether gizmos are included in the initial scope] |
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
nothing about what is drawn. `tools\shot_verify.bat` captures five deterministic screenshots with
`Runtime.exe` (no ImGui, so neither `imgui.ini` nor the cursor position can leak in) and compares
them against `tests\golden\*.png` pixel by pixel, writing a difference heat map next to any shot
that moved. `--update` re-records the golden set.

Determinism of a *frame* needs two guarantees that determinism of a *tick* does not:

- **Frame-to-tick coupling.** The tick loop consumes an accumulator fed by real elapsed time, so
  the number of ticks simulated by frame *N* normally depends on how fast the machine drew the
  previous frames. Passing `--screenshot` (without `--shot-every`) therefore switches the loop to
  a fixed frame delta equal to the tick length: the accumulator gains and loses exactly one tick
  per frame, making **frame index equal tick index**. `--shot-realtime` restores wall-clock pacing
- **Resource residency.** Textures decode on a worker thread and are published at a frame
  boundary (M23), so "did the decode finish in time" is another wall-clock dependency. The same
  capture mode drains the async queue before drawing

Two machine-dependent inputs are pinned rather than tolerated: the rasterizer (`--warp`, because
WARP and a discrete GPU differ by up to two levels per channel over most of the frame) and the
font atlas (`--font-embedded`, because the atlas otherwise picks whichever Japanese TTF the
machine happens to have installed, and an English Windows Server runner has none). Debug and
Release produce bit-identical images, so the golden set is captured from Release only. The
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
**ADR-011 compose assets (`.actor.json` = prefab 2.0)** (§10).

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