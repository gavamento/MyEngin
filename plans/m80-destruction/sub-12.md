# sub-12: ABI v22 (onBreak / ApplyFractureDamage)・デモ仕上げ・文書

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## やること

M80 の ABI 変更を 1 回だけ入れ、文書を締める。

1. **イベント `onBreak`**: `MyeScriptDesc` (`src/Shared/ScriptTypes.h:49-67`) の**末尾**に `void (*onBreak)(void* state, MyeUpdateContext* ctx, MyeEntityId piece, MyeVec3 point, float impulse);`。発行は FractureSystem の分離確定時 (spec §4.1 破断 8): ルートにあるスクリプト (C++ と C#) へ、ルート index → 新リーダー index 昇順。`point` = その塊で荷重最大の破片の原点 (ワールド)、`impulse` = その荷重 [N]。C++ は `ScriptHost` に Dispatch を足し (`ScriptHost.cpp:81-129` の DispatchCollision と同じ形、`ScriptHost.cpp:251-270` の desc 写し)、`ScriptAPI.h:187-189` の `Get...Fn<T>` の流儀で GameLogic 側の糖衣を足す。C# は `ManagedHost` → `Bootstrap.cs` の vtable → `ScriptRuntime.cs` → `MyeScript.OnBreak`
2. **スロット `ApplyFractureDamage(void* engine, MyeEntityId entity, MyeVec3 point, float radius, float amount)`**: `struct MyeEngineApi` の末尾、`EngineApiTable.cpp` で sub-07 の内部関数を呼ぶ、`Interop.cs` の位置ミラー、C# の糖衣は `MyeScript` 側 (`Engine` は internal — メモの ABI 検証レシピ)
3. **版**: `MYE_API_VERSION 22u`、ヘッダの版履歴に 1 行、`tools\check_rules.ps1:676` の表に `22 = 126`、`docs/history/api-scripting-tools.md` に v22 の項
4. **デモ仕上げ**: `--fracture-demo` にスクリプト (C++ の GameLogic か C# のどちらかが既存デモの流儀に合う方。可能なら両方の経路を SelfTest で) を載せ、決まった tick に `ApplyFractureDamage` で壁を割り、`onBreak` を受けて何か状態に残る変化 (カウンタのフィールド等) を起こす。replay 一致
5. **文書**: `engine_spec.md` に破壊の節 (§10.8 想定。方式・データ・荷重モデル・6 挙動・決定論・存在ゲート・非目標・v1 の制限: 凸包 1 個の近似 / スキンの継ぎ目 / 非一様スケール / 骨の速度)、§11.3 の replay 一覧に fracture、§12 の milestone 表。**ADR-021** (`docs/adr/`): 事前分割 + 接着グラフ + root proxy、`.mfrac` を assets に置く理由、ECS 内に状態を置く理由、B (実行中の分割) への拡張点 (`BuildFracturePieces` を破断直前に呼ぶ形)、「`ConvexColliderLibrary::Clear()` を本番経路で呼ぶなら `FractureLibrary::ReregisterAll()` を対で呼ぶ」契約 (sub-03 の申し送り。`ConvexColliderLibrary.h` の `Clear()` 宣言コメントにも 1 行)、却下案。§13 の ADR 一覧
7. **(sub-11 からの申し送り) Debug の `--selftest` を軽くする**: 破壊系の SelfTest (特にボクセル化の焼き: 解像度 32/48/64 × 開いた箱 / 平面、焼き時間の表) が Debug で約 10 分かかる。**時間の計測 (表を作るための焼き) は `--fracture-bench` (Release の計測ツール) へ移す**。SelfTest には正しさの被覆だけを残し、入力を小さくする (例: 解像度 48/64 の開いた箱は pieceCount 4 にして、薄い壁の輪郭の接触を踏むことだけを確かめる)。目標: Debug で破壊系の節の合計が 3 分以内。被覆を落とす項目があれば SELF_EVAL に列挙する。`_DEBUG` / `NDEBUG` でテストを出し分けることはしない
9. **(sub-11 の観測の説明)** bench.md §7 で、`--fracture-demo` のドローコール数が tick 11 から 45 で一定のまま、割れる前後で変わらなかった。root proxy が効いていれば、割れた後に破片 (と `_cap`) のぶん増えるはず。`prof::GetRenderStats()` が何を数えているか (パス合算か、上限で丸めているか) を確かめ、説明を bench.md に 1 段落で追記する。割れる前に破片が描かれている (root proxy が効いていない) と分かったら、止めて planner に返す
8. engine_spec の破壊の節に、bench.md の上限 (1 Destructible あたり推奨 64 破片以下) と、**複数が同じ tick に割れると予算を超える** (8 個 × 32 破片で 16.4 ms) ことを書く
6. `[ユーザーに聞ける]` の回答 (Notion) で裁定が覆っていたら、その反映が済んでいるか確認 (司会から届く)

## やらないこと (このサブでは)

- ABI の他の変更。状態 getter の専用スロット (汎用 GetComponentField で足りる裁定)

## 触る場所 (planner の見立て)

- `src/Shared/EngineAPI.h`、`src/Shared/ScriptTypes.h`、`src/Shared/ScriptAPI.h`
- `src/Engine/Engine/Script/EngineApiTable.cpp`、`ScriptHost.*`、`ManagedHost.*`
- `src/Scripting/Interop.cs`、`Bootstrap.cs`、`ScriptRuntime.cs`、`MyeScript.cs`
- `tools/check_rules.ps1`、`docs/history/api-scripting-tools.md`
- `FractureSystem.cpp` (発行)
- `engine_spec.md`、`docs/adr/ADR-021-*.md`、`README.md` の機能一覧 (あれば)
- 三校など外部プロジェクトの GameLogic DLL は ABI 版の不一致で読み込みを拒否される (既存の版検証)。その旨を完了報告に書く (外部プロジェクトは触らない)

## 受け入れ条件 (このサブ)

1. `tools\check_rules.ps1` 規則 11 PASS (v22 = 126、C++ と C# の順序・名前・引数個数一致、全スロット代入) — 実行ログ
2. SelfTest: C++ スクリプトで `onBreak` が期待の引数で 1 回ずつ届く。`ApplyFractureDamage` で割れる — `--selftest`
3. C# レーン: `tools\build_managed.bat Debug/Release` の後、C# スクリプトで `onBreak` が届き `ApplyFractureDamage` が効く (一時プローブで実走確認でよい。プローブはコミットしない) — ログ
4. `--fracture-demo` の replay 一致 (スクリプト込み) — `replay_verify.bat`
5. 既存 replay / golden 全 PASS (ABI 変更で既存スクリプトの挙動が変わらない) — `replay_verify.bat`、`shot_verify.bat`
6. 文書: spec の節・ADR-021・replay 一覧・版履歴の差分があり、リンク先が実在する
7. WIP 不変

## 検証コマンド

```
tools\gen_project_files.ps1
tools\build_managed.bat Debug
tools\build_managed.bat Release
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

コミット件名に ABI 変更を明記する (AGENTS §8)。

## 実装メモ (coder が追記)

## フィードバック履歴
