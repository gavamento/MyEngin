# sub-09: CharacterController の `plane2D` (一方通行・移動床・剛体押し・報告)

- 依存: sub-01
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #5, #6, #9、4.1.6

## やること
1. `CharacterControllerComponent` 末尾に spec 4.1.6 の 7 フィールドを append (read-only は `kFieldReadOnly`、全部 hash 対象。`groundEntity` は `FieldType::EntityRef`)。`kSimSnapshotVersion` +1。
2. `SolveCharacters` (`PhysicsSystem.cpp:1375-1465`) に `plane2D` 分岐:
   - `moveInput.z` を 0 として扱い、z を tick 開始時の値に固定 (変位・押し出しの z 成分を捨てる)。
   - 一方通行: 押し出し (:1397-1423) と接地プローブ (:1425-1441) で `oneWay` 形状は「CC の tick 開始時の最下点 ≥ 天面 − slop、かつ `velocity.y <= 0`、かつ `!ignoreOneWay`」のときだけ有効。
   - 移動床: 接地した形状の所有エンティティを `groundEntity` に、その Rigidbody (kinematic / dynamic) の `velocity` を `groundVelocity` に書き、接地中は変位に `groundVelocity × dt` を足す (床の速度は tick 開始時の値)。
   - 剛体押し: `pushPower > 0` で、押し出し相手が非 kinematic の plane2D 剛体なら、その剛体の `velocity.x += pushPower × (CC の水平変位 / dt) の符号 × |…|` 相当のインパルス (質量で割らない。単純則、Inspector 注記)。剛体は次 tick の物理で動く (1 tick 遅れ、既存の「CC は tick 頭の位置で静的に見える」と同じ)。
   - 報告: `wallContact` (押し出し法線の |nx| > 0.9 で ±)、`ticksSinceGrounded` (接地で 0、離地で +1、32767 で飽和)。
   - `plane2D` OFF の CC は 1 演算も増やさない。
3. Inspector 注記 (和名の説明 or Components.h コメント): 「床は kinematic Rigidbody の velocity で動かす」「異なる z の物は当たらない」。
4. PhysicsSelfTest に `CC2D` 節 (spec 4.1.6 の 9 項目) + Debug / Release のハッシュ焼き込み。
5. `--nav-demo` / `--bt-demo` / `--acoustic-demo` (CC を使う既存デモ) の hash-diff が append のみ。

## やらないこと (このサブでは)
- 箱 CC (spec 2. #6)、はしご (スクリプト側で `gravityScale = 0` + moveInput.y 相当は 2D では無い → spec スコープ外。必要なら未決事項へ)、コヨーテタイム (ゲーム側)

## 触る場所 (planner の見立て)
- `Components.h:841-853`、`Components.cpp` (CC 登録)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp:1264-1465` (SolveCharacters / TryCharStepUp / 定数)、:1785-1832 (CC 収集)
- `PhysicsSelfTest.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`
- 参考: `plans\m82-navmesh\spec.md` 2. #21 (stepOffset に scale を掛ける規約。2D でも同じ)

## 受け入れ条件 (このサブ)
1. (spec 19) `CC2D` 節 9 項目が Debug / Release で PASS、焼いたハッシュ同値。
2. (spec 19) `nav` / `bt` / `acoustic` の replay_verify PASS、hash-diff が append のみ (ログ)。
3. (spec 1-5) golden 不変、0 警告、check_rules 0、snapshot +1 のみ。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`pwsh tools\check_rules.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
