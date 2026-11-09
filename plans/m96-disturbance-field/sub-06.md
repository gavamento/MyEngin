# sub-06: アニメイベント `disturb` (斬撃で霧を散らす)

- 依存: sub-05 (波 W2。spec §6)
- 状態: OK (W2 の統合待ち)
- 往復: 1


## 並列の注意 (波 W2)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): `ClipEventKind` / `ControllerClipEvent` の末尾、`TickRunner.cpp` の配送の switch。デモは `AddFieldDemoSwing`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.1 E のアニメイベント、§2 #6。スクリプト無しで、攻撃モーションの振りの瞬間に霧へ注入できるようにする。

1. `ClipEventKind` に `Disturb` を末尾追加 (`AnimatorController.h:108-113`)。`ControllerClipEvent` に disturb 用の欄 (`radius` / `speed` / `shape` / `dir[3]` / `coneDeg` / `densityDelta` / `trailDepth` / `ripple`) を末尾追加
2. `.controller.json` の読み込み: `"kind":"disturb"` と各欄 (spec §4.1 E の名前: `radius` / `speed` / `shape` (`"point"|"radial"|"cone"`) / `dir` / `coneDeg` / `density` / `trail` / `ripple`)。不正な `shape` は既存のイベントの不正値と同じ扱い (エラーログ + そのイベントを捨てる)
3. 配送: `TickRunner.cpp:509-545` の switch に `case ClipEventKind::Disturb`。位置 = `ev.pos`、向き = `dir` をコントローラのエンティティの WorldMatrix (前 tick の確定値。`ev.pos` と同じ時点) の回転で回して正規化 → `DisturbanceQueueImpulse`。場の step (フェーズ 4) が同じ tick に消費する
4. エディタ: アニメイベントの編集 UI (M89o) に kind の選択肢があれば `disturb` と欄を足す。文字列は `Tr()` の両言語
5. デモ: `--field-demo` に骨アニメの人 (既存の `--anim-demo` が使っているモデルとクリップで、振りに当たるクリップがあればそれ。無ければ歩きのクリップの足に `disturb` を付けて「足元の霧を蹴る」にしてよい) を足す。どのモデル・クリップを使ったかを SELF_EVAL に書く

## やらないこと (このサブでは)

- 刀身の前 tick 位置から速度を出す (spec §2 #6 で却下)
- 既存の kind の挙動の変更

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\AnimatorController.h:108-134`、`AnimatorController.cpp` の clipEvents の読み込み (`:1188-1221` 付近、M95 の `{"fps":30,"events":[...]}` 形も通ること)
- `src\Engine\Engine\Loop\TickRunner.cpp:509-545`
- アニメイベント編集のエディタ窓 (`grep -rn "ClipEventKind::Noise" src\Editor`)
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 19, 30。

- 19: animatorcontroller / disturbance の selftest で、(a) JSON の全欄が読まれる (b) 不正な shape でイベントが捨てられる (c) 発火した tick の場の step で注入される (d) コントローラのエンティティを y 軸に 90° 回すと注入の向きも 90° 回る
- 12: replay_verify `field`・`anim` PASS
- 絵: 振り (または蹴り) の瞬間の 1 枚

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='animatorcontroller,animatorcontrolleredit,disturbance'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-06 (round 1)
- ClipEventKind::Disturb と ControllerClipEvent の末尾 8 欄 (disturb*) を追加。JSON は radius / speed / shape / dir / coneDeg / density / trail / ripple。不正な shape と半径 0 以下はイベントごと捨てる。
- 配送は TickRunner.cpp の BuildClipEventImpulse (新規) → DisturbanceQueueImpulse。向きは WorldMatrix の回転で回して正規化。
- エディタ (AnimationPreviewWindow) に disturb の kind と欄、Tr() 両言語 8 件。
- デモ: --field-demo に anim_test.glb + assets\anims\field_swing.controller.json (Attack の 22 tick、Hand.R、cone) を AddFieldDemoSwing で追加。
- selftest (RunDisturbanceAnimEventTests) は書いたが未実行。replay_verify も未実行 (運用)。
- 検証: Debug / Release ビルド OK、check_rules 0 error、スクショ撮影済み。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-09)。ClipEventKind::Disturb と欄・JSON の読み書き・エディタ欄・両言語の文字列・配送 (向きはコントローラの WorldMatrix の回転)・デモ (anim_test.glb の Attack、tick 21 に Hand.R から +X の Cone) を確認。radius <= 0 も捨てる追加、BuildClipEventImpulse の公開、専用の controller アセットを受理。switch の配送の経路そのものは replay `field` (W2 後のまとめ実行) が被覆する。帯が薄い件は sub-17 の目視へ
