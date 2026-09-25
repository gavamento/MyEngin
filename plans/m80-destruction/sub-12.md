# sub-12: ABI v22 (onBreak / ApplyFractureDamage)・デモ仕上げ・文書

- 依存: sub-11
- 状態: OK (コミット待ち)
- 往復: 1

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

SELF_EVAL: sub-12 (round 1)
実装:
  - `src/Shared/EngineAPI.h` — `MyeEngineApi` 末尾に `ApplyFractureDamage` スロット追加、`MYE_API_VERSION` を 22u へ、版履歴コメント追加
  - `src/Shared/ScriptTypes.h` — `MyeScriptDesc` 末尾に `onBreak` イベント追加
  - `src/Shared/ScriptAPI.h` — `mye_script_detail::GetBreakFn<T>()` (OnCollisionEnter 等と同じ `if constexpr` 検出パターン) を追加し `MakeDesc<T>` から `d.onBreak` を埋めるよう配線
  - `src/Engine/Engine/Script/EngineApiTable.cpp` — `out.ApplyFractureDamage` を sub-07 の内部関数 `mye::ApplyFractureDamage` (FractureSystem.h) へ橋渡し
  - `src/Engine/Engine/Script/ScriptHost.h/.cpp` — `ScriptType::onBreak` 関数ポインタ、`DispatchBreak(root, piece, point, impulse)`、`LoadModule` での配線と DLL 差し替え時のクリア
  - `src/Engine/Engine/Script/ManagedHost.h/.cpp` — `MyeManagedVTable::InvokeBreak` (末尾追加)、`DispatchBreak` (旧 MyeScripting.dll では no-op)
  - `src/Engine/Engine/FractureSystem.h/.cpp` — `Update`/`UpdateImpl` に `ScriptHost*`/`ManagedHost*` (既定 null) を追加、`FractureBreakEvent` と `FractureSystem::LastBreakEvents()` (観測用、非ハッシュ) を新設、`ProcessRoot` が新リーダーごとに荷重最大の破片の原点・荷重を計算してローカルに leader-index 昇順ソートしてから返す、`UpdateImpl` が jobs (Destructible ごと) を root `EntityID.index` 昇順に並べ替えてから処理・配信 (spec §4.1 破断 8 の「ルート index → 新リーダー index 昇順」を満たすための追加。既存の単一破壊物のシーンでは観測できない差分)
  - `src/Engine/Engine/TickRunner.cpp` — `fractureSystem.Update` 呼び出しに `&scriptHost, runManaged ? &managedHost : nullptr` を追加 (collisionSystem と同じ配線)
  - `src/Engine/Engine/DemoContent.cpp` — `BuildFractureShowcaseScene` の固定壁に GameLogic スクリプト `FractureDamageProbe` を付与 (デモ仕上げ)
  - `src/GameLogic/Scripts/FractureDamageProbe.cpp` (新規) — tick 5 に `ApplyFractureDamage` で自分 (壁) を割り、`OnBreak` で `brokenCount`/`lastImpulse` を登録フィールドへ書き戻す恒久 probe (SchemaHealthDemo / PartRaycastDemo と同じ流儀、replay 被覆の本体)
  - `src/Scripting/Interop.cs` — `MyeEngineApi` 末尾に `ApplyFractureDamage` delegate、`ManagedVTable` 末尾に `InvokeBreak` delegate、`Engine.ApplyFractureDamage` (internal 糖衣)
  - `src/Scripting/Bootstrap.cs` — `vt->InvokeBreak = &ScriptRuntime.NativeInvokeBreak`
  - `src/Scripting/ScriptRuntime.cs` — `InvokeBreak`/`NativeInvokeBreak` (InvokeCollision と同型)
  - `src/Scripting/MyeScript.cs` — `protected static void ApplyFractureDamage(...)` 糖衣、`public virtual void OnBreak(...)` (既定 no-op)
  - `tools/check_rules.ps1` — `$apiVersionSlots` に `22 = 126` を追加
  - `docs/history/api-scripting-tools.md` — v22 の経緯を追記
  - `docs/adr/ADR-021-fracture-destruction.md` (新規) — 事前分割 + 接着グラフ + root proxy の設計判断・却下案・拡張点・`ConvexColliderLibrary::Clear()` 契約を記録
  - `src/Engine/Engine/Physics/ConvexColliderLibrary.h` — `Clear()` 宣言に `ReregisterAll()` を対で呼ぶ契約を 1 行追記
  - `engine_spec.md` — §10.8 (破壊の節、新設) / §11.3 (replay 一覧に UI・fracture を追加、7→9 ペアへ訂正) / §12.2 (Physics 行に M80 追加、§12.3 の M61/M62 記述を訂正) / §13 (ADR-018〜021 を追加) を追記
  - `tools/replay_verify.bat` — `:job_fracture` のコメントを現状 (破断・スクリプトが実際に動く) に合わせて更新
  - `plans/m80-destruction/bench.md` — §7 に「`prof::GetRenderStats()` の中身」と「sub-11 の『45 で変化しない』の原因 (Editor.exe の headless capture は Play/シミュレーションを開始しないため、当時は一度も割れていなかった)」を追記。§2 の焼き時間表と同じ内容を `--fracture-bench` にも実装として反映 (下記)
  - `src/Engine/Engine/Physics/FractureBenchmark.cpp` — ボクセル化 (開いたメッシュ) の焼き時間計測セクションを追加 (`MakeBenchOpenBox`/`MakeBenchPlaneQuad`、解像度 32/48/64 × 開いた箱/平面、pieceCount=16、`--fracture-bench` の一部として実行)。従来 FractureSelfTest.cpp にあった同じ表をここへ移設
  - `src/Engine/Engine/Physics/FractureSelfTest.cpp` — (a) onBreak の 2 ブロック追加 (後述の「テスト」参照)。(b) sub-11 由来の軽量化: 開いたメッシュの解像度 48/64 バケの `pieceCount` を 16→4 に削減 (解像度 32 は 16 のまま)、時間計測専用で合否に数えなかった「解像度 64/128/256 の voxelize 単体タイミング」ループを削除 (`--fracture-bench` へ移設。正しさの被覆はそのまま: res32/48/64 いずれも `openMeshMode=1` の bake 成功 + 全破片が幾何的に閉じることを検証し続ける)
  - `src/Editor/PartSelfTest.cpp` / `src/Engine/Renderer/ComputeAbiSelfTest.cpp` — ABI v22 化に伴いハードコードされていた `MYE_API_VERSION == 21u` / `api.version == 21u` の期待値チェックを 22u へ更新 (Debug --selftest で FAIL していたのを発見・修正)

