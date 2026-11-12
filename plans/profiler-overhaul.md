# プロファイラー刷新 計画書

## Context（なぜやるか）

今のプロファイラー（`src/Editor/Windows/Debug/ProfilerWindow.cpp`、229 行の `ImGui::Text` の羅列）は「何が負荷をかけているか」が読めない。実機撮影とコード調査で確認した原因は次のとおり。

- **文字が被る**: 既定ドックが右下（約 341×244 px）。警告トースト（`ToastCenter.cpp:97-109`、作業領域の右下アンカー、背景アルファ 0.92）が同じ場所に積まれ、文字が透けて重なる。
- **列が揃わない**: 空白と `%-14s` の桁合わせに頼っているが、フォントは Segoe UI / 游ゴシック（等幅なし）。
- **順位が読めない**: 瞬間値のみ。平均・最大・割合なし。CPU スコープは Pop 順で親子が逆、複数 tick で同名行が重複。
- **時点が不揃い**: フェーズ時間は前フレーム、スコープは今フレームの途中、GPU は数フレーム遅れ。
- **計測の穴**: `presentMs` は実際には ImGui の時間で、`Present()`（`EngineLoop.cpp:3519`）はどこにも計上されていない。スクリプト層・描画フェーズにスコープが無い。メモリは累計のみ。GPU メモリ・アセット・ジョブの計測は存在しない。

ゴール: **タブ + プルダウン + 表**の構成に作り直し、選ばれた機能（1 自動記録 / 4 エンティティ別 / 5 スクリプト別 / 7 積み上げグラフ / 8 予算 / 11 メモリ内訳 / 12 GPU メモリ / 13 アセット読み込み / 14 ジョブ）を載せる。

## 守る制約

- 計測値は sim 状態・ワールドハッシュに入れない（決定性に影響させない）。`_DEBUG` 分岐で挙動を変えない。
- 記録領域は固定長で事前確保（tick 中の追加確保なし。現行 `Profiler.h:5-6` の方針を維持）。
- **ABI は変えない**（スクリプト別計測はホスト側だけで可能と確認済み。v36 / 203 スロットのまま）。
- 生の D3D 型を Renderer 層の外へ出さない（GPU メモリは POD の一覧で公開）。
- `RenderStats` の欄と `--render-stats-dump` の `counts` スキーマは変えない（`tools/cull_counts.ps1` と `RenderStatsSelfTest.cpp` の Fingerprint に波及させない）。
- UI 文字列は `LocalizationTable.inl` + `Tr()`。色は `themeColor::`（ImVec4 直書き禁止、Accent を状態色に使わない）。

## 画面構成

```
┌───────────────────────────────────────────────────────────────┐
│ 16.7 ms (60 fps) ▮▮▮▮▮▯▯  CPU 3.4 / GPU 0.4 / 待ち 12.9   [⏸] │ ← 常時表示の帯
│ ビュー:[全体▼] 平均:[60f▼] 単位:[ms▼]  [0の行を隠す] [検索…]  │ ← プルダウン
├───────────────────────────────────────────────────────────────┤
│ 概要│CPU│GPU│描画│メモリ│アセット│ジョブ│システム│履歴        │
└───────────────────────────────────────────────────────────────┘
```

| タブ | 中身 |
|---|---|
| 概要 | 積み上げフレームグラフ（案7）、重い順ランキング上位 10（CPU スコープ + GPU パス + スクリプト混在）、予算超過の一覧（案8） |
| CPU | スコープのツリー表（名前 / 今回 / 平均 / 最大 / 割合バー / 回数）。下段にスクリプト別（案5）・エンティティ別（案4）の表。行クリックでヒエラルキー選択 |
| GPU | パス別の表。そのフレームで走っていないパスは灰色。GPU 合計と未計測分 |
| 描画 | ドローコール / 三角形 / カリング理由別 / LOD / オクルージョン / 影（ビューはプルダウンで切替） |
| メモリ | タグ別の確保回数・バイト数（フレームあたり）、生存数、GPU メモリ（種類別 + 大きい順一覧、案11・12） |
| アセット | 読み込み履歴（種類 / パス / ms）、非同期キューの長さ、ホットリロード履歴（案13） |
| ジョブ | ワーカー別稼働率バー、バッチ数、呼び出し側の待ち時間（案14） |
| システム | パーティクル / 音響 / レイトレ BVH / ナビ / 知覚など既存の機能別統計、レンダーパス、エンティティ数 |
| 履歴 | 自動記録したフレームの一覧（案1）。選ぶと全タブがそのフレームの内容に切り替わる |

