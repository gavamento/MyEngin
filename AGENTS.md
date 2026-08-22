# Repository Guidelines

## Project Structure & Module Organization

MyEngine is a Visual Studio 2022 C++20 / DirectX 11 game engine. The solution file is `MyEngine.sln`, with project files under `build/`. Runtime code lives in `src/`: `Editor/` hosts the ImGui editor, `Runtime/` is the UI-free executable, `GameLogic/` is the hot-reloaded user DLL, `Shared/` defines the C ABI boundary, and `Engine/` contains Platform, Core, Renderer, and Engine layers. Assets and shaders are in `assets/`; design notes and ADRs are in `docs/`; helper scripts are in `tools/`.

## Build, Test, and Development Commands

- Open `MyEngine.sln` in Visual Studio 2022, select `Debug|x64`, then press F5 to build and run the editor.
- `tools\gen_project_files.ps1`: refreshes `.vcxproj` source lists after adding or moving files.
- `tools\check_rules.ps1`: runs static coding-rule checks for deterministic behavior, localization safety, and C++/HLSL constant agreement.
- `tools\replay_verify.bat`: builds Debug/Release and verifies replay determinism across covered scenes.
- `Editor.exe --selftest`: runs engine/editor self-tests from a built output directory.

## Coding Style & Naming Conventions

Follow the existing C++ style in `src/`: 4-space indentation, braces on their own line for functions, `PascalCase` types and functions, `camelCase` locals, and `kCamelCase` constants. Keep dependencies layered: higher layers may depend only on lower layers, and raw D3D types must not escape Renderer. The `src/Shared/` DLL boundary must stay C ABI + POD only; do not pass STL types, vtables, or exceptions across it.

## Testing Guidelines

Place focused regression tests beside the feature using the existing `*SelfTest.cpp` / `*SelfTest.h` pattern. Add tests when touching ECS, serialization, replay, assets, localization, renderer constants, or hot reload behavior. Run `Editor.exe --selftest`, `tools\check_rules.ps1`, and `tools\replay_verify.bat` before submitting broad engine changes.

## Commit & Pull Request Guidelines

Recent commits use milestone-prefixed Japanese subjects such as `M50c: ...` and mention ABI changes explicitly when relevant. Keep commits scoped, describe behavior changes, and include updated ADRs or docs for architectural decisions. Pull requests should summarize intent, list verification commands, link issues or ADRs, and include screenshots or captures for visible editor/runtime changes.

## Agent-Specific Instructions

Do not delete files without first listing the absolute path, purpose, and impact, then receiving explicit approval. When reporting repository access, include the accessed link target and absolute file path.