仕様との差分:
  - [追加] `FractureSystem::LastBreakEvents()` (観測用、非ハッシュのメンバ関数) を新設した。sub-12.md は明示していないが、onBreak の配信内容 (leader/point/impulse/順序) を実 DLL を介さずに --selftest で検算するための唯一の現実的な経路として追加した (理由は「不安・質問」参照)。sim 状態・WorldHash には一切影響しない (CollisionSystem の `LastCollisionEnter()` 等の既存の観測用 API と同じ位置づけ)
  - [追加] `UpdateImpl` で jobs (Destructible ごとの処理単位) を root `EntityID.index` 昇順にソートする処理を追加した。spec §4.1 破断 8 の「ルート index → 新リーダー index 昇順」を厳密に満たすために必要だが、sub-06〜11 の実装ではこの並べ替えが無かった (単一破壊物のシーンでは観測できない差分だったため気づかれていなかった)。既存 replay (`--fracture-demo` は同一 tick に複数 Destructible が同時に割れる構成ではない) には影響しないことを `replay_verify.bat` で確認した
  - [追加] `--fracture-bench` にボクセル化の焼き時間計測 (解像度 32/48/64 × 開いた箱/平面) を追加した。sub-11 の申し送り (やること 7) に明記されていた作業で、FractureSelfTest.cpp から移設した分の受け皿
  - [逸脱] 受け入れ条件 2 の「--selftest」経由での onBreak 到達検証は、実 DLL をロードする経路ではなく、ScriptAPI.h の糖衣とみ FractureSystem の計算内容の 2 点に限定した (理由・根拠は「不安・質問」参照)。実 DLL 経由の受信確認は `replay_verify.bat` の fracture job (Debug/Release 一致) で行った

