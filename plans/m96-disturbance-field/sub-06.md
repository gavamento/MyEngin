# sub-06: アニメイベント `disturb` (斬撃で霧を散らす)

- 依存: sub-05
- 状態: 未着手
- 往復: 0

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

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-16 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='animatorcontroller,animatorcontrolleredit,disturbance'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
