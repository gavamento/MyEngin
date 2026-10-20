# sub-09: CharacterController の `plane2D` (一方通行・移動床・剛体押し・報告)

- 依存: sub-01
- 状態: OK (commit 02e4022)
- 往復: 2
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
2. (spec 19) `--nav-demo` / `--bt-demo` / `--acoustic-demo` の hash-diff が append のみ (ログ)。replay_verify 全体は sub-10。
3. (spec 1-3) 0 警告、check_rules 0、Physics suite が `MYE_SELFTEST_ONLY=m94` で PASS、snapshot +1 のみ。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --nav-demo / --bt-demo / --acoustic-demo --replay-record` → `--hash-diff`
チェックポイントではない (重い 3 本は sub-10)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-09 (round 1)
実装:
  - Components.h/.cpp: CharacterControllerComponent 末尾へ 7 欄 (plane2D / ignoreOneWay / pushPower / wallContact / ticksSinceGrounded / groundEntity / groundVelocity) と kMaxTicksSinceGrounded。read-only 4 欄は kFieldReadOnly、全部 hash 対象。Inspector 注記は Components.h コメントと tooltip。
  - SimSnapshot.h: kSimSnapshotVersion 54 -> 55。AcousticAudioSelfTest.cpp の版照合も 55。
  - PhysicsSystem.cpp SolveCharacters: p2 = plane2D の分岐。z 固定 (押し出しの nz を捨てる)、一方通行 (CharPassesThroughOneWay / CharRejectsOneWayNormal。TryCharStepUp にも ow を渡す)、移動床 (前 tick の isGrounded かつ groundVelocity x dt を変位に足す)、剛体押し、報告 (wallContact / ticksSinceGrounded / groundEntity / groundVelocity)。plane2D OFF は p2 の分岐を踏まないだけ。
  - PhysicsSelfTest.cpp: CC2D 節 (坂 30 / 60、段差 0.3、一方通行 5 項目、移動床 2、押し 2、壁 4、z 不変と ticksSinceGrounded と OFF 対照 3、決定論 + 焼き込み 2)。ハッシュ 7E99492E7EE44DC1。
仕様との差分:
  - [追加] 移動床に乗っている間、cc->velocity.x は「床に対する速度」(床の変位を引く)。歩行アニメ / 足音が床の速度で歩き出さないため。spec は velocity の定義に触れていない。
  - [追加] 移動床の groundVelocity は SolveCharacters 時点の Rigidbody.velocity (kinematic は tick 開始時のまま、dynamic は解決後 = 次 tick 開始時の値)。groundVelocity.y も変位に足す。
  - [追加] wallContact は「押し出し法線 |nx| > 0.9 の接触が今 tick にあった」= 壁に接している間は押し付けていなくても立つ (離れると 0)。spec の「押し出し法線の |nx| > 0.9」の素直な読み。
  - [追加] 剛体押しの向き条件: |nx| > 0.5 かつ nx x moveInput.x < 0 (進行方向へ押している接触)。インパルス = pushPower x moveInput.x (水平変位 / dt の意図値。ブロックされた実変位ではない)。1 tick に 1 剛体 1 回、最大 8 体。押した剛体は起こす (isSleeping/sleepTicks をリセット)。
  - [追加] 一方通行の上向きでない法線 (ny <= 0) の接触は捨てる (剛体側 RejectOneWayContact と同じ規約)。上昇中 (velocity.y > 0) は全部すり抜け。
  - [未実装] 親付き (frame が identity でない) plane2D CC の z ビット不変は未検証 (親フレームの往復変換で z がずれ得る)。
検証:
  - Debug / Release `/p:MyeWarnAsError=true` ビルド -> 0 警告・exit 0 (Debug は初回 SelfTestUnused C4100 の既知癖で 1 回落ち、同コマンド再実行で通る)
  - `MYE_SELFTEST_ONLY=m94` Editor.exe --selftest (Debug / Release) -> Physics ALL PASS (CC2D 節の全項目)、他の suite も PASS。CC2D scene hash @900 = 7E99492E7EE44DC1 が Debug == Release。plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 は不変 (焼いた値と一致)。
  - `pwsh tools\check_rules.ps1` -> 3 error / 50 warning (着手前と同数、全て M94 範囲外の既存)
  - hash-diff (--nav-demo / --bt-demo / --acoustic-demo、Debug、300 tick、tick 299 のダンプ): 基点 (6 ファイルを一時的に HEAD へ戻して作った exe) と比較。新 7 欄の行を除くと leaf の差 0。残る差は CC ごとの rollup 行だけ (nav 6 / bt 3 / acoustic 4 = CC の個数と一致)。新欄は全部既定値 (00 / 0 / FFFFFFFF00000000)。ダンプは cache\m94\s09\ (git 管理外)。作業ツリーは patch 照合で完全復元済み。
  - 未実行: replay_verify / 全体 selftest / shot_verify (ユーザー判断 9。sub-10)
自己採点 (1-5):
  仕様適合: 4 — 4.1.6 の 7 欄・移動床・一方通行・押し・報告・9 項目を実装。差分は上の [追加] 5 件。
  正しさ: 4 — Debug/Release 同値ハッシュ、既存 CC テスト群と 3 デモの leaf 差 0 で OFF の不変を確認。親付き CC と複数押し・2 側同時 wallContact(=2) は未検証。
  コード品質: 4 — 分岐は p2 に閉じ、一方通行判定は 2 関数に集約。SolveCharacters が更に長くなった (報告部を小関数に割る余地)。
  テスト: 4 — spec 9 項目を網羅。wallContact = 2 と剛体押しの複数対象は無し。
不安・質問:
  - wallContact を「接触している間」にするか「押し付けている間」にするかは spec が曖昧。壁ジャンプ / 壁滑りには接触式が便利なので前者にした。
  - 移動床 groundVelocity に dynamic 剛体の解決後 velocity を使うのは tick 開始時でなく tick 末尾の値。spec 文言 (tick 開始時) と厳密には異なる (kinematic は同値)。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Core\Ecs\Components.h
  - C:\HAL\MyEngin\src\Engine\Core\Ecs\Components.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Replay\SimSnapshot.h
  - C:\HAL\MyEngin\src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp
  - C:\HAL\MyEngin\plans\m94-2d\sub-09.md
申し送り:
  - 床は kinematic Rigidbody の velocity を設定し、Transform は同じ量だけスクリプトが動かす (物理は kinematic を積分しない)。engine_spec (sub-24) に書く。
  - 親付き plane2D CC の z 不変は未検証 (sub-24 の限界欄)。sub-20 (Effector2D) / sub-21 (GridAgent2D) は groundEntity / ticksSinceGrounded を読める。
  - sub-11 のタイル箱は StaticShape 経由で bodies に入るので、CC の一方通行 / 接地プローブ / groundEntity (owner = Tilemap エンティティ) にそのまま効く (oneWay は Collider の値を継承)。タイル用 selftest で CC を載せて確認すること。

SELF_EVAL: sub-09 (round 2)
実装:
  - #1: PhysicsSystem.cpp の Body に kinDrive (kinematic かつ rb->plane2D、休止ラグドールは除く) を追加。位置積分帯で pose.px/py += v*h、rotation を angularVelocity.z で積分 (z・X/Y 回転は動かさない。収集時に vz/wx/wy を落とし姿勢を Z 回転へ正規化)。書き戻しは kinDrive も位置・回転を書く (rb の velocity は利用者の入力なので書き換えない)。plane2D OFF の kinematic は従来どおり積分しない。
  - #1 テスト: CC2D 節の床を kinematic + plane2D にして Transform を触らない。「床が 60 tick で 1.0 動く (z 不変)」「CC が床の上に留まる (相対 x ±0.01、velocity は床に対する値)」の 2 項目に直し、OFF の kinematic (Still) が動かない対照を 1 項目追加。決定論シーンの Lift も velocity だけで動かす。焼きハッシュを再採取: C04B7FFA3E31E5D0 (Debug == Release)。
  - #2 #3 (nit): 今回は直さない (申し送り)。
仕様との差分:
  - [追加] kinematic + plane2D は pz を固定、質量中心オフセットは無視 (kinematic は持たない)。
検証:
  - Debug / Release 0 警告ビルド (exit 0)。MYE_SELFTEST_ONLY=m94 両構成 Physics ALL PASS、他 suite も PASS。plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 不変、CC2D C04B7FFA3E31E5D0 が両構成一致。
  - check_rules 3 error / 50 warning (増減なし)。
  - hash-diff (nav / bt / acoustic) を再実行: 新 7 欄の行を除くと leaf 差 0、rollup 差は CC の個数分のみ (round 1 と同じ総ハッシュ値)。
  - 未実行: replay_verify / 全体 selftest / shot_verify (sub-10)。
自己採点 (1-5):
  仕様適合: 4 / 正しさ: 4 (親付き kinematic plane2D・回転する床に乗る CC は未検証) / コード品質: 4 / テスト: 4
不安・質問: なし
触ったファイル: PhysicsSystem.cpp / PhysicsSelfTest.cpp (round 1 の一覧に加えて変更なし)
申し送り:
  - 回転する kinematic 床 (angularVelocity.z) に乗る CC は groundVelocity が並進のみなので運ばれない (v1 の限界。sub-24 の限界欄へ)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-20)。must 1 件: kinematic + plane2D の剛体を物理が velocity で積分する (spec 4.1.6 に追記)。理由: 現状は「velocity を設定し、Transform は同じ量だけスクリプトが動かす」二重管理で、spec の「床は kinematic Rigidbody の velocity で動かす」の意図 (velocity だけで動く) を満たさない。plane2D OFF の kinematic は従来どおり。[追加] 5 件 (床に対する velocity.x、groundVelocity の時点、wallContact = 接触中、押しの条件と 8 体上限、ny ≤ 0 を捨てる) は採用し spec に反映。その他 (CC2D 節 PASS、焼いたハッシュ 7E99492E7EE44DC1 両構成同値、3 デモの leaf 差 0) は根拠として確認済み。
- round 2: VERDICT OK (planner、2026-10-20)。must 1 を `Body.kinDrive` (kinematic かつ plane2D、休止ラグドール除く) で実装: 収集 (:1853)・位置積分 (:5162)・書き戻し (:5377/5406) の 3 箇所に閉じ、OFF の kinematic は従来どおり。CC2D 節に床の自走 / 相対位置 / OFF 対照の 3 項目、焼いたハッシュ C04B7FFA3E31E5D0 が両構成同値、plane2D / Shapes2D の焼いた値は不変、3 デモの hash-diff は round 1 と同じ総ハッシュ (kinematic 積分が OFF シーンに触れていない証拠)。回転床と親付きは R9 (e) に記録。
