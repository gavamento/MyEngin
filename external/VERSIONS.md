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

方針: パッケージマネージャ・サブモジュールは使わず、ソースをそのままコミットする（クローン → F5 で動くことを優先）。