- 表はすべて `ImGui::BeginTable`（手本: `PerformanceCompareWindow.cpp:341-361`、スクロール固定は `TimelineWindow.cpp:701-704`）。ソートは `ImGuiTableFlags_Sortable` を新規導入。
- タブは `BeginTabBar`（手本: `SourceControlWindow.cpp:275-284`）。
- 色分け: 予算の 80% 以上 = `themeColor::Warning`、超過 = `themeColor::Error`。
- 選択タブ・プルダウン・予算・記録しきい値は `EditorSettings`（`.mye/editor_settings.json`）に保存（手本: `FeelMonitorWindow.cpp:98-108`）。
- 既存の長い 1 行（froxel / acoustic / rt / occlusion の注記）は表の「詳細」列とツールチップへ分解する。

## 設計の要点

### A. フレームの「確定」と履歴（すべての土台）
`src/Engine/Core/Diagnostics/Profiler.h/.cpp`
- スコープ記録を「Push 時に枠を確保、Pop 時に ms を書く」へ変更 → 前順（親→子）になる。`ScopeRecord` に親 index を追加。
- `prof::BeginFrame()` の先頭で**前フレームを確定**し、固定長リング（240 フレーム）へ格納する。同名・同親のスコープは合算し回数を持つ（`physics ×3`）。
- 確定フレーム `prof::Frame` が持つもの: フェーズ時間、スコープ（最大 256）、GPU 項目（最大 64）、項目別サンプル（スクリプト / エンティティ、最大 256）、メモリ差分、ジョブ統計、frameIndex と tick 番号。
- UI は**確定済みフレームだけ**を読む → CPU / フェーズ / 統計の時点が揃う（GPU は GpuTimer の遅延ぶん遅れる旨を表示）。
- `FrameScopes()` は互換のため残す（`FractureBenchmark.cpp:137` が名前で参照）。

### B. フレーム時間の内訳を正す（案7）
`src/Engine/Engine/Loop/EngineLoop.cpp` / `.h`
- `FrameTimings` に `imguiMs`（現 `presentMs` の実体）と `presentWaitMs`（`:3519` の `Present()` を計時）を追加。`その他 = frameMs − 合計` を表示。
- 積み上げグラフは壁時計の分割（ホットリロード / tick / 描画 CPU / ImGui / Present 待ち / その他）を `ImDrawList::AddRectFilled` で描く（手本: `TimelineWindow.cpp:301-452`）。GPU 合計は折れ線で重ねる（CPU と時間が重なるため積まない）。16.67 / 33.3 ms の目安線、ホバーで内訳、クリックでそのフレームを表示。

### C. GPU パスの一本化
`src/Engine/Engine/Rendering/RenderSystem.cpp`
- `Render` の末尾で `prof::ReportGpu(name, ms, ranThisFrame)` を既存 GpuTimer 約 25 本ぶん呼ぶ。ウィンドウ側の手書き条件分岐（`ProfilerWindow.cpp:51-171`）と古い値が出る問題を解消。既存の `*GpuMs()` アクセサは dump が使うので残す。

### D. スクリプト別・エンティティ別（案4・5、ABI 変更なし）
- `prof::ItemScope(category, name, EntityID)` を追加し、次の直列ループに挟む。
  - C++ スクリプト: `ScriptHost.cpp:513-523`（`RunningScriptScope` の位置。名前は `ScriptType::name`）とイベント dispatch `:102-194`
  - C# スクリプト: `ManagedHost.cpp:402-411`（`CsType::name`）と dispatch `:420-494`
  - BehaviorTree: `BehaviorTreeSystem.cpp:1995-2008` の `StepOwner`
  - GridNav2D: `GridNav2D.cpp:440-441` の `UpdateAgent`
  - Fracture: `FractureSystem.cpp:801` の RootJob ループ
- フレーム内で「名前別の合計・回数」と「エンティティ別の上位」を固定長表に集計。
- **計測レベル**（軽い / 標準 / 詳細）を `prof::SetDetailLevel` で切替。項目別計測は「詳細」のときだけ有効（QPC 2 回 / 呼び出しのコストを常時払わない）。
- Perception / Particles（ワーカー並列）と Physics / Nav（一括処理）はエンティティ別の対象外と明記。
- 行クリック → `Selection::SelectOnly(scene->EnsureFileId(e))`（手本: `SearchWindow.cpp:88`）。`ProfilerWindow::OnImGui` に `Selection&` を追加（`EditorApp.cpp:878`）。**`EnsureFileId` はコンポーネントを追加しうるので、Play 中は FileId を既に持つエンティティだけ選択可能にする**（実装時に非変更の参照手段を確認）。

