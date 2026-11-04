# sub-01: ヒットストップ — Animator の tick 単位停止 (ABI v32、snapshot v66)

- 依存: sub-00 (Wave 1。sub-02 / sub-04 / sub-06 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 作業の最初と最後に worktree と `C:\HAL\MyEngin` の両方で `git status` を確認し、SELF_EVAL に貼る
- 共有資源は spec 付録 A の予約表どおり。`Components.h` / `Components.cpp` / 各 SelfTest / `LocalizationTable.inl` への追記は sub-00 が入れた **自分のマーカー (`// ---- M95 sub-01 ...`) の直下**にだけ書く。他サブのマーカーや他の struct に書かない
- 同じ波の他サブ (sub-02 / sub-04 / sub-06) のファイルを「ついでに」触らない。必要なら「不安・質問」へ

## やること

spec §4.1 A。`AnimatorControllerComponent` に `pauseTicks` を足し、`Update` で停止を実装し、ABI v32 の最初の 2 スロット
(`AnimatorSetPause` / `AnimatorGetPause`) を開ける。ABI 版の宣言 (`MYE_API_VERSION 32`、版コメント、`PartSelfTest` の版チェック) は
このサブで行い、sub-03 は同じ v32 にスロットを足すだけにする。

## やらないこと (このサブでは)

- 速度倍率、SkinningSystem の変更、CC の重力停止
- sub-03 の部位スロット (順は付録 A.1 で固定済み。sub-01 は 189 / 190 の 2 本だけ)

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h:782-828` `AnimatorControllerComponent` のマーカー直下に `int32_t pauseTicks = 0` (hash 対象のまま)
- `src\Engine\Core\Ecs\Components.cpp:374-411` 反射登録のマーカー直下に `pauseTicks` (読み取り専用、`MYE_JP`)
- `src\Engine\Engine\Animation\AnimatorController.cpp:1688-1735` `Update`: 非アクティブ分岐 (`:1709-1722`) の直後に停止分岐。凍結の手は非アクティブ分岐と同じ (関数に切り出して共有してよい)。`rootMotionVelocity` / `rootMotionDeltaRotation` を 0 / 恒等にし、`applyRootMotion` なら `ApplyRootMotion` (`:834-880`) を 0 で呼ぶ (ヨーは呼ばない)。`--pauseTicks; continue;`。**1. 遷移チェック以降のブロックは触らない** (sub-04 / sub-05 の領域)
- `src\Engine\Engine\Replay\SimSnapshot.h:175` `kSimSnapshotVersion = 66` + 版コメント (付録 A.3)
- `src\Shared\EngineAPI.h:58` `MYE_API_VERSION 32`、版コメント (`:57` の次に v32 の行。5 本全部の名前を書いてよい)、`MyeEngineApi` 末尾 (`TilemapGetBounds` の次) に 2 本
- `src\Engine\Engine\Script\EngineApiTable.cpp:1839` `AnimatorPlay` の隣に実装
- `src\Shared\ScriptAPI.h` `MyeAnimatorPlay` の隣に `MyeAnimatorSetPause` / `MyeAnimatorGetPause`
- `src\Scripting\Interop.cs:498` の次に `// ---- v32 (M95): アクション戦闘 ----` と関数ポインタ 2 本 (位置ミラー) と静的糖衣 (`:1682` の隣)、`src\Scripting\MyeScript.cs:229` の隣に糖衣
- `src\Editor\SelfTest\PartSelfTest.cpp:178-185` 版を 32u にし、`// ---- M95 sub-01 (ABI) ----` の直下に「v32 の 2 本が filled in」と受け入れ 4 の断言 (sub-03 が 5 本に増やす)
- `src\Engine\Engine\Animation\AnimatorControllerSelfTest.cpp` の `// ---- M95 sub-01 ----` の直下に受け入れ 1〜3。骨クリップ付きのヘルパ `MakeNamedClipModel` (`:63-76`) と M89j のルートモーションテスト (`:1380-1430` 付近) を参考に
- GameLogic 側のサンプル (`src\GameLogic`) は触らない (ABI 版が上がるので再ビルドは必要)

## 受け入れ条件 (このサブ)

spec §5 の 1, 2, 3, 4, 5, 6。
- 1〜3: 「停止 → n tick 進める → 再開」の断言。停止をまたぐイベント位置が再開後に 1 回だけ発火すること
- 4: `PartSelfTest` の ABI 様式で `AnimatorSetPause` (上書き / 負 / 無し) と `AnimatorGetPause` (-1)、`MYE_API_VERSION == 32u`
- 5: RV 全 PASS、三校 verify.bat PASS (最後の round)
- 6: `Interop.cs` と `EngineAPI.h` の関数ポインタ順の機械照合 (memory `abi-bump-verification` のレシピ。無ければ両ファイルから名前列を抜いて diff) の出力を SELF_EVAL に貼る

## 検証コマンド

- ビルド Debug / Release (worktree の `MyEngine.sln`。GameLogic.dll も通ること)
- 往復中: `$env:MYE_SELFTEST_ONLY='animatorcontroller,part,simsnapshot'; bin\x64\Debug\Editor.exe --selftest` (PowerShell ツール。終わったら環境変数を消す)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`cmd /c C:\HAL\三校\tools\verify.bat` (`MYE_ENGINE` を worktree のパスに)

## 実装メモ (coder が追記)

## フィードバック履歴
