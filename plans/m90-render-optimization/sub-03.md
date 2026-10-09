# sub-03: オクルージョンを Forward へ広げ、デバッグ表示と統計を仕上げる

- 依存: sub-02
- 状態: OK (commit 24781ec)
- 往復: 2

## やること
spec §4.1.4、§4.3。
1. sub-02 の 2 フェーズを Forward の不透明に通す (共通部は sub-02 のパスを使い回す。経路ごとに重複実装しない)。
2. `--hzb-debug` に max-Z ピラミッドの表示と「オクルージョンで落とした物の AABB」(EditorLinePass 等) を足す。既存の min-Z 表示は残す。
3. ProfilerWindow のオクルージョン欄を仕上げる (フェーズ別、ビュー別)。
4. (sub-02 VERDICT から) `OcclusionCuller` を Deferred / Forward の両方から使える位置へ切り出す (経路ごとに複製しない)。`forward_lit_instanced` にも remap (`remapPlus1`) を通す。`--hzb-debug` のために `ViewState::pyramid` を読む口を足す。
5. (sub-02 VERDICT から) 統計の意味を固定する: `drawCalls` / `triangles` は **CPU が提出した論理数** (ON/OFF・構成間で不変の基準値) のままにする。GPU が間引いた効果は `occlusionPhase1Draws / occlusionPhase2Draws / occluded` で見る。ProfilerWindow ではこの 2 種類を並べ、「実際に描いたインスタンス数 = phase1 + phase2」を 1 行で出す (2 フレーム遅れと明記)。
6. (sub-02 VERDICT から) エディタの実機で Scene View + Game View (viewKey 2 / 3) を同時に開き、両方でオクルージョンが効いて欠けないことを確かめる (sub-02 は viewKey 1 の Runtime だけで確認した)。

