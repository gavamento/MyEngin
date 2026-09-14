# 経緯 (コードのコメントから移したもの)

コードのコメントには「今の事実」と「今も踏みうる罠」だけを残し、変更の経緯はここに置く。
移したのは、後から理由を知りたくなるもの — 実測した事故、採らなかった案とその理由、版を上げた理由、再発しうるバグ — に限る。
「移した」「切り出した」「1 ビットも変えていない」のような作業の報告は、コミットメッセージにあるので移していない。

| ファイル | 範囲 |
|---|---|
| [physics-replay.md](physics-replay.md) | `src/Engine/Engine/` の Physics / Replay / Net / HotReload |
| [audio-acoustic-ui-script.md](audio-acoustic-ui-script.md) | `src/Engine/Engine/` の Audio / Acoustic / UI / Script |
| [engine-core.md](engine-core.md) | `src/Engine/Engine/` 直下 (selftest を除く) |
| [particles-assets-selftests.md](particles-assets-selftests.md) | `src/Engine/Engine/` の Particles / Asset / RayTracing / Vfx と直下の selftest |
| [renderer.md](renderer.md) | `src/Engine/Renderer/` |
| [shaders-core-platform.md](shaders-core-platform.md) | `assets/shaders/`、`src/Engine/Core/`、`src/Engine/Platform/` |
| [editor.md](editor.md) | `src/Editor/` (SourceControl を除く) |
| [api-scripting-tools.md](api-scripting-tools.md) | `src/Editor/SourceControl/`、`src/Shared/`、`src/Scripting/`、`src/Runtime/`、`src/GameLogic/Scripts/`、`tools/` |

各ファイルは元のソースファイルごとの節に分けてある。コードを読んでいて「なぜこうなっているのか」の背景が欲しくなったら、そのファイル名で検索する。
設計判断そのものの記録は ADR にあり、ここはその補足にあたる。
