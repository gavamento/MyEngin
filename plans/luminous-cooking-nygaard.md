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
| M51b アセットクック | **完了** | (このコミット) | 計画外の事実: ①ヘッダに **srcPathKey** を追加 — サブアセット AssetID は正規化パス由来なので、移動後のフレッシュパースは別キーを登録する。クックだけ旧キーを再生すると意味論が割れるため、パス不一致は再クック (GUID ファイル名は台帳どおり)。②外部テクスチャは deps として**存在のみ**検証 — 内容はリプレイ時も再ロード/再デコードする (= .meta インポート設定が生き続ける。テクスチャ画素のクックは M51j の DDS 一括が本丸)。③sink はキーで重複排除 — glTF はプリミティブ毎に材質を、FBX はインスタンス毎にメッシュを再登録するため、素直に記録すると埋め込みテクスチャが blob 内で重複肥大する。④**Deserialize は count を「最小要素サイズ×個数 ≤ 残量」で検算してから resize** — 検算なしだとジャンク blob の count で bad_alloc 即死 (selftest の破損テストが実際に踏んで abort、exit 3)。⑤性能実測 (Debug): 現状アセット (6 モデル) では走査 ~280ms 中パース成分が小さくウォーム短縮はノイズ内。CesiumMan×20 でも 4% — 支配項は埋め込み PNG デコード (設計上リプレイでも再実行)。小型 FBX×20 はリプレイ≒パース (per-file 固定費支配)。**「1 桁短縮」は現アセット規模では対象成分自体が小さい** — 効果は重量級モデル導入時。なお総起動 ~5.5s の支配項は走査 (~0.3s) ではなく D3D/シェーダ/CoreCLR/C# コンパイル側 (M52 候補)。⑥書きたて .mmdl の初回読みは AV スキャンで一過性に遅い (556→300ms 収束)。⑦Debug/Release のクック blob はバイト一致。⑧assets に .ogg 実アセットは現在 0 — .mpcm の実被覆は selftest 合成往復のみ。⑨`[assets] startup asset scan: N ms` ログを恒久追加。検証実績: selftest 全 PASS 両構成 / replay_verify 2 ペア (コールド record→ウォーム verify) / A/B 2 交差 (ウォーム rec→--no-cook-cache verify / 逆) 600 tick ビット一致 / **blob 改竄の識別テスト**で再生経路の実使用を実証 / 実モデルで mtime のみ→ヒット維持・1 バイト改変→ミスをログ確認 |
| M51c Transform スキップ | **完了** | (このコミット) | 計画外の事実: ①側テーブルは lastTrs_ (TRS 10 float 生ビット) + state_ (kSlotValid/kSlotChanged の 2 ビット、EntityID.index 添字)。子の据え置き判定は「親スロット == kSlotValid ちょうど」— 親はレベル間バリアで今 tick 確定済み、テーブル外/無効/再計算済みは全部「変化あり」に倒す。②Rebuild の O(N) メモ化は旧チェーン歩きと**厳密同値**が必須 (depth は HierarchyComponent 内 = WorldHash 対象): 壊れ親 (死亡/世代不一致/Hierarchy なし) は「1 段数えて打ち切り」まで再現。③WorldMatrixComponent の書き手唯一性を grep で確認 — 直書きは Particle/Vfx selftest のヘッドレスヘルパのみ (TransformSystem 非使用で無干渉)、gizmo は LocalTransform 経由 = memcmp が捕捉。④実行中トグル OFF→ON は自己整合 (OFF 期間は毎 tick 全再計算で wm が常に正しい → 復帰後の skip は「現 wm == 再計算結果」の温存で安全)。⑤性能は現シーン規模ではノイズ内 (verify 600 tick 壁時計 ON≒OFF: demo 11s / parts 7s、支配項は起動費用) — 効果は構造改善 (静的エンティティの行列再計算が 0 に、selftest が computed/skipped 件数を機械検証)。検証実績: 両構成 0 警告 / selftest 全 PASS 両構成 / replay_verify 2 ペア + check_rules 0 error / A/B 4 交差 (ON rec→OFF verify / 逆 × demo/parts) 600 tick ビット一致 |
| M51d アクションマップ | **完了** | (このコミット) | 計画外の事実: ①評価の挿入点は「スナップショット確定直後」ではなく **tick ループ内・verify 入力置換の直後** — ReplayPlayer が tick 毎に ctx.input を置換するため、フレーム頭で評価すると verify で記録入力を見ない。prev は EngineLoop ローカル (tick 0 ゼロ値)、複数 tick/フレームでも pressed は先頭 tick のみ立つ (リプレイと同義)。②VK 名テーブルは**汎用修飾キーのみ収載** — WM_KEYDOWN の wParam は Shift/Ctrl/Alt を汎用 VK (0x10-0x12) で届けるので L/R 分離名 (0xA0-) は決して押下にならない死に割り当てになる。未収載 VK は "0xNN" 16 進名で JSON 往復可能。③MyePadButton 値は Platform 層へ再掲 (Shared を include しない。ABI 定数なので不変)。④Save は Load(force) の読み直しで正規形化 — 重複名・不明キー名はこのとき WARN + 脱落する仕様。⑤エディタのキー捕捉は ImGui 非依存 (エンジンスナップショットの押下エッジ差分、0x00-0x07 のマウス VK は除外、Esc 取消)。⑥ABI/スクリプト公開は M51h なので評価結果はまだ誰も読まない = replay は構造的に無風 (実測 PASS)。実起動ログで 3 action / 2 axis のロードを確認済み |
| M51e UI レイアウト | **完了** | (このコミット) | 計画外の事実: ①anchor 意味論は**「矩形左上をアンカー基準点 + オフセットに置く」(センタリング無し)** — space=1 の親矩形基準でも同じ (旧 ResolveAnchor が (void)w で幅を無視していたのはこのため。selftest 期待値を一度これで誤った)。②矩形解決は uilayout::ResolveRect へ一本化し UIRenderer::ResolveAnchor / TextWidth / PushText は削除 (外部利用なしを grep で確認)。③UIFocusNav は祖先クリップで可視矩形が退化した要素を候補から除外 (スクロール外へフォーカスが飛ばない)。④ボタンラベルは PushTextInRect(align=4) に統一 — 単一行は旧式と同値、複数行ラベルは総高センタリングに改善。⑤clipChildren は子孫のみ切る (自分は切らない)。シザーは常時 ON・既定 RT 全域 = クリップ無しシーンはバッチ分割ゼロ。⑥FontAtlas.h は触らず (計測は FontGlyphMap 純関数で足りた — 計画 touch リストの過剰見積り)。⑦手動目視は DemoContent 改変不要 — scratchpad にプローブシーン JSON を手書きし `Runtime --scene X --screenshot Y --shot-frame 3` で撮影 (入れ子/2 段クリップ/クリップ外非表示/折返し/整列/ボタン回帰を 1 画面で確認)。検証実績: 両構成 0 警告 / selftest 全 PASS 両構成 (ResolveRect 9 アンカー × 入れ子 + 循環打ち切り + クリップ交差 + LayoutText 折返し・整列 21 項目) / replay_verify 2 ペア + check_rules 0 error (NoHash append の replay 無風を実証) |
| M51f UI オーサリング | **完了** | (このコミット) | 計画外の事実: ①**ボタンラベルは UIRenderer が白固定** (`UIRenderer.cpp` PushTextInRect の label) — 生成ボタンの既定色を白のままにすると白地に白文字で潰れるため、暗色 (0.22,0.27,0.38) を既定にした。②9-grid ピッカーは呼び出し側 HandleEditUndoMulti の定型に乗らない — あれは**直前 1 アイテム**の activate/deactivate しか見ないので、ボタン群はクリック即確定の自前 Undo (Collider.mask と同じ ownUndo 方式) にした。③`BeginPopupContextItem()` の既定 ID は「最終アイテムの ID」で、ラベルを TextUnformatted で締める field (mask、今回の anchor) は ID=0 で IM_ASSERT に落ちる**既存の潜在バグ** — f.name を明示して恒久回避 (プレハブメンバの Collider 選択で Debug ビルドが assert する穴だった)。④imgui 1.92.8 で `AddRect` の thickness/flags 引数順が入替 (旧順序は `= delete`)。⑤World::AddComponent は非イテレーション中は即時適用 → エディタメニュー経路では生成直後の GetComponent が有効 (space=1 の後書きが安全)。⑥UI 生成の親決定: 明示 parent 優先、無ければ選択 primary が UIElement 持ちのときその子 + space=1。⑦エディタ GUI の実機目視 (メニュー生成/Undo 往復/9-grid 切替/アウトライン一致) は未 — 既定値の見た目は Runtime プローブシーン (scratchpad) のスクショで確認済み (パネル/ボタン/テキスト/画像 + 子 space=1 配置)。検証実績: 両構成 0 警告 / selftest 全 PASS 両構成 / replay_verify 2 ペア + check_rules 0 error |
| M51g ゲームフロー | **完了** | (このコミット) | 計画外の事実: ①決定台帳の「PersistStore は Scene 外で保持」は「シーン文書/world の外」の意で、実装は計画 touch リストどおり **Scene メンバ** — Scene::Clear が time_/persist_ を触らないことで LoadScene (LoadFromJson→Clear) 生存を実現。②ゲートは `stepSim = ctx.simulateScripts && scene.Time().Advance()` の 1 変数で 3 ブロック差し替え (アニメ 3.5 = エフェクト duration/linger タイマー込み / 物理 3.6 = CC 込み / 衝突・パーティクル・トレイル 4)。**短絡評価が意図** — 編集中 (simulateScripts=false) は Advance が呼ばれず accum 凍結 = Play 開始位相が予測可能。③scalePercent >100 は 100 扱い (tick ゲートは 1 tick 1 ステップ上限 — 早送り非対応)、accum はポーズで凍結し再開時は続きから。④Scene に SourcePath 追加 (SaveToFile/LoadFromFile が設定) = SaveGame の「現シーンパス」。メモリ構築シーン (デモ) は空パスで保存され、LoadGame は persist のみ復元 (シーン切替なし)。⑤TimeControl は LoadScene / LoadGame を跨いで維持 (Unity timeScale 意味論 — スクリプト非ゲートなので新シーン側からいつでも解除可)。⑥セーブ書出は record/verify 中もゲートしない (出力レーン。台帳が no-op 指定するのは Load のみ)。シーンパスは assets 相対化して書く (プロジェクト移動耐性)、persist キーは %016llX 固定幅 = JSON 辞書順が数値順で出力決定論。⑦オーディオは非ゲート (出力レーン) — ポーズ中も BGM/SE は鳴り続ける (止めたければスクリプトから)。⑧selftest は **Editor 層** (PlayModeController 被覆のため、PartSelfTest と同じ理由)。申し送り: pendingSaveSlot/pendingLoadSlot の**積み手はまだ居ない** (M51h の ABI + SetSharedServices 拡張で開通。消費側と SaveGameFile は selftest 被覆済みだが EngineLoop 内の save/load 実走は未 — M51h の C++ probe で全新スロットと一緒に実走する)。エディタ実機目視 (Play 中ポーズ→物理静止 + C# UI 継続) も同じく M51h probe 待ち — TimeControl を触る入口が ABI までない。検証実績: 両構成 0 警告 / selftest 全 PASS 両構成 (TimeControl 位相 9 + hash 被覆 8 + PersistStore 意味論 5 + SaveGameFile 往復・破損 9 + PlayModeController 漏れ 3) / replay_verify 2 ペア PASS (WorldHash 新構成での録り直し) / check_rules 0 error |
| M51h ABI v12 束ね | **完了** | (このコミット) | 計画外の事実: ①規則 11 は「version⇄スロット数の対応表を検査器内に持つ」方式で同時性を強制 (11-a 順序・件数・名前 / 11-b 引数個数 = C# delegate* 型リスト数-1 / 11-c 版数表 / 11-d EngineApiTable の `out.<Name>=` 充填 + 抽出 0 件ガード)。**先に現行 73 本で PASS → 変異テストは v9 #69 / v12 #85 の両方で検出実証** (入替はタイプ列でなく名前だけ swap すると引数個数照合も同時に効く)。②**SetTimeControl は次 tick から効く** — ゲート (Advance) は tick 頭で確定済みで、スクリプトは phase 3 = その後に走る (Unity timeScale と同じ意味論。probe の期待を一度この罠で誤った)。③**LoadGame はセーブのシーンパスが非空なら同 tick でシーンごと再ロード** — スクリプトの登録フィールドはシーン文書の初期値へ戻る (C++/C# とも。probe のステージングが 1 回リセットされて実証)。scratchpad のシーンは assets 外 = 絶対パスで保存され、それでも復元される。④UIHitTest は ResolveVisibleRect (祖先クリップ適用) で判定・最前面 = order 最大→entity.index 最大 (UIRenderer の描画順と同一)。基準解像度 1920x1080 固定 — クライアント実寸スケーリングは M52 Canvas 待ち。⑤SetUIRect の w/h・SetUILayout の全パラメータは**負値 = 現値維持** (UI は write-only で読み返せないため partial 更新にはこの keep 意味論が必須)。⑥PersistGet は -1=不在 / 0=空 blob を区別。PersistSet は MYE_PERSIST_MAX_BLOB (64KB) 超を拒否 (WorldHash/セーブの肥大防止)。⑦振動は ScriptApiContext::PadVibrationState へ目標値を書くだけ → EngineLoop がフレーム末に record/verify/フォーカス喪失を 0 ゲートして Input::ApplyVibration (量子化値の変化時のみ XInputSetState 発行)。Win32Window::HasFocus 新設 (出力レーン専用 — sim から読むの禁止)。⑧C# ユーザースクリプトから MyeEntity.Id は internal で触れない — null 判定は IsAlive (probe のコンパイルエラーで実証)。⑨ヘッドレス実行では vsync が実効せず tick はフレーム数でなく実時間依存 (--frames 4000 で 139 tick — probe をフレーム数で見積もると足りない)。申し送り: C# LoadScene 糖衣は追加済みだが実呼出は probe 未実施 (下層 v3 スロットは既存被覆)。エディタ GUI 目視は引き続き未 — M51j flow-demo で総括。検証実績: 規則 11 = 87 スロット一致 + 変異 2 箇所検出 / 4 プロジェクト × 2 構成 0 警告 + build_managed 両構成 / selftest 全 PASS 両構成 (PartSelfTest が v12 テーブルと 14 スロット充填を検査) / replay_verify 2 ペア PASS (probe 削除後に再実測) / **C++ temp probe で全 14 スロット実走 21 checks 0 FAIL** — M51g 申し送り (EngineLoop の save/load 消費経路 + ポーズ中の物理 14 tick ビット同一凍結 + スクリプト継続) もここで実走回収 / C# temp probe 15/15 ×2 (シーン再ロード前後) |
| M51i AssetBrowser UX | **完了** | (このコミット) | 計画外の事実: ①新規ファイルなし (AssetOps/AssetOpsSelfTest は M30 系譜で既存、gen_project_files 不要)。②**UndoStack のファイル操作エントリは serial を現状態から継承** — 新規採番すると StateSerial が動き保存済みシーンが偽 dirty (未保存確認モーダル誤発火) になる。session も 0 固定 = EndPlaySession の破棄対象外 (ディスクは Play 停止で巻き戻らない)。③逆操作先消滅の Undo は WARN + no-op でも**エントリを redo 側へ送って消費** — 死んだエントリで Ctrl+Z が詰まらない。④Duplicate の追加罠: 宛先に**孤児 .meta** が残っていると EnsureMeta が旧 GUID を「尊重」して継いでしまう → コピー後に dst.meta を除去してから発行 (selftest で固定)。⑤Create Undo はスクリプト 2 種を対象外 — 既存ファイル時に「開くだけ」で同パスを返す経路と区別できず、Undo が既存ソースをごみ箱送りにするため。redo は bytes 書き戻しのみ (ライブラリ再登録・.meta 発行なし — 元の Create も作らず遅延解決で足りる)。⑥RecordAssetCreated は Create* の署名を変えず DoCreate だけが呼ぶ — InstantiateAssetAtPath 内部の CreateSoundAsset (シーン Undo の一部) を独立エントリに割らない。⑦Delete 後の assetDb テーブル除去は MoveAsset(path, path) 流用で足りる (新パス不在 = 再登録フェーズ no-op)。⑧フォーカス判定は ImGuiFocusedFlags_RootAndChildWindows (WindowFlags ではない)。⑨エディタ GUI 実機目視 (確認モーダル/OS 復元→再走査復活/検索 UI) は未 — M51j で総括。ごみ箱送り自体 (IFileOperation) は selftest が実走済み。検証実績: 両構成 0 警告 / selftest 全 PASS 両構成 (AssetOps に 15 項目追加 = 連番+新 GUID / 孤児 .meta 非継承 / Duplicate・Rename・Create の Undo/Redo 往復 / 消滅 no-op / dirty serial 不変 / ごみ箱削除 3 種) / replay_verify 2 ペア PASS + check_rules 0 error |
| M51j ビルド + 統合デモ | **完了** | (このコミット) | 計画外の事実: ①**LoadScene は record/verify 中も許可** (M19.4 の設計 — no-op なのは LoadGame だけ) → flow はタイトル⇄ゲームの**シーン遷移ごと**リプレイ被覆に入れた。②flow シーンの置き場は **assets\scenes\ 必須** — スクリプトの遷移リテラルが assets 相対解決 ("scenes/flow_*.scene.json") なので cache\ に置けない。builtin メッシュ + 名前マテリアルのみで組む = チェックアウト非依存、生成物 2 本は gitignore (正解はコード側 = parts の流儀)。③**cooked 同梱は封印 (.sealed) 方式が「正しさ」の担保**: srcPathKey は絶対パスで移設先は全ミス→再クック、再クックは移設先パス由来のサブアセット ID を登録して**配布シーンのモデル参照が全空振りする** (M15 以来の潜在バグを発見)。封印中は magic/version/guid のみ検証してクック時の登録列をそのまま再生 = どこに置いても元 ID が揃う。blob 内の外部テクスチャ絶対パスは "\assets\" テールを現行ルートへ付け替え (ModelCook::Replay)。④DDS 一括は「dist 内で変換 + 元画像削除 + ローダに『**ソース不在なら**同 AssetID で同名 .dds』フォールバック」 — 開発環境の挙動は 1 ビットも不変。LUT 等の非可逆回避は .meta compress=none (ツールチップ明記)。⑤スクリプトビルドの非対話化は **stdin=NUL** が要 (bat の失敗系 pause が EOF で即抜ける)。CreateProcessW + ログリダイレクト + 毎フレームポーリングで UI は生存。zip は Windows 標準 bsdtar (tar.exe -a)。⑥**--package DIR CLI を恒久追加** (--package-dds/-zip/-boot。GUI と同一パイプライン = パッケージ検証が自動化可能に)。⑦FlowGameDriver の resume 規約: セーブ直前だけ flow.resume=1 (SaveGame 後に 0 へ戻す = セーブファイルにのみ 1 が残る) → LoadGame 復帰だけスコア再開。⑧dist の Runtime は高 fps で frame≠tick (--frames 15000 ≒ 562 tick) — 完走確認はフレーム数を盛る (M51h 既知罠の再確認)。検証実績: 8 ビルド 0 警告 / selftest 全 PASS 両構成 (sealed 6 項目追加) / replay_verify **3 ペア** PASS + check_rules 0 error / CLI パッケージ 6 段 PASS (dds+zip) / **移設 dist 実走**: sealed 再生 + DDS フォールバック + モデル参照シーン完全描画 (スクショ) + flow 完走 (遷移 2 回 + セーブ書出) / zip 展開先でも同様。エディタ GUI 実機目視 (BuildSettings 窓 / M51f/i の GUI 系) は引き続き未 — 自動化不能のため次回ユーザー同席時に |

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