検証:
  - `tools\gen_project_files.ps1` → GameLogic.vcxproj(.filters) に `FractureDamageProbe.cpp` を追加 (26 files)
  - MSBuild `MyEngine.sln` Debug|x64 / Release|x64 (フルビルド、複数回) → 0 error
  - `tools\check_rules.ps1` → `=== result: 0 error(s), 0 warning(s) ===` (規則 11 = ABI v22/126 スロット一致を含む)
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)。破壊系の節の合計は実測 132.0s (Fracture mesh core) + 0.27s (Fracture editor) + 0.98s (Fracture skin) ≈ **133 秒** (目標「Debug で 3 分以内」を達成。全体の selftest は 3m16s)
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS (exit 0)、45 秒
  - `tools\build_managed.bat Debug` / `Release` → 成功 (MyeScripting.dll 再ビルド)
  - C# レーンの実走確認 (一時プローブ、コミットせず revert 済み): `assets\scripts\FractureDamageProbeCs.cs` を追加し `FractureWall` へ一時的に付与、`bin\x64\Debug\Runtime.exe --fracture-demo --warp --no-audio --frames 10` で実測ログ `FractureDamageProbeCs firing ApplyFractureDamage at tick 5` → `FractureDamageProbeCs.OnBreak brokenCount=1 impulse=20000` を確認 (C++ probe と同時に壁を叩いたため impulse は両者の damage 合算値 20000)。確認後、プローブファイル削除・DemoContent.cpp/FractureDamageProbe.cpp の診断コード・EngineLoop.cpp の一時ログをすべて revert 済み (`git diff` で無変更を確認)
  - `tools\replay_verify.bat` → **PASS** (`[PASS] replay consistency (Debug/Release, 9 scenes: demo + parts + flow + mp + physics + joints + acoustic + ui + fracture) + snapshot round-trip + time travel + rule check`、14 jobs 並列 130.2s)。fracture job は ABI v22 のスクリプト (`FractureDamageProbe`) 込みで Debug/Release ビット一致
  - `tools\shot_verify.bat` → 32 枚中 4 枚 FAIL: **parts / joints / acoustic_forward / acoustic_deferred** (harness.md に記録済みの M80 着手前からの既知の乖離、今回の変更と無関係)。`fracture_before` / `fracture_after` は両方 **PASS** (`maxDiff=0`/`maxDiff=1`、既存ゴールデンのまま更新不要 — デモへスクリプトを足しても壁の最終静止絵は元のゴールデンと視覚的に一致した)
  - `--fracture-bench` (Release) → 新設のボクセル化焼き時間セクションが動作し、既存記録 (res32≈4.7-5.3s, res48≈10.8s, res64≈20s) と整合する実測値 (4.57s/10.57s/19.69s、open box) を確認
  - bench.md §7 の再調査 (一時プローブ、revert 済み): `Editor.exe --frames --screenshot` は Play を開始せず物理・スクリプトが一切走らないため sub-11 の「45 で変化しない」が観測された (`broken=0/3` のまま) ことを特定。`shot_verify.bat` が実際に使う `Runtime.exe` で撮り直すと `draws` は `broken` 数と連動して 4→26→30→45 と変化し、root proxy の可視性ゲートが正しく効いていることを確認 (割れる前に破片が描かれている兆候は無し。停止条件には該当しない)
