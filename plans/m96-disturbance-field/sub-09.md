# sub-09: 追従モード (整数セルずらし)

- 依存: sub-08
- 状態: 未着手
- 往復: 0

## やること

spec §4.1 F、§2 #11。プレイヤーの周りだけに格子を持ち、広いマップでも霧・草・跡が効くようにする。

1. `DisturbanceFieldComponent` の末尾に `followTarget` (EntityRef) と `followMargin` (int32、既定 4、範囲 1〜格子の半分) を足す。snapshot は着手時の版 + 1
2. step の 2 番目 (spec §4.1 B): 相手が生きていれば、相手の位置をセルに丸めた格子中心と今の中心の差 (整数セル、軸ごと) のどれかが `followMargin` 以上の step で、`originCell` をその差だけずらす。GroundLayer は xz だけ (baseY は場のエンティティの y のまま)、Volume は 3 軸
3. ずらしは全配列で「重なる部分を写す・入ってきたマスは静止状態・出たマスは捨てる」。波紋は h と hPrev の両方。写しは前の配列を読んで新しい配列へ書く (自分の場所で上書きして順序に依存しない)
4. 描画: CB の原点が tick ごとに変わるだけ (テクスチャは毎回全域転送なので追加の作業は無いはず。確認する)
5. デモ: 2 個目の 2D 層 (プレイヤー追従、跡あり) をデモの人に付ける。固定の霧の外を歩いても足跡が付く配置

## やらないこと (このサブでは)

- 1 マス未満のなめらかなずらし (整数セルだけ。半セルの補間はしない)
- 相手が消えたときの特別な挙動 (その場で止まる = 固定と同じ)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Disturbance\` の step
- `src\Engine\Core\Ecs\Components.h` / `.cpp` の末尾欄
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 21, 30。

- 21: 相手を +x へ `followMargin` セル動かした step で `originCell.x` が同じだけ増え、ずらす前に (x, z) にあった値が (x − k, z) にビット一致で移り、+x 端の k 列が静止状態。`followMargin − 1` では動かない。追従ありの 2 ワールド 600 tick 一致、snapshot 往復 100 tick 一致、jobs あり / なし一致
- 12: replay_verify `field` PASS
- 絵: 固定の霧の外で、追従の層に付いた足跡が写った 1 枚

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-16 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