## やらないこと (このサブでは)
- 影・半透明へのオクルージョン。

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Pipeline\ForwardPath.cpp`、sub-02 のパス
- `--hzb-debug` の実装箇所 (Deferred のデバッグ表示)
- ProfilerWindow

## 受け入れ条件 (このサブ)
1. Forward 経路の `render_bench` で occluded > 0、ON/OFF の画素差 0。 — `--screenshot` A/B + img-diff
2. `--hzb-debug` のスクショで max-Z と落とした物が見える。 — スクショのパスを報告 (目視はユーザー)
3. golden 全 PASS、`tools\check_rules.ps1` PASS。
4. エディタの Scene View と Game View で、両ビューのオクルージョン統計が別々に出て、ON/OFF で絵が変わらない (エディタのスクショか、ビュー別の dump で確認)。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、A/B スクショ

## 実装メモ (coder が追記)

### round 1 (前セッションの途中差分を引き継いで完了)
- 引き継ぎ時点で済み: やること 1 (ForwardPath の 2 フェーズ、DrawUnit/PlanItems/DrawUnits)、4 (OcclusionCuller の共有化、forward_lit_instanced の remap、DebugPyramid)、HzbDebugPass の切り出し、`--hzb-debug-max`、AABB の読み戻し (CollectOccludedBoxes)。
- 今回足した: 落とした物の AABB を RenderSystem の線パスで描く (赤、onTop)、ProfilerWindow の実描画インスタンス数行 (phase1 + phase2、2 フレーム遅れ明記) とビュー別オクルージョン行、Prof_OcclusionDrawn / Prof_ViewOcclusion、ForwardPath.h の改行崩れ修正、tools\gen_project_files.ps1 (pwsh) で HzbDebugPass を build/Engine.vcxproj(.filters) へ。
- 検証: Debug/Release ビルド OK、check_rules 0 error、selftest Debug/Release exit 0 (ServerNetSelfTest の一過性 FAIL は今回出ず)、Forward render_bench の ON/OFF diffPixels=0 (Release/Debug、カット直後 frame 40/42 も 0、occluded 2782 / カットで phase2 2207)、shot_verify 30 枚 PASS、--hzb-debug --hzb-debug-max のスクショ (Forward/Deferred、scratchpad の h_fwd_1.png / h_def3.png) で max-Z + AABB を確認、Editor で Scene(2)+Game(3) を同時表示 (一時 imgui.ini、復元済み) して view 2 / view 3 の統計が別々、ペイン内の画素 ON/OFF 差なし (差はタイミング文字のみ)。

### round 2 (FIX_REQUEST #1〜#3)
- #1: OcclusionSelfTest.cpp の TestDeferredAB / TestStats を `template <class Path>` の TestPathAB / TestStats にし、Deferred と Forward で使い回した。Forward 側に一時ディレクトリの OccProbe.surface (固定色) 材質の項目を 1 個混ぜ、判定対象外 (フェーズ 1 で常に描く) と restoreForwardLitBindings の remap 張り直しを通す。ForwardPath のテスト口 3 個が呼ばれるようになった。5 フレーム ON/OFF 一致・カット直後・通番の飛び・作成失敗の注入・隠れた物 3 / フェーズ 1 = 1 が Forward でも PASS (Release の selftest ログで確認)。
- #2: EngineLoop.h (occlusionCulling / hzbDebug)、RenderTypes.h (occlusionEnabled)、RenderSystem.cpp / RenderSystem.h のコメントを Forward 対応に直した。
- #3: ProfilerWindow.cpp のビュー別行の条件を hasOcclusion に分けて折り返した。
- 質問 (b): Runtime が `--screenshot` だけで終了しないのは M90 以前から。3b30251 の EngineLoop.cpp は --screenshot / --shot-frame の保存後に終了する処理を持たない (終了は --frames か --render-stats-dump が担う)。コードを読んで確認しただけで、旧版の実行はしていない。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-09)
  1. [must] ForwardPath の `InjectOcclusionFailureForTest` / `OcclusionDisabled` / `OcclusionStatsForTest` が未使用 (呼び出しは DeferredPath 版だけ = `OcclusionSelfTest.cpp:499-559`)。AGENTS §5 の「未使用コードを追加しない」に反し、spec §4.4 の失敗の局所化 (受け入れ 15) と ON/OFF 一致が Forward では回帰テストに無い。`OcclusionSelfTest.cpp` の `TestDeferredAB` / `TestStats` を経路で使い回せる形にし (経路ごとに複製しない)、ForwardPath でも「5 フレームの ON/OFF 全画素一致・カメラカット直後・通番の飛び・作成失敗の注入」と「隠れた物 3 / フェーズ 1 = 1」を確かめる。Forward 固有の確認として、サーフェスマテリアルの項目 (判定対象外 = フェーズ 1 で常に描く) を 1 個混ぜて ON/OFF 一致を見る。
  2. [should] オクルージョンが Deferred 専用だと書いたままのコメントを直す: `EngineLoop.h` の `occlusionCulling` (「Deferred の不透明のみ」)、`RenderTypes.h` の `RenderView::occlusionEnabled` (「Deferred の不透明だけが読む」)、`RenderSystem.cpp` の `view.occlusionEnabled` 代入の行 (「Deferred の不透明のみ」)、`EngineLoop.h` の `hzbDebug` (「Deferred パスのみ効く」→ max-Z 表示は Forward も)。
  3. [nit] `ProfilerWindow.cpp` のビュー別オクルージョン行の if が 1 行に長い。折り返す。
  - 不安・質問への回答: (a) Editor の A/B をペイン内の差分で判定したのは妥当 (受け入れ 4 は「ビュー別の dump」でも可と書いてあり、dump 側で両ビューの統計を確認済み)。(b) Runtime が `--screenshot` 単独で終了しない件は本サブの範囲外。M90 以前からの挙動かを確かめ (直前コミット `72ff316` 以前の Runtime で 1 回)、以前からなら申し送り、M90 由来なら次の SELF_EVAL に書く。
- round 2: VERDICT OK (planner、2026-10-09)。#1 は OcclusionSelfTest をテンプレート化して Deferred / Forward で共有し、Forward にはサーフェス項目を混ぜた A/B・カット・通番の飛び・失敗注入・統計を入れた (Debug / Release PASS)。#2 #3 解消。(b) の --screenshot 単体で終了しない件は M90 より前からの挙動 (コード読みのみ) なので申し送りへ回す。nit (申し送り): サーフェス項目が実際に描かれていることは ON/OFF の一致から間接的にしか確かめていない (プローブ色の画素検査は無い)。selftest が %TEMP%\mye_occlusion_selftest を終了後も残す (次回の開始時に消している)。