自己採点 (1-5):
  仕様適合: 4 — ABI v22 の全要素 (スロット・イベント・版履歴・check_rules 表・C++/C# 両糖衣) を実装し、`replay_verify` の fracture job (実 DLL、Debug/Release 一致) で実際に届くことを確認した。onBreak の配信順序 (ルート index → リーダー index 昇順) も実装・テスト済み。受け入れ条件 2 の文言どおりの「--selftest で実 DLL 経由の onBreak 到達」だけは未検証 (理由は不安・質問参照)
  正しさ: 4 — `replay_verify` (Debug/Release 一致)・`check_rules` (ABI ミラー一致)・`--selftest` 全 PASS (Debug/Release 両方) を確認。C# レーンも一時プローブで実走確認済み (ログ証拠あり)。bench.md の「ドローコール数が変化しない」という既存記録の原因を追跡し、計測ツールの選択ミス (Editor.exe) であって root proxy の不具合ではないことを実測で切り分けた
  コード品質: 4 — 既存の規約 (コメント日本語、CollisionSystem::Update と同型の ScriptHost*/ManagedHost* 引数、既存のヘルパ複製の流儀) に従った。FractureSystem の break-event 収集はローカルでソートしてから確定させる形にし、決定論を壊さないようにした
  テスト: 4 — FractureSelfTest.cpp に 2 ブロック追加 (ScriptAPI 糖衣のマクロレベル検証 [16h]、FractureSystem::LastBreakEvents の内容・順序検証 [16i])。SelfTest 軽量化 (pieceCount 16→4、焼き時間表を `--fracture-bench` へ移設) で目標の 3 分以内を実測で達成し、退行が無いことを Debug/Release 両方の全 PASS で確認した
不安・質問:
  - 受け入れ条件 2 の「--selftest」文言について: `ScriptHost::DispatchBreak` が実際に GameLogic.dll を `LoadModule` して呼ぶ経路は、このリポジトリに「実 DLL をロードする --selftest テスト」のインフラが無く (既存の `OnCollisionEnter`/`OnTriggerEnter` 等の全イベントも同様に --selftest では未検証。`DllReloaderSelfTest.cpp` は失敗ケースしか実 DLL を使わない)、--selftest では (a) `ScriptAPI.h` の `GetBreakFn<T>` 糖衣が `T::OnBreak` へ正しく引数を転送すること、(b) `FractureSystem::LastBreakEvents()` が leader/point/impulse/順序を仕様どおり計算すること、の 2 点のみを検証した。実 DLL 経由の受信は `tools\replay_verify.bat` の fracture job (実際の GameLogic.dll を読み込む Editor/Runtime.exe で、Debug/Release のビット一致を含めて実行) で検証した。実行経路・実 DLL・両ビルド構成という点では --selftest より強い証拠だと考えているが、文言上「--selftest」ではない。この解釈でよいか、それとも実 DLL ロードの新規テストインフラ (例: テスト専用の小さい GameLogic 相当 DLL を別途ビルドする仕組み) を別サブで作るべきか、planner の判断を仰ぎたい
  - 既存の外部プロジェクト (三校/HAL Collector 等) が ABI v21 向けにビルド済みの GameLogic.dll を持っている場合、v22 のエンジンへの読み込み時にバージョン不一致でロード拒否される (既存の `LoadModule` の版検証、`ScriptHost.cpp` の apiVersion チェック)。外部プロジェクト側の再ビルドが必要になる。外部プロジェクトには一切手を入れていない
  - sub-11 由来の「[ユーザーに聞ける] の回答 (Notion) で裁定が覆っていたら反映する」件: 司会から新しい回答の通知を受け取っていないため、Q-1〜Q-14 はすべて仮決定のまま実装した
触ったファイル:
  - src/Shared/EngineAPI.h
  - src/Shared/ScriptTypes.h
  - src/Shared/ScriptAPI.h
  - src/Engine/Engine/Script/EngineApiTable.cpp
  - src/Engine/Engine/Script/ScriptHost.h
  - src/Engine/Engine/Script/ScriptHost.cpp
  - src/Engine/Engine/Script/ManagedHost.h
  - src/Engine/Engine/Script/ManagedHost.cpp
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/TickRunner.cpp
  - src/Engine/Engine/DemoContent.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/Physics/FractureBenchmark.cpp
  - src/Engine/Engine/Physics/ConvexColliderLibrary.h
  - src/Editor/PartSelfTest.cpp
  - src/Engine/Renderer/ComputeAbiSelfTest.cpp
  - src/GameLogic/Scripts/FractureDamageProbe.cpp (新規)
  - src/Scripting/Interop.cs
  - src/Scripting/Bootstrap.cs
  - src/Scripting/ScriptRuntime.cs
  - src/Scripting/MyeScript.cs
  - tools/check_rules.ps1
  - tools/replay_verify.bat
  - docs/history/api-scripting-tools.md
  - docs/adr/ADR-021-fracture-destruction.md (新規)
  - engine_spec.md
  - plans/m80-destruction/bench.md
  - build/GameLogic.vcxproj (gen_project_files.ps1 による再生成、新規ファイル登録のため)
  - build/GameLogic.vcxproj.filters (同上)
申し送り:
  - レビュー時は「不安・質問」の 2 点 (--selftest の解釈、外部プロジェクトへの影響) を優先的に見てほしい
  - `shot_verify.bat` の既知 4 枚 (parts/joints/acoustic_forward/acoustic_deferred) は今回も harness.md の記録どおり (M80 着手前からの乖離、修正しない)
  - WIP (`WaterPass.cpp`、`.agents/`、`SKILL.md`) には触れていない (`git diff` で無変更を確認済み)
  - 一時プローブ (C# の `.cs` ファイル、`DemoContent.cpp`/`FractureDamageProbe.cpp` の診断ログ、`EngineLoop.cpp` の一時ドローコールプローブ) はすべて revert 済みでコミット対象に含まれない
  - これが M80 の最後のサブ。ABI は v22 / 126 スロットで確定。plans/m80-destruction 配下の未決事項・後回し項目は ADR-021 と engine_spec.md §10.8 に転記済み

## フィードバック履歴
- round 1: VERDICT OK (planner)。ABI v22 (126)、onBreak / ApplyFractureDamage が C++ (fracture の replay job で実 DLL 経由) と C# (一時プローブの実走) で届き、check_rules の規則 11 が PASS。文書 (engine_spec §10.8、ADR-021、replay 一覧の訂正)、Debug の selftest の軽量化 (3 分 16 秒)、ドローコールの観測の説明 (計測ツールの選択ミス、root proxy は正しい) も完了。受け入れ条件 2 の逸脱 (SelfTest は糖衣と計算、実 DLL は replay job) は採用。ユーザー回答の反映は sub-15 へ