### E. 自動記録（案1）と予算（案8）
- フレーム時間がしきい値（既定 20 ms、設定可）を超えた確定フレームを「記録」領域（最大 32 件、古い順に破棄）へコピー。
- 予算は名前 → ms の表を `EditorSettings` に保存。超過行は赤、概要タブに一覧。通知は `ToastCenter::Notify`（頻度制限つき、ログには出さない = Console を汚さない）。

### F. メモリ（案11）
`src/Engine/Core/Diagnostics/MemoryTrack.cpp`
- `thread_local` のタグ + タグ別 `atomic` 配列（確保回数 / バイト）。`prof::MemTagScope`。タグ: 一般 / 描画 / 物理 / 音響 / アセット / スクリプト / エディタ / ECS。ロックなし・コンテナなしの方針は維持。
- フレームあたりの確保回数・バイトは確定時に前回値との差で算出。
- 生存バイトは `_msize` で解放側を数える案を**スパイクで計測してから採否を決める**（`delete` のコスト増を PerfBenchmark で確認。悪化するなら生存「数」のみ）。
- タグ別の「生存バイト」は出さない（解放側でタグを知る手段が無いため）と明記。

### G. GPU メモリ（案12）
`src/Engine/Renderer/Device/`（新規 `GpuMemoryTracker.h/.cpp`）
- 登録点: `RenderTexture::Create`（`RenderTexture.cpp:30,71`）、`gpubuf::CreateStructured/CreateConstant`（`GpuBufferUtil.h:14,52`）、`TextureLibrary::CreateFromPixels` / `LoadDdsInto`（`GpuResources.cpp:691,897`）、`MeshLibrary::UploadBuffers`（`:302`）、`VolumeTexture`。
- バイト数は desc（幅・高さ・フォーマット・ミップ）から計算。公開は POD（種類 / 名前 / バイト）のみ。
- `IDXGIAdapter3::QueryVideoMemoryInfo` でプロセス全体の使用量と予算を取得し、**「追跡済み / 全体」の差 = 未追跡分**を表示（パス直書きの `CreateBuffer` 約 55 箇所は未追跡と明記。全箇所の置換はこの計画の範囲外）。

### H. アセット読み込み（案13）
- `prof::AssetLoadRecord{種類, パス, ms, frame}` の固定長リング（256 件）。
- 計時点: `TextureLibrary::LoadFile`（`GpuResources.cpp:715`）、非同期はデコード時間をワーカーで測り `PollAsyncLoads`（`:1000`）でメインスレッドから記録、`ModelLoader::Load` / `FbxLoader::Load`、`AudioSystem::LoadClipFile`（`AudioSystem.cpp:877`）、`SceneSerializer::LoadFromFile`（`SceneSerializer.cpp:914`）、`ShaderManager` のコンパイル、`ReloadHub::HandleChange`（`ReloadHub.cpp:268`）、`DllReloader::TryCopyAndLoad`。
- キュー長の getter を追加: `TextureLibrary::PendingAsyncCount()`、`ShaderManager::PendingCompileCount()`。

### I. ジョブ（案14）
`src/Engine/Core/Jobs/JobSystem.h/.cpp`
- ワーカーへ index を渡し（`JobSystem.cpp:38`）、`DrainChunks` の `(*b.fn)(begin,end)`（`:88`）前後の QPC をワーカー別 `atomic` に積む。呼び出し側の `cvDone_` 待ち時間（`:161-163`）も計測。
- `Stats` を拡張（ワーカー別稼働時間 / フレームあたりバッチ数 / 待ち時間）。EngineLoop がフレーム確定時に取り込む。

### J. 被りの修正
- トースト: アンカーを作業領域の右下から**中央ドックノード（Scene / Game）の右下**へ変更。`EditorApp` が中央ノードの矩形を求めて `toasts_.OnImGui(anchorRect)` に渡す（`ToastCenter.cpp:97-99,108`、`EditorApp.cpp:1168`）。
- 既定ドック: Profiler を `rightBottom` → `bottom`（Console と同じ下段、`EditorApp.cpp:2077`）。レイアウトの版管理は無いので、既存の imgui.ini には「レイアウトをリセット」で反映される旨を報告に書く。

## 実装ステップ（各ステップ = ビルドと selftest が通る 1 状態、1 コミット）

