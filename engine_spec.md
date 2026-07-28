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

[TBD] Rendering path:

- **Option A: Forward Rendering** — Lower implementation cost. Recommended if particles and hot reloading are the primary portfolio focus
- **Option B: Deferred Rendering** — Stronger demonstration of rendering technology, but substantially higher development cost. Recommended if the project is intended to emphasize graphics programming

### 6.2 Initial Feature Scope

| Feature | Priority | Notes |
|---|---|---|
| Static mesh rendering | Required | |
| Materials / shader variants | Required | Hot-reloadable |
| Textures (2D / mipmaps) | Required | |
| Directional light + ambient light | Required | |
| Particle rendering (additive / alpha blending) | Required | See Chapter 7 |
| Shadow mapping | [TBD] | |
| Post-processing such as bloom | [TBD] | |
| Skeletal animation | [TBD] | |

### 6.3 DirectX 11 Abstraction

- The device and context are encapsulated within the Renderer layer. Raw DirectX 11 types are not exposed to the Engine layer or higher layers
- Rendering follows a “collect render items → sort → submit” model. No immediate-mode rendering API is provided, preserving room for future multithreading and graphics API replacement

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

- Provide Play / Pause / Step controls. Editing policy during Play mode: [TBD: discard changes as Unity does / save changes]

---

## 10. Asset Pipeline

| Asset Type | Source Format | Runtime Format |
|---|---|---|
| Textures | png, tga | [TBD: direct loading / DDS conversion cache] |
| Models | [TBD: FBX using the SDK / glTF using a custom parser or library] | Custom binary cache |
| Shaders | hlsl | Runtime compilation during development / precompiled for distribution |
| Scenes / Prefabs | Text format defined in Section 8.3 | Same as source |

- Import assets asynchronously while the editor is running and generate intermediate-format caches under `cache/`
- glTF is recommended because it avoids FBX SDK licensing and redistribution issues, while a custom parser can also demonstrate technical ability

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

### 11.3 Automated Verification: Replay Consistency Test

**Mechanism:**

1. Create a replay file that records input from the gamepad, keyboard, and mouse, together with time data such as delta time
2. Play the same replay in both Debug and Release builds
3. For every frame, calculate and record a hash of world state, including all entity Transforms, major components, and internal deterministic RNG state
4. Compare the hash sequences from both configurations. **The test fails if even one frame differs**, and reports the first divergent frame together with the differing state

- Introducing a fixed timestep, marked [TBD] in Section 5.3, is strongly recommended as a prerequisite
- CPU particles are included in the hash. GPU particles are excluded because they are rendering output; their behavior is verified separately through comparison mode without readback
- The test can run in CI through a command-line invocation such as `Editor.exe --replay-verify xxx.rep`

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