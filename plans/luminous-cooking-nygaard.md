# M51: ゲームプレイ基盤 (UI 重視) + 大規模化 (性能) + エディタ UX 一括

## Context

M50 完遂 (`d52e015`, ABI v11, kEngineVersion 0.65) 後の次期マイルストーン。ユーザー要望「大規模化 / 使いやすく / 様々なことができるように」に対し、3 軸調査で判明したギャップを **大型 M51 (M48 級、10 サブ)** で一括回収する。アニメーション強化 (スケルタル×AnimatorController 接続) はユーザー選択で今回見送り。

調査で判明した主要ギャップ:
- **大規模化**: 起動毎に全 FBX/glTF/WAV をフルパース (ディスクキャッシュ皆無、`DemoContent.cpp:448-506`)。`ForEachArchetype` 全走査 / `FindByFileId` 線形 (Prefab::Instantiate が O(メンバ×全体)) / Transform 毎 tick 全件再計算。
- **できること**: 入力アクションマッピングなし (生 VK 直書き)。UI はスクリプトから矩形を動かせない (NoHash 遮断)。ポーズ/タイムスケール/シーン跨ぎ永続/ランタイムセーブすべて皆無。C# から LoadScene・パッドが呼べない (Interop.cs 未公開)。
- **使いやすく**: AssetBrowser に検索/Delete/Duplicate なし。アセットファイル操作は Undo 完全非対応。ビルドはコピーのみ。

## 実装開始時の手順

1. 本計画をリポジトリ `plans\luminous-cooking-nygaard.md` へコピーしてコミット対象にする (マイルストーン運用の定型)。
2. 1 サブ = 1 コミット (`M51a:` 形式) = 1 セッション + /clear。進捗の一次情報は git log、進捗表には計画外の事実・罠・申し送りのみ書く。

## 再開手順 (セッション跨ぎ用)

1. `git log --oneline -5` で最後に完了した M51x を確認。
2. 本ファイルの該当サブの節を読んで着手。
3. 全サブ共通検証: Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS → `tools\replay_verify.bat` PASS → `tools\check_rules.ps1` 0 error。ソース追加時は `pwsh tools\gen_project_files.ps1`。M51h は `tools\build_managed.bat` 両構成も。
4. 新規 UI 文字列は `LocalizationTable.inl` に `MYE_STR(id, en, ja)` (規則 10)。
5. **M51g 以降は WorldHash 構成が変わる** (TimeControl/PersistStore 追記) — それ以前の手元 .rep は verify 不可 (replay_verify は毎回録り直すので無風)。

## 進捗表

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M51a Sim 索引 | **完了** | (このコミット) | 計画外の事実: ①**World::Clear は archetypes_ を破棄する** — 「append-only」はシーン寿命内の不変で、Clear でクエリキャッシュも必ず同時破棄 (selftest で固定)。②QueryCacheEntry は unique_ptr 保持 — ネストした ForEachArchetype が同ハッシュバケットへ別クエリを充填したとき、外側が掴む index 列が vector 再配置で無効化されるのを防ぐ。③追記マッチはゲート OFF 中も無条件維持 (実行中トグルで集合が欠けないように)。④EnsureFileId をイテレーション中に呼ぶと scratch 段階の値をキャッシュし得るが、ヒット時検証が弾いて自己修復するので無害。⑤起動時間は現状シーン規模ではノイズ内 (3.84s vs 3.87s、支配項は起動時アセットパース = M51b の対象)。効果は構造改善 (Prefab::Instantiate O(メンバ×全体)→O(メンバ)) で、透過性は 4 交差 (ON rec→OFF verify / 逆 × demo/parts) の 600 tick ビット一致で実証 |
| M51b アセットクック | 未着手 | | |
| M51c Transform スキップ | 未着手 | | |
| M51d アクションマップ | 未着手 | | |
| M51e UI レイアウト | 未着手 | | |
| M51f UI オーサリング | 未着手 | | |
| M51g ゲームフロー | 未着手 | | |
| M51h ABI v12 束ね | 未着手 | | |
| M51i AssetBrowser UX | 未着手 | | |
| M51j ビルド + 統合デモ | 未着手 | | |

---

## 決定台帳 (主要 8 論点の確定事項)