| # | 内容 | 主なファイル | 検証 |
|---|---|---|---|
| 0 | 本計画書を `plans/profiler-overhaul.md` に保存 | `plans/` | 差分確認 |
| 1 | 被り修正（J） | `ToastCenter.*`, `EditorApp.cpp` | 撮影で重なりが無いこと |
| 2 | フレーム確定・履歴・前順・合算（A）、Present 計時（B の計測側） | `Profiler.*`, `EngineLoop.*`, 新規 `ProfilerSelfTest.*` | selftest（順序 / 合算 / リング周回） |
| 3 | UI の骨組み: 帯・プルダウン・タブ・表、概要 / CPU / 描画 / システム、積み上げグラフ、設定保存 | `ProfilerWindow.*`（タブ別にファイル分割）, `EditorSettings.*`, `LocalizationTable.inl` | 撮影（日英）、localization selftest |
| 4 | GPU パス一本化と GPU タブ（C） | `RenderSystem.cpp`, `Profiler.*` | 撮影、`--render-stats-dump` の出力が変わらないこと |
| 5 | 自動記録・履歴タブ・予算（E） | `Profiler.*`, `ProfilerWindow*` | selftest（しきい値 / 上限 32）、撮影 |
| 6 | スクリプト別・エンティティ別・計測レベル（D） | `ScriptHost.cpp`, `ManagedHost.cpp`, `BehaviorTreeSystem.cpp`, `GridNav2D.cpp`, `FractureSystem.cpp` | selftest、`replay_verify`（決定性）、三校プロジェクトで実表示 |
| 7 | メモリのタグ・フレーム差分（F）※ `_msize` はスパイク先行 | `MemoryTrack.cpp`, `Profiler.*` | selftest、PerfBenchmark で悪化なし |
| 8 | GPU メモリ（G） | 新規 `GpuMemoryTracker.*`, `RenderTexture.cpp`, `GpuBufferUtil.h`, `GpuResources.cpp`, `VolumeTexture.cpp`, `GraphicsDevice.cpp` | バイト計算の selftest、撮影 |
| 9 | アセット読み込み（H） | `GpuResources.cpp`, `ShaderManager.cpp`, `ReloadHub.cpp`, `DllReloader.cpp`, ローダ各種 | テクスチャ差し替えで履歴に出ること |
| 10 | ジョブ統計（I） | `JobSystem.*` | 既存 `JobSystemSelfTest` の拡張 |

- ステップ 1〜3 だけで「被らない・揃う・順位が読める」まで到達する。4 以降は独立に止められる。
- ファイルを追加したステップでは `pwsh tools\gen_project_files.ps1` を実行する。
- コミット件名は既存慣例（`M102a: …` のようなマイルストーン接頭辞。番号は着手時に `git log` で確認）。ステップ 2・4・6・8 のような設計変更は `engine_spec.md` の該当節も更新する。

## 検証（全体）

1. **ビルド**: `MyEngine.sln` を Debug|x64 と Release|x64 で MSBuild。
2. **自己テスト**: 途中は `MYE_SELFTEST_ONLY=<関連スイート>` で `Editor.exe --selftest`、最後に `--selftest-full`（fracture shutdown の既知の揺らぎは再実行で判定）。
3. **静的ルール**: `tools\check_rules.ps1`（規則 10 ローカライズ、規則 15 D3D 型の露出）。
4. **決定性**: `tools\replay_verify.bat`（ステップ 2・6・7・10 の後。計測が sim に影響していない証拠）。
5. **画面**: 今回と同じ手順で撮影して目視（`tools\collab_fixture.ps1` の fixture + `Editor.exe --project … --lang ja --shot-frame 40 --screenshot …`、`.mye\imgui.ini` で Profiler を浮かせる）。既定ドック・狭い幅・日英の 3 通り。
6. **実プロジェクト**: 三校（`C:\HAKtokyo\Shadow_Sound`）を開き、スクリプト別・エンティティ別・ランキングに実際の値が出ることを確認。見やすさの最終判定はユーザー（撮影画像を渡してフィードバックを受ける）。
7. **性能**: 計測レベル「軽い」でのフレーム時間が刷新前と同等であること（PerfBenchmark / `--render-stats-dump` の前後比較）。

## 範囲外（今回やらない）

- スクリプト**内部**の区間計測（ABI スロット追加が必要。v37 化は別計画）。
- タイムライン表示、フレーム同士の見比べ、Chrome tracing 書き出し、Replay 連動（今回は選ばれていない。tick 番号だけは記録に含めておく）。
- パス直書きの `CreateBuffer` / `CreateTexture2D` 全箇所の一元化（GPU メモリは「未追跡分」を表示して割り切る）。
- ワーカースレッド内のスコープ計測（Profiler のマルチスレッド化）。
