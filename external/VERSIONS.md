# Vendored Third-Party Libraries

| Library | Version | Source | License |
|---|---|---|---|
| Dear ImGui | v1.92.8-docking | https://github.com/ocornut/imgui/releases/tag/v1.92.8-docking | MIT |
| ImGuizmo | v1.92.5 WIP (master, src/ImGuizmo.{h,cpp} のみ) | https://github.com/CedricGuillemet/ImGuizmo | MIT |
| stb_image.h | master @ 31c1ad37456438565541f4919958214b6e762fb4 | https://github.com/nothings/stb | MIT / Public Domain |
| stb_dxt.h | v1.12 | https://github.com/nothings/stb | MIT / Public Domain |
| stb_vorbis.c | v1.22 (master @ 1ee679ca2ef753a528db5ba6801e1067b40481b8) | https://github.com/nothings/stb | MIT / Public Domain |
| cgltf | v1.15 | https://github.com/jkuhlmann/cgltf/releases/tag/v1.15 | MIT |
| ufbx | v0.23.0 (master, ufbx.{h,c}) | https://github.com/ufbx/ufbx | MIT / Public Domain |
| nlohmann/json | v3.12.0 (single include json.hpp) | https://github.com/nlohmann/json/releases/tag/v3.12.0 | MIT |
| DirectXMath | Windows SDK 同梱 | `<DirectXMath.h>` | MIT |
| IconFontCppHeaders | main (IconsFontAwesome6.h) | https://github.com/juliettef/IconFontCppHeaders | Zlib |
| Font Awesome 6 Free Solid | 6.x (fa-solid-900.ttf → fa_solid_900.h に C 配列で埋め込み) | https://github.com/FortAwesome/Font-Awesome | SIL OFL 1.1 (LICENSE.txt 同梱) |
| libtess2 | master @ 8dbd6483e920311a58c9af10a10beb278efebc36 (2025-10-15、タグ v1.0.2 は2011年時点のものでバグ修正が反映されていないため不採用) | https://github.com/memononen/libtess2 | SGI Free Software License B 2.0 (LICENSE.txt 同梱、MIT相当) |
| Amazon GameLift Servers Server SDK for C++ | v5.6.0 (`7c2a5a7cae6616b1ca2f217aaceff36b06393c80`)。公開ヘッダ + `/MT`・`/MTd` のビルド済み静的 .lib (Debug 72MB / Release 53MB)、パッチ 1 本 (`/Zi` 除去)。Server.exe だけが使う (M81g)。手順は `gamelift-server-sdk\BUILD.md` | https://github.com/amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk | Apache-2.0 (LICENSE / NOTICE 同梱) |
| OpenSSL | 3.6.5 (vcpkg `x64-windows` の Release。ヘッダ + import lib + `libssl-3-x64.dll` / `libcrypto-3-x64.dll`)。GameLift SDK の依存で DLL 必須 (M81g)。手順は `openssl\BUILD.md` | https://github.com/openssl/openssl/releases/tag/openssl-3.6.5 | Apache-2.0 (LICENSE.txt 同梱) |
| Recast Navigation | v1.6.0 (`6dc1667f580357e8a2154c28b7867bea7e8ad3a7`)。Recast / Detour / DetourTileCache / DetourCrowd / DebugUtils の Include と Source だけ (RecastDemo / Tests / CMake は入れない)。DebugUtils の RecastDump.cpp はビルド対象外。パッチは `recastnavigation\PATCHES.md` (M82a) | https://github.com/recastnavigation/recastnavigation/releases/tag/v1.6.0 | Zlib (License.txt 同梱) |
| meshoptimizer | v1.3 (`9e1f07b159d3cb777f1c67ed31fc11fd117986f4`)。`src\` のうちメッシュ LOD 生成に要る meshoptimizer.h / allocator.cpp / indexgenerator.cpp / simplifier.cpp と LICENSE.md だけ (M90e)。改変なし | https://github.com/zeux/meshoptimizer/releases/tag/v1.3 | MIT (LICENSE.md 同梱) |

方針: パッケージマネージャ・サブモジュールは使わず、ソースをそのままコミットする（クローン → F5 で動くことを優先）。
例外 (M81g): GameLift Server SDK と OpenSSL は、ソースからのビルドが重い (SDK は CMake + 依存取得、OpenSSL は Perl + NASM で約 13 分) ため、
ビルド済みの .lib / .dll をコミットする (nethost と同じ流儀)。再ビルドの手順は各フォルダの BUILD.md。