### 1. アセットクック
- 形式: `<project>\cache\cooked\<guid 16hex>.mmdl` (モデル) / `.mpcm` (.ogg PCM)。GUID は `AssetDatabase::GuidForPath` (パスハッシュ + .meta でリネーム耐性)。レガシー起動/配布ビルドは `<exeDir>\cache\cooked\`。
- 無効化: ヘッダ `{magic, kCookVersion, guid, srcSize, srcMtime, srcContentHash}`。size+mtime 一致→即有効、不一致→コンテンツハッシュ再計算で一致なら mtime のみ更新、不一致で再クック。`kCookVersion` bump で全無効化。
- 挿入点は Editor/Runtime 共通の `RegisterAssetLibraries` (`DemoContent.cpp:448`) のみ。`ModelLoader::Load` (D&D 配置経路) は従来パースのまま。.wav は展開費用ほぼゼロなので対象外。
- クック blob はパース結果の生バイト保存 (float ビットパターン維持)。フレッシュパースとのビット一致を selftest で強制。

### 2. コアホットパス
- 入れる (全て「結果不変・計算省略」型): クエリキャッシュ (archetype append-only を利用、新アーキタイプ生成時に追記マッチ、**列挙順 = 生成順を厳守**) / fileId→EntityID 索引 (ヒット時 IsAlive+値検証つきキャッシュ、stale なら線形フォールバックで補修) / `FindTypeIndex` 二分探索 (Types() は TypeId 昇順)。
- `Scene::Find` (名前) は据え置き (重複名「先勝ち」意味論を守る)。**空間加速構造は見送り** (M52 候補)。
- A/B ゲート: `EngineConfig::useSimCache` + `--no-sim-cache` (useJobs と同型)。**ON で record → OFF で verify のビット一致が透過性の証明**。

### 3. UI 拡張
- 矩形操作は **write-only 専用スロット** (SetUIRect/SetUILayout/SetUITexture)。**UIElement のハッシュ対象化は不可** — C# レーンは record/verify 中走らないため、C# が書いた UI 値がハッシュに入ると恒常 MISMATCH。演出レーン分離と構造矛盾。
- 追加フィールド (NoHash 末尾 append = シーン互換): `space` (0=screen/1=親矩形基準, opt-in) / `clipChildren` (シザー) / `align` / `wrap`。
- 矩形解決を `UILayout` に抽出し **UIRenderer / UINav / UIHitTest の 3 者で共有** (描画とヒットテストのズレを構造的に断つ)。
- Canvas スケーリング見送り (ウィンドウ実寸読取は恒久禁止事項に抵触)。代わりに `UIHitTest(x,y)` スロット。スクロールは親クリップ + 子オフセット + ホイール ABI で「組める」状態がゴール。
- `GetMouseWheel`: wheelDelta は InputSnapshot に既在 (`Input.h:14`) → スロット追加のみ、ReplayFile 不変。

### 4. 入力アクションマッピング
- `assets\input\actions.json` (actions: name/keys/pad/mouse、axes: posKey/negKey/padAxis/deadzone)。不在時は空マップ = 完全 no-op。
- 評価は `Evaluate(cur, prev)` の**記録済み InputSnapshot 純関数** = 決定論安全。prev は EngineLoop 保持 (tick 0 はゼロ値)。
- ABI: `GetActionState(hash)` (bit0=held/1=pressed/2=released) + `GetAxisValue(hash)` の 2 本 (FNV 名前ハッシュ、FindPartsByTag と同型)。
- エディタ: ProjectSettings に Input セクション (一覧 + キー捕捉 + 保存ホットリロード + ライブ状態表示)。

### 5. ゲームフロー
- **ポーズ/タイムスケール = tick ゲート方式** (dt スケール不採用 — 整数 tick 時刻と噛み合わない)。`TimeControl {paused, scalePercent, accum}` を Scene 保持の sim 状態として **WorldHash に追記** (RNG の直後、M32a ageTicks 前例)。ゲート対象 = 物理/CC/アニメ/パーティクル/タイマー。**C++ スクリプトは非ゲート** (でないと sim レーンからアンポーズ不能)。入力/ハッシュ/記録/構造変更適用も常時実行。
- **シーン跨ぎ永続 = key-value ストア** (DontDestroyOnLoad 不採用 — world.Clear 前提全域に例外を掘る工事はリスク過大)。`std::map<uint64, vector<uint8_t>>` (ordered = 決定論走査) を Scene 外で保持し hashed、LoadScene で温存。ABI は `PersistSet/PersistGet` blob 2 本 + 型付き糖衣。
- **セーブ/ロード**: `SaveGame(slot)` = PersistStore + 現シーンパスを `<project>\save\<slot>.json` へ **tick 末ハッシュ後に書く** (出力レーン = 決定論を汚さない)。`LoadGame` は pendingScene と同じセーフポイントで消費、**record/verify 中は no-op + WARN** (「リプレイはセーブ読込を跨がない」を仕様として明文化)。

### 6. ABI v12 束ね
- **M51h の 1 サブで 14 スロットを末尾 append** (73→87、bump 1 回の M48h 運用): GetMouseWheel / SetUIRect / SetUILayout / SetUITexture / UIHitTest / GetActionState / GetAxisValue / SetTimeControl / GetTimeControl / PersistSet / PersistGet / SaveGame / LoadGame / SetPadVibration。
- SetPadVibration は XInput 直行の出力レーン。verify 中 suspend (オーディオ同型)、フォーカス喪失/終了で 0 リセット。
- C# 未公開分の回収: LoadScene / パッド 4 本 / 無印 Overlap・SphereCast の糖衣を Interop.cs に追加。
- **ミラー機械照合 = check_rules.ps1 規則 11** (C++ selftest 不採用 — バイナリから C# ソースは引けない): ①EngineAPI.h の関数ポインタ名リストと Interop.cs のスロット名リストの順序・件数・名前完全一致 ②引数個数照合 ③MYE_API_VERSION 変更とスロット数変更の同時性。**まず現行 73 本で PASS を確認してから v12 を足す** (検査器自体の検証)。

### 7. エディタ UX
- Delete = `IFileOperation` + `FOF_ALLOWUNDO` で**ごみ箱送り** + 確認モーダル (.meta 同伴)。UndoStack 統合はしない (ごみ箱が復元手段)。
- Duplicate = コピー + 連番 + **新規 GUID の .meta 発行** (旧 .meta コピーは GUID 衝突でシーン参照破壊 — 本サブ最大の罠)。Ctrl+D。
- アセット操作 Undo は**逆操作が安全な 4 種のみ**: Rename / Move / Duplicate / Create。Delete・Import は対象外。マテリアル編集等の編集系 Undo 穴は今回スコープ外。
- ビルドワンストップ: ①スクリプト自動リビルド (既存 RebuildGameLogic / C# コンパイルを呼ぶ — MSBuild 起動不要) ②モデル/オーディオクック温め + `cache\cooked\` 同梱 ③DDS 一括クック (opt-in) ④zip (opt-in)。未使用アセット除外は見送り (依存グラフ未整備)。
- ショートカットリバインドは見送り (M52 候補)。

### 8. 依存順序
性能 3 連 (a 索引 → b クック → c Transform) を先頭に (b が最大リスクなので 2 番目で早期露見)。次にエンジン内部機能 (d 入力 → e UI ランタイム → f UI オーサリング → g ゲームフロー)、**h (ABI 束ね) は全スロット確定後**。i (AssetBrowser) は独立、j (ビルド + 統合デモ) が b と h を消費して締める。

---

## サブ分割 (10 分割)

### M51a: Sim 索引 — クエリキャッシュ + fileId 索引 + 型二分探索
- **目的**: `ForEachArchetype` 線形マッチ (`World.h:63-82`)、`FindByFileId` 全走査 (`Scene.cpp:7-24`)、`FindTypeIndex` 線形を潰し、A/B ゲート基盤 (`useSimCache`) を敷く。
- **触る**: `src\Engine\Core\World.h/.cpp`, `Core\Archetype.h`, `Engine\Scene.h/.cpp`, `Engine\EngineLoop.h`, `src\Runtime\RuntimeMain.cpp`, `src\Editor\EditorMain.cpp`, selftest 追加。
- **検証**: selftest (キャッシュ後のアーキタイプ追加をクエリが拾う / fileId 破棄→再生成→索引補修 / 型索引と線形の全件一致) / replay_verify 無風 / **ON record → `--no-sim-cache` verify ビット一致 (逆向きも)** / parts_showcase ロード時間 before/after 計測。

### M51b: アセットクックキャッシュ — モデル + .ogg PCM
- **目的**: 起動毎フルパースをディスクキャッシュ化、起動時間 1 桁短縮。`engine_spec.md:420-428` の TBD 回収。
- **触る**: `src\Engine\Engine\Asset\CookedCache.h/.cpp` (新規), `Asset\ModelCook.h/.cpp` (新規), `Renderer\FbxLoader.cpp`, `Renderer\ModelLoader.cpp`, `Engine\DemoContent.cpp`, Audio の LoadClipFile 相当, `engine_spec.md`。`--no-cook-cache` CLI。
- **検証**: selftest (**フレッシュパース vs クック往復の memcmp 一致** — 頂点/インデックス/スキン行列/クリップ) / **replay_verify をコールド (cache 削除) record → ウォーム verify** / 起動時間 before/after / ソース 1 バイト改変→再クック、mtime のみ変更→ハッシュ一致で再クックされない。

### M51c: Transform 比較スキップ + Rebuild O(N) 化
- **目的**: 毎 tick 全件再計算 (`TransformSystem.cpp:102-120`) を比較スキップに、Rebuild の深度計算を O(N) に。
- **設計**: dirty フラグ不採用 (LocalTransform は生ポインタで書かれるためフック不能)。前回 TRS (10 float) + 親の変化世代を側 table 保持、ビット比較で不変ならスキップ。**スキップ = 前回値温存 = 再計算とビット同一**。Rebuild は親チェーンメモ化で 1 パス。useSimCache ゲート共用。
- **触る**: `src\Engine\Engine\TransformSystem.h/.cpp`。
- **検証**: replay_verify 無風 / **ON record → OFF verify ビット一致** / selftest (親移動で子孫全更新・兄弟スキップの件数検査) / Profiler で静的シーン tick ms before/after。

### M51d: 入力アクションマッピング (ABI は M51h)
- **目的**: JSON アセット + エンジン内評価器 + ProjectSettings 編集 UI。
- **触る**: `src\Engine\Platform\InputActions.h/.cpp` (新規、`Load` + `Evaluate(cur, prev)` 純関数), `Engine\EngineLoop.h/.cpp` (prev 保持 + スナップショット確定直後に評価), VK⇄名前テーブル `.inl` (新規), `src\Editor\Windows\ProjectSettingsWindow.cpp`, `assets\input\actions.json` サンプル。
- **検証**: selftest (held/pressed/released 全遷移 + deadzone/軸合成 + 不正 JSON 耐性) / replay_verify 無風 / 手動: キー捕捉→保存→ライブ表示。

### M51e: UI ランタイム拡張 — 親子・クリップ・整列・折返し
- **目的**: メニュー/ダイアログが「組める」UI ランタイム。UIElement 末尾 append (`space`/`clipChildren`/`align`/`wrap`) + UIRenderer 拡張。
- **触る**: `src\Engine\Core\Components.h`, `Engine\UI\UILayout.h/.cpp` (新規 `ResolveRect` — UIRenderer/UINav/UIHitTest 共有), `UI\UIRenderer.h/.cpp` (シザー分割 + `MeasureText/LayoutText` 抽出), `UI\UINav.h`, `Renderer\FontAtlas.h`。wrap は文字単位折返し (日本語優先)。
- **検証**: replay_verify 無風 (NoHash append の実証) / selftest (ResolveRect: 9 アンカー × 入れ子、LayoutText: 折返し行数・整列オフセット) / 手動目視。

### M51f: UI オーサリング — Create メニュー + Inspector
- **目的**: UI を Add Component 手組みから解放。
- **触る**: `src\Editor\CreateMenu.cpp/.h` (UI > Panel/Image/Button/Text、選択が UIElement 持ちなら子として space=1)、`Windows\InspectorWindow.cpp` (anchor 9-grid ピッカー、kind/align/fillMode コンボ)、`Windows\GameViewWindow.cpp` (選択 UI の解決済み矩形アウトライン — UILayout 共有)、`LocalizationTable.inl`。既存 Undo 定型に乗せる。ビューポート内ドラッグ編集は見送り (M52 候補)。
- **検証**: replay_verify 無風 / 手動: Create→描画 / Undo 往復 / 9-grid 全切替 / アウトラインと描画矩形の一致。

### M51g: ゲームフロー — ポーズ/タイムスケール + 永続ストア + セーブ/ロード (**WorldHash 構成変更**)
- **目的**: 決定台帳 5 をエンジン内部に実装 (ABI は M51h)。
- **触る**: `src\Engine\Engine\Scene.h/.cpp` (TimeControl/PersistStore メンバ + `Time()`/`Persist()` API), `Engine\EngineLoop.cpp` (`ShouldStep()` ゲート + SaveGame 書出をオーディオ drain の隣 + LoadGame をセーフポイント消費), `Replay\WorldHasher.h/.cpp` (RNG 直後に追記、PersistStore は key 昇順), `src\Editor\PlayModeController.h` (**スナップショット/復元に TimeControl+PersistStore 追加 — 忘れると Stop 後に永続値が漏れる、本サブの罠筆頭**), `Engine\SaveGame.h/.cpp` (新規)。
- **検証**: replay_verify PASS (録り直しなので無風) / selftest (scalePercent=50 で 600 tick 中 300 ステップ / PersistStore 挿入順を変えて同ハッシュ / Save→Load 往復一致) / 手動: Play 中ポーズ→物理静止・C# UI 継続、Stop→Play で永続値が残らない。

### M51h: ABI v12 束ね + Interop 公開 + ミラー機械照合 (MYE_API_VERSION 11→12)
- **目的**: 決定台帳 6 の 14 スロット開通 + C# 未公開分回収 + 規則 11 恒久化。d/e/g のエンジン内 API への薄い委譲に徹する。
- **触る**: `src\Shared\EngineAPI.h` (v12 ブロック + 履歴コメント), `Shared\ScriptAPI.h` (糖衣), `Engine\Script\EngineApiTable.cpp`, `src\Scripting\Interop.cs` (末尾 append + 糖衣: Input.GetAction / UI.SetRect / Game.Pause / Persist.GetInt 等), `tools\check_rules.ps1` (規則 11), PartSelfTest のバージョン定数。
- **検証**: 8 ビルド + build_managed 両構成 / 規則 11 が 87 スロット一致を報告 + **変異テスト (Interop.cs の 1 スロット入替→検出→復元)** / replay_verify 無風 / C++ テストスクリプトで全新スロット実走 / C# temp probe で糖衣疎通 (ABI 検証レシピのとおり)。

### M51i: AssetBrowser UX — 検索/型フィルタ/Delete/Duplicate + アセット操作 Undo
- **目的**: AssetBrowser を「消せる・増やせる・見つかる」に。
- **触る**: `src\Editor\AssetOps.h/.cpp` (`DeleteAssetToRecycleBin` = IFileOperation + .meta 同伴 + フォルダ再帰 / `DuplicateAsset` = 新 GUID 発行 / Rename・Move・Duplicate・Create に UndoStack 引数 — 逆ファイル操作エントリ、逆操作先消滅時は WARN + no-op), `Windows\AssetBrowserWindow.cpp/.h` (検索 + AssetType フィルタ + 再帰検索モード + 確認モーダル + Delete/Ctrl+D), `LocalizationTable.inl`。削除前のシーン参照チェックはやらない (ごみ箱 + GUID 安定が保険)。
- **検証**: selftest (Duplicate の新旧 GUID 不一致 + 連番命名) / 手動: Delete→ごみ箱→OS 復元→再走査で復活 / Rename→Undo→シーン参照維持 / replay_verify 無風。

### M51j: ビルドワンストップ + 統合デモ + 仕上げ
- **目的**: ビルド強化 + M51 全機能を消費する統合デモで締める。
- **触る**: `src\Editor\Windows\BuildSettingsWindow.cpp/.h` (段階化: スクリプトリビルド→クック温め→コピー + `cache\cooked\` 同梱→DDS 一括 opt-in→zip opt-in、各段の進捗表示), `Engine\DemoContent.cpp` (`--flow-demo`: タイトル→ゲーム→ポーズ→リザルト。C++ = アクションマップ/ポーズ/PersistStore スコア持ち越し/セーブロード、C# = メニュー UI), `tools\replay_verify.bat` (**3 ペア目 --flow-demo 追加** — TimeControl/PersistStore/アクションマップの 600 tick 検証 = M51 決定論保証の総括), `engine_spec.md` 更新。
- **検証**: replay_verify 3 ペア PASS / パッケージ出力を別フォルダで実行→クック済み高速起動 + デモ完走 / zip 展開→同様 / DDS opt-in で描画不変 (目視)。

---

## 見送り (M52 候補メモ)

- 空間加速構造 (物理クエリ/ブロードフェーズの決定論 BVH、sort&sweep 軸リストの Overlap 流用)
- スケルタルアニメ × AnimatorController 接続 + パラメータ拡張 (float/bool/trigger) + ブレンドツリー
- ショートカットリバインド / Canvas スケーリング / UI ビューポートドラッグ編集
- 未使用アセット除外 (依存グラフ整備が前提) / マテリアル・クリップ・コントローラ編集の Undo
- LOD / オクルージョン / シーン追加ロード / JobSystem 汎用化

## 実装セッション冒頭で要確認 (未検証事項)

- FbxLoader/ModelLoader の RegisterAssets 内部構造 (M51b の 2 段切り出しの切れ目)
- AudioSystem::LoadClipFile の実シグネチャ
- UIRenderer のバッチ構造 (シザー分割コスト)
- PlayModeController のスナップショット実装 (M51g の追加点)
- Interop.cs の「ミラー struct 有り・糖衣無し」の正確な境界
