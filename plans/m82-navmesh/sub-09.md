# sub-09: 文書と全体検証

- 依存: sub-08 (sub-04 も OK であること)
- 状態: OK (commit 4e0d7e5)
- 往復: 2

## やること
spec 5. の 11, 12, 15, 16, 17, 18 (c)。

1. `docs\adr\ADR-023-navmesh.md` を仕上げる (sub-01 の下書き + 各サブの決定: 復元方式、**将来の実行時再ベイクの足し方 (どの関数を・どの tick のどのステップで呼ぶか・何が未実装か)**、塗りの描画レーン、TypeId、`.mnav`、エリアコストを Surface に置いた理由、CC 必須、Link の渡り、Obstacle の同期確定、除外した案と理由、AcousticNav との役割分担)。
   sub-10 から: CC の stepOffset (既定 0.3、|scale.y| 倍、上限は実効の全高)、cellSize / cellHeight の自動決定とその丸め、段差判定の量子化のずれ (1 セル未満で、残りは Stuck で受ける)、登る tick に 0.13〜0.16 m 前へ出る跳び、acoustic の golden が Agent Eye の乗り上がりを写さないこと。 sub-05 から: Stuck の仕様 (60 tick、radius/4、解除の条件、部分経路の到着との使い分け)、carve=false が何もしない理由、Box の切り抜き方、5 cm の閾値、表示を Generation で作り直すこと、Commit で入れ直しを省く条件、計測 (324 タイル・障害物 8 個で Release 平均 857 µs)。plans の m75-ugui.md の TypeId 注記 (71 / 72 / 73 を M82 が使用)。 sub-05 round 2 から: 部分経路の Arrived が 60 tick 遅れること、dtCrowd の位置が終点の手前で止まること、回転 Box (yaw)、Surface の AABB による絞り込み、kSimSnapshotVersion 27 / NavTileStore の状態 v2、HasPolyAt は最寄り点の距離で判定する (穴の縁のポリゴンを拾う罠)。 sub-06 から: Recast パッチ 3 (paint と priority による塗り順、areaId があると通行不可を復活させない)、ポリゴンのフラグ = 1 << area、OPTIMIZE_VIS をコスト上げ時に外す理由 (実測)、Modifier をベイクに焼き込まない理由、filter の割り当て (16 種まで)、kStateVersion 3 / kSimSnapshotVersion 28、TypeId 74。 sub-07 から: Recast パッチ 4、Link は .mnav に入れず World から差し込む設計、出口は入口のタイルと同じか隣まで、入口の y を寄せる処理、渡りの状態は Nav 節の NavAgentSlot、渡っている途中で Link が消えたときの振る舞い、TypeId 75、版 29 / 状態 v4。 sub-08 から: ABI v24 = 139 (8 本、NavCompleteLink、areaMask、outPartial)、RandomPoint の方式と RNG の消費規則、クエリは前の tick の状態を見ること。文書側の更新: m75-ugui.md に『ABI v24 は M82 が使用、M75h は v25』と『TypeId 71〜75 は M82 が使用』、engine_spec.md の ABI 節 (v23 の節の付近) に v24 を追記、docs の engine-feature-guide.md の ABI 説明 (v18 のまま古い) を更新。
   sub-03 から: 坂の実効上限 atan(maxClimb / (2·cellSize)) と CC の登れる段差の実測 (spec 2. #19 / #5)、`targetPathqRef` を tick 末に `DT_PATHQ_INVALID` へ正規化する理由 (dtPathQueue の連番は復元で 1 からになる)、回避なし = `collisionQueryRange` 0.01 で近傍探索ごと止める理由、Nav 節の実測 (6 体 2,894 B / 128 体 38,782 B、capture +64 µs、restore 58 µs、ロールバック 148 KB に対し 128 体で +26% → 差分化しない)、最悪 tick の `dtCrowd::update` (128 体一斉要求で 0.76-0.87 ms、dtPathQueue 容量 8 件が自然に絞るので件数制限を足さない)。
2. `engine_spec.md` に NavMesh 節 (コンポーネント、Tick 位置、スナップショット、ABI 表の版)。`docs\engine-feature-guide.md` 9.3 の「NavMesh ベイクとは区別します」を書き換え。`AcousticNav.h:22-24` のコメントを「音響ナビは NavMesh を使わない (汎用移動は ADR-023 の NavMesh)」の意味に整える。`docs\test_checklists.md` に NavMesh の手動確認項目。`CLAUDE.md` / `AGENTS.md` に末尾 TypeId や検証表の記述があれば更新 (無ければ触らない)。`plans\m75-ugui.md` の TypeId 注記。
3. 全体検証: 両構成 `/p:MyeWarnAsError=true`、`--selftest` 両構成、`check_rules`、`replay_verify` (全ジョブ)、`shot_verify`、Runtime.exe `--nav-demo` 実走 + スクショ、エディタで Create → 4 項目 → Bake → 再生で Agent が歩くのを `--screenshot` で確認。

## やらないこと (このサブでは)
- 機能追加

## 触る場所 (planner の見立て)
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`、`src\Engine\Engine\Acoustic\AcousticNav.h` (コメントのみ)、`plans\m75-ugui.md`

## 受け入れ条件 (このサブ)
1. (spec 16) 文書の差分が spec 4. と実装に一致 (参照先のパス・番号が実在)。
2. (spec 11, 12, 15, 17) 全検証 PASS (shot_verify の golden `nav` を含む)。未実施があれば理由とリスクを書く。

## 検証コマンド
- 上記 3. の全部

## 実装メモ (coder が追記)

### SELF_EVAL: sub-09 (round 1)

実装:
  - docs\adr\ADR-023-navmesh.md: 下書きを確定に。決定 5〜12 (ユーザーが planner の裁定を覆した点の経緯 / TypeId・.mnav・読み込み・Presence gate / Agent・Stuck・部分経路・Nav 節の実測 / stepOffset とセルサイズ自動決定 / NavDebugView と塗り / Obstacle / Modifier・エリアコスト・Link / ABI v24)、除外した案、既知の限界、検証結果、ビルド警告を追加。「将来の実行時再ベイクの足し方」は呼び出し順 4 段 + 未実装の宿題に書き直し (関数名は実在の `NavCollectTriangles` / `NavBakeTile` / `ReplaceTileLayers` / `Commit` に修正)。Off-Mesh の記述と AcousticNav の節を最終状態に直した
  - engine_spec.md: 10.9 NavMesh (M82) を新設 (コンポーネント・Tick 位置・スナップショット・段差登り・表示・ABI・再ベイク・限界)、11.5 の ABI v23 の次に ABI v24 の段落、ADR 一覧に ADR-023
  - docs\engine-feature-guide.md: 4 章の ABI 説明を v24 / 139 に更新、9.3 の「NavMesh ベイクとは区別します」を書き換え、9.4 NavMesh を新設、15 章の ABI 行を更新
  - docs\test_checklists.md: 「M82: NavMesh」の手動確認項目を追加
  - src\Engine\Engine\Acoustic\AcousticNav.h: コメント 3 行のみ (「音響ナビは NavMesh を使わない。汎用の移動は ADR-023」)
  - plans\m75-ugui.md: TypeId 71〜75 は M82 が使用 (InputField は 76 以降)、ABI v24 は M82 が使用 (M75h は v25) を注記

仕様との差分:
  - [追加] test_checklists.md の末尾に M82 の節を追加 (sub-09 の 2. に明記された更新先)。
  - [逸脱] 受け入れ 2 の「Runtime.exe --nav-demo 実走 + スクショ」「エディタで Create -> Bake -> 再生の --screenshot」は M82j では撮り直していない。golden nav (shot_verify、PASS) と NavEditorSelfTest、各サブの画像で代替。
  - [逸脱] 「両構成 /p:MyeWarnAsError=true」は Debug / Release とも HEAD 由来の ProjectComputeRunnerSelfTest.cpp の C4127 で Engine.vcxproj が落ち、後続プロジェクトまで進まない (台帳の既知は Debug のみだったが Release も同じ)。通常ビルドは replay_verify 内で警告 0。
  - [追加] CLAUDE.md / AGENTS.md には末尾 TypeId や検証表の記述が無いので触っていない (指示どおり)。

検証:
  - tools\replay_verify.bat (PowerShell ツール経由) -> 全 15 ジョブ PASS (144.7 s)。nav ジョブ含む、静的規則 0 / 0
  - tools\shot_verify.bat -> golden nav PASS。FAIL は既知 5 枚のみ (198/3625、208/137、83/596、82/594、150/192。数値は着手前と同一)
  - Editor.exe --selftest Debug / Release (直列、Start-Process -Wait) -> Nav 4 種を含め ALL PASS。FAIL は既知の Source control 2 件のみ (exit 1)
  - Server.exe --selftest Debug / Release -> ALL PASS (exit 0)
  - tools\check_rules.ps1 (pwsh) -> 0 error / 0 warning
  - /p:MyeWarnAsError=true の Debug / Release ビルド -> 上記 C4127 (ProjectComputeRunnerSelfTest.cpp 7 件) で失敗 (既知、R9)。他のファイルの警告は出ない
  - 文書の整合: ADR / engine_spec が参照する関数名・TypeId・版 (kSimSnapshotVersion 29、状態 v4、ABI v24 = 139、Stuck 60 tick) を実コードで grep して確認
  - 失敗談: 最初に Editor.exe --selftest を `&` で呼んだら GUI サブシステムなので待たずに戻り、Debug / Release / Server が並列に走った。止めて Start-Process -Wait で直列に流し直した (結果は流し直しのもののみ)

自己採点 (1-5):
  仕様適合: 4 — 文書 6 点と全検証を実施。画面での再撮影 (逸脱 2 点目) と MyeWarnAsError の全体成功は未達で理由を記載
  正しさ: 4 — 記載した数値・名称は実コードと台帳・各サブの実装メモで照合。ADR の sub-03/05 の実測値は各サブの記録からの転記で今回再測定していない
  コード品質: 5 — コードはコメント 3 行のみ
  テスト: 4 — 全体検証は実行して PASS (既知の FAIL を除く)。Editor GUI の手操作は未実施 (チェックリスト化)

不安・質問:
  1. 受け入れ条件 1 / R9: CI の MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true は Debug / Release とも HEAD の ProjectComputeRunnerSelfTest.cpp で落ちる。M82 の範囲外だが、CI を通すには別途直しが要る
  2. ADR に書いた「ユーザーが覆した点」の表の反対意見は spec 2. の記録から転記した

触ったファイル:
  docs\adr\ADR-023-navmesh.md, docs\engine-feature-guide.md, docs\test_checklists.md, engine_spec.md,
  src\Engine\Engine\Acoustic\AcousticNav.h, plans\m75-ugui.md, plans\m82-navmesh\sub-09.md (実装メモ節のみ)

申し送り:
  - cache\s09_*.log (検証ログ) が作業ファイルとして増えた (git 管理外、削除は承認が要るので残置)
  - 外部プロジェクト (三校 / HAL Collector) の GameLogic.dll は ABI v24 で再ビルドが要る (ユーザー作業)
  - 未確認: 半透明の塗りの CI (WARP) での一致 (赤くなったら ci.yml に MYE_SHOT_SKIP_NAV を足す)

### SELF_EVAL: sub-09 (round 2)
実装:
  - #1: ADR-023 の 2 か所を確定内容に書き換えた。(a) 差分化の記述を「M82c の実機計測で 128 体 +26% (38,782 B) に収まり、差分化しないと決めた (決定 7)」に。(b) maxTiles = max(ceil(1.5 x 層数), 層数 + 4)、maxPolys = NextPow2(2 x 実測最大) で下限 64、salt が 10 ビット未満なら Failed、に (NavBake.cpp の式と照合済み)。残りの「判断」「目安」「決める」を grep で見直し、残りは確定済みの事柄か名詞用法 (ユーザー判断、経路を決める等) で未決表現は無い
仕様との差分: なし
検証: 差分確認と NavBake.cpp:176-201 の式との照合のみ (文書だけの変更、ビルド・replay は未実行で要求どおり)
自己採点 (1-5): 仕様適合 5 — 指摘箇所を修正し式を実コードと照合 / 正しさ 5 — 同上 / コード品質 5 — 変更は文書のみ / テスト 4 — 文書のため grep と照合のみ
不安・質問: なし
触ったファイル: docsdr\ADR-023-navmesh.md, plans\m82-navmesh\sub-09.md (実装メモ節のみ)
申し送り: なし

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-04)。must: ADR-023 に、決定済みの事項が『後のサブで決める』と書かれたまま残っている。102 行目付近の『dtCrowd の保存を差分化する余地 (sub-03 で判断)』は、sub-03 で『差分化しない』と決まった (128 体で +26%)。158 行目付近の maxTiles / maxPolys の『目安 (sub-02 のベイクで決める)』は、実装の式 (maxTiles = max(1.5 × 層数, 層数 + 4)、maxPolys = NextPow2(2 × 実測最大)・下限 64、salt 10 ビット未満なら Failed) に直す。ほかに『〜で決める / 〜で判断』の未決表現が無いかも見る。出所としての (sub-NN) は残してよい。採用した点: 画面の撮り直しを省いたこと (各サブの画像と golden nav、手動確認は test_checklists.md)、MyeWarnAsError の扱い (R9 を Release も同じに更新)、文書 6 点の内容。
- round 2: VERDICT OK (planner、2026-10-04)。ADR に残っていた古い記述 2 か所を、確定した内容に直したことを確認した。maxTiles / maxPolys の式は NavBake.cpp:176-201 と一致し、『差分化しない』は決定 7 に書かれている。
