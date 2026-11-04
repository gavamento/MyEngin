# sub-02: CC 同士の押し出しと接触イベント (snapshot v67、ADR-034)

- 依存: sub-00 (Wave 1。sub-01 / sub-04 / sub-06 と並列。sub-05 より先)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 作業の最初と最後に worktree と `C:\HAL\MyEngin` の両方で `git status` を確認し、SELF_EVAL に貼る
- 共有資源は spec 付録 A の予約表どおり。`Components.h` / `Components.cpp` / `PhysicsSelfTest.cpp` への追記は sub-00 が入れた **自分のマーカー (`// ---- M95 sub-02 ...`) の直下**にだけ書く (CC の sub-05 のマーカーはその下に残す)
- `SimSnapshot.h` は自分の版 67 を書く。統合で sub-01 (66) と競合するのは想定どおり (付録 A.3)
- 同じ波の他サブのファイル (`AnimatorController.*`、`EngineAPI.h` など) を触らない

## やること

spec §4.1 D。`CharacterControllerComponent` に `collideCharacters` / `pushPriority` / `reportContacts` を足し、
`SolveCharacters` に CC 同士の押し出し (相手の tick 頭のカプセル、優先度) と接触の記録を入れ、`SolidContact` 列へ合流させる。
ADR-034 を書き、dogfooding 17 を解決にする。

## やらないこと (このサブでは)

- 運動量の交換、CC が剛体を押す応答 (plane2D の `pushPower` は既存のまま)
- 配送側 (`CollisionSystem` / `ScriptHost`) の変更 — 合流した `SolidContact` が既存経路でそのまま届くことを確認するだけ
- `verticalInput` (sub-05)

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h:859-883` `CharacterControllerComponent` の `// ---- M95 sub-02 (末尾 append) ----` 直下に `bool collideCharacters = false; int32_t pushPriority = 0; bool reportContacts = false;` (hash 対象のまま)
- `src\Engine\Core\Ecs\Components.cpp:431-460` 反射登録のマーカー直下に 3 欄 (`MYE_FIELD_TIP` で英語の説明、`MYE_JP` で日本語名)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp`
  - `:1354-1364` `CharBody` — 必要なら記録用の一時欄 (無くてもよい。関数ローカルで足りる)
  - `:1516-1720` `SolveCharacters(bodies, chars, dt)` → `SolveCharacters(bodies, chars, dt, std::vector<SolidContact>* outContacts)`。
    固定パスの `bodies` 走査 (`:1565-1607`) の後に CC 同士の走査 (spec D の条件)。接触の記録は `Collide` が真になった箇所 (`bodies` 側 `:1577` と CC 側) で `reportContacts` を見て積む。
    `collideCharacters` / `reportContacts` の CC が 1 体も無いときは追加の走査・確保に入らない (関数冒頭で `chars` を 1 周して旗を立てる)。**`vy` の初期化行 (`:1526`) は触らない** (sub-05 の領域)
  - `:5772` 呼び出し元に `outContacts` を渡す。合流 (ソート → unique → 既存 key を除いて差し込み → 全体を key 昇順) は `SolveCharacters` の末尾か呼び出し元の直後。記録が 0 件なら `outContacts` に触らない
  - `SolidContact` の法線規約は `PhysicsSystem.h:20-36` (大 index → 小 index)、代表点は `shapes::ClosestPointOnShape` (`Shapes.h:117`)
- `src\Engine\Engine\Physics\Rigid\CollisionSystem.cpp:118-135` `FindContactNormal` が key 昇順前提 — 合流後も昇順であることを selftest で断言
- `src\Engine\Engine\Replay\SimSnapshot.h:175` v67 + 版コメント (付録 A.3)
- `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp` の `// ---- M95 sub-02 ----` 直下に 7〜11。既存の CC テスト (`:877-970`) と、`CollisionSystem` を直接回すテスト (`:511` / `:552` / `:1207`) の様式に合わせる
- `docs\adr\ADR-034-character-contacts.md` (ADR-033 の体裁: 状態 / 出所 / 背景 / 決定 / 却下した案 / 影響)
- `docs\dogfooding.md:43` の表と `:528-548` の本文に「修正済み (M95)」と何で直したか
- `src\GameLogic` のサンプル (`VehicleDriver` の回避コード) は触らない

## 受け入れ条件 (このサブ)

spec §5 の 7, 8, 9, 10, 11, 12, 13。
- 7〜9: CC 2 体 (`collideCharacters`、`pushPriority` の組み合わせ) を数 tick 歩かせ、位置と重なり深さを断言
- 10: `reportContacts` の CC と 床 / kinematic 剛体 / 動的剛体 / 別の CC の 4 ケースで `SolidContact` の key・法線の向き・impulse 0 と、`CollisionSystem` 経由の enter → stay → exit の回数を断言
- 11: CC + ソリッド capsule Collider と動的剛体で `SolidContact` が同 key 1 件 (impulse > 0)
- 12: RV 全 PASS、三校 verify.bat PASS (最後の round)
- 13: ADR-034 と dogfooding 17 の更新がコミットに含まれる

## 検証コマンド

- ビルド Debug / Release (worktree)
- 往復中: `$env:MYE_SELFTEST_ONLY='physics,simsnapshot'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`cmd /c C:\HAL\三校\tools\verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
