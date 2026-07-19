# ADR-006: ネイティブ VS ソリューション + v143 + ベンダリング

## 決定
- ビルドは Visual Studio ネイティブの .sln / .vcxproj (CMake / Premake は使わない)
- PlatformToolset は v143 (VS2022 互換。VS2026 でもそのまま開ける)
- 共通コンパイル設定は build\Common.props に一元化 (/fp:precise, /Zi, /W4, C++20, RuntimeLibrary 統一)
- 外部ライブラリはソースを external\ にコミット (バージョンは external\VERSIONS.md に記録)
- ファイル一覧は tools\gen_project_files.ps1 で vcxproj/.filters に反映する

## 理由
- Windows + VS 固定という仕様 (spec 2 章) に対して最小の複雑さで済む
- クローン → .sln を開く → F5 で動く、という評価者体験を最優先
- 一貫性ポリシーに関わるコンパイルオプションを 1 ファイルで管理・レビューできる

## トレードオフ
- 他ビルドシステムへの可搬性はない (仕様上のスコープ外)
- GameLogic.vcxproj は Engine.lib をリンクしない構造を維持する必要がある (レビュー観点)
