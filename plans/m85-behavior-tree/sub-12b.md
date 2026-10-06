# sub-12b: C# タスクのフィールドと BT 窓のクラスのピッカー

- 依存: sub-12 (eec835e)
- 状態: OK (M85l2 としてコミット、ハッシュは台帳)
- 往復: 1

## やること
spec 2. #21 (2026-10-06 ユーザー回答「M85 で作る」)、spec 4.1.7 の C# 節、受け入れ条件 18。

1. **C# タスクの一覧の問い合わせ**: ManagedHost に「`[BtTask]` クラスの一覧 (FullName) と、クラスごとのフィールドの記述子 (名前・型・既定値)」を返す口を足す (`MyeManagedVTable` の末尾。EngineAPI の版は変えない、sub-12 VERDICT と同じ扱い)。
   - 対象のフィールド: public なインスタンスフィールドで、型が bool / int / float / string / MyeVec3 のもの。それ以外の型は一覧に出さない。
   - 既定値: インスタンスを 1 つ作って読む。Roslyn の再コンパイル (C# のリロード) で一覧を作り直す。
   - 呼べるのは C# が読み込まれているとき (`IsReady`) だけ。読み込まれていなければ空を返す (呼び出し側が文字入力へ戻る)。エディタの窓から毎フレーム呼ばない。リロードの世代が変わったときだけ取り直す。
2. **`.bt.json` の CsTask に `"fields"`**: CppTask の `taskFields` と同じ置き場所・同じ防波堤 (オブジェクトのみ、値は数・真偽・255 バイト以下の文字列・数の配列 (Vector は 3 要素)、32 エントリまで)。JSON の往復は BehaviorTreeLibrary で行う。
3. **実行時**: C# のインスタンスを作った直後 (OnStart の前) に、`fields` の値をそのインスタンスのフィールドへ書く。
   - 受け渡しは、レーンの enter 呼び出しに fields の JSON 文字列を渡す (C# 側で System.Text.Json で読む)。
   - 名前が無い・型が違う値は書かず、(クラス, フィールド) ごとに 1 回警告。
   - 決定論の保証外であることは変わらない (sub-12 の門・警告・Failure の規則はそのまま)。
4. **BT 窓**:
   - CsTask の `class` 欄を、一覧から選ぶコンボにする (CppTask のタスク名ピッカーと同じ見た目)。一覧に無い名前 (手入力済み・C# 未読み込み) は、そのまま表示して「見つからない」の印を付ける。C# が読み込まれていないときは文字入力に戻す。
   - 「C# task fields」欄を記述子から自動生成する (CppTask の欄と同じ流儀)。編集は `SetTaskField` 相当で Undo / Redo に乗り、1 操作 = 1 段 (sub-09 のジェスチャの規則)。
   - クラスを変えたら、新しいクラスに無いフィールドは消す (同じ 1 段)。
   - 検査: 記述子に無いフィールド名・型違いの値を警告として一覧に出す (保存は止めない)。
5. 文字列は en / ja。

## やらないこと (このサブでは)
- C# タスクの状態をスナップショット・リプレイに入れること (保証外のまま)
- MyeVec3 以外の構造体・配列・Entity 型のフィールド

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Script\ManagedHost.{h,cpp}`、`src\Engine\Engine\AI\BtManagedTaskLane.h`
- `src\Scripting\ScriptRuntime.cs`、`Bootstrap.cs`、`Interop.cs` (vtable のミラー)、`MyeScript.cs`
- `src\Engine\Engine\AI\BehaviorTreeLibrary.{h,cpp}` (CsTask の fields)、`BehaviorTreeSystem.cpp` (enter に fields を渡す)
- `src\Editor\Windows\AI\BehaviorTreeEditModel.{h,cpp}`、`BehaviorTreeWindow.{h,cpp}`、`src\Editor\SelfTest\BehaviorTreeEditorSelfTest.cpp`、`LocalizationTable.inl`
- 一覧の問い合わせを Editor から引く経路 (EngineContext など。Editor → Engine の依存方向を守る)

## 受け入れ条件 (このサブ)
1. (spec 18) モデル層: CsTask の fields の設定・往復 (保存 → 読み直し)・Undo / Redo、クラスの変更で無いフィールドが消えて 1 段で戻る — `BehaviorTreeEditorSelfTest`。
2. (spec 18) BehaviorTreeLibrary: CsTask の fields の JSON 往復と防波堤 — `BehaviorTreeSelfTest`。代役のレーン (StubLane) で、enter に fields が渡ることも確かめる。
3. (spec 18) 一時プローブ (C# の `[BtTask]` クラスに int / float / bool / string / MyeVec3 のフィールド) で、次の 3 つを確かめてログを SELF_EVAL に写す。プローブは消す。
   - BT 窓のピッカーでクラスを選べる。
   - フィールド欄の値が `.bt.json` に保存される。
   - Play で OnStart の時点にその値が入っている。
4. 画面: ピッカーを開いた状態と、フィールド欄の画像 (一時プローブ)。C# が読み込まれていないときの文字入力の画像 (`--no-managed` などで再現できれば。できなければ理由を書く)。
5. (spec 16) ビルド 0 警告 (managed を含む)、Editor `--selftest` 両構成・Server `--selftest` で新しい FAIL 0、`check_rules.ps1` 0、`replay_verify.bat` 全 PASS (C# レーンは被覆外だが、BT 節や BT の実行器を触ったときの不変の確認)。

## 検証コマンド
- `tools\build_managed.bat` Debug / Release、`msbuild MyEngine.sln` Debug / Release (`/p:MyeWarnAsError=true`)
- `bin\x64\Debug\Editor.exe --selftest` / `bin\x64\Release\Editor.exe --selftest` / `bin\x64\Debug\Server.exe --selftest`
- `tools\check_rules.ps1`、`tools\replay_verify.bat`、一時プローブの実行と `--screenshot` / 実マウス (ui.ps1)

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要点)
- 一覧: `MyeManagedVTable` 末尾に `BtTaskCatalog(buf, bufLen)` (JSON。終端 NUL 込みで書き、NUL を除く長さを返す)、`BtTask` に 6 引数目 `fieldsUtf8`。`ManagedHost::BtTaskClasses()` が `CompileScripts` 成功ごとの世代 (`ReloadGeneration`) でキャッシュ。Editor は `EngineContext::managedHost` から引く (新しい依存方向なし)。`BtManagedTaskClass` / `BtManagedTaskField` / `BtManagedFieldAccepts` は `BtManagedTaskLane.h`。
- fields: `BtKindHasTaskFields` (CppTask / CsTask)。`BehaviorTreeLibrary` の読み書きは CppTask と同じ防波堤。`BtManagedTaskLane::RunTask` に `fieldsJson` を足し、`VisitCsTask` が enter / tick で毎回渡す (C# 側は作り直しのときだけ使う = リロード後の作り直しにも値が入る)。
- C#: `ScriptRuntime.BtTaskCatalog` (public 非 readonly インスタンスの bool / int / float / string / MyeVec3、名前の昇順、既定値は 1 インスタンスから)、`ApplyBtFields` (インスタンス作成直後・OnStart の前。名前なし・型違いは (クラス, フィールド) ごとに 1 回警告)。
- エディタ: `SetCsTaskClass` (クラス変更とフィールド削除を 1 回の Touch)、`SetTaskField` を CsTask にも開放、`BindManagedTasks` + 検査 `CSharpField` (名前つき警告)。窓は `DrawCsTaskPicker` / `DrawCsTaskFields`。C# 未読み込み (`IsReady` が偽) は文字入力のまま、フィールド欄なし。文字列 4 本 (en / ja)。
- テスト: `BehaviorTreeSelfTest` に fields 13 項目、`BehaviorTreeEditorSelfTest` に節 13 (21 項目)。
- 一時プローブ (scratchpad の project。リポジトリには残さず、窓のコードに入れた自動入力は元へ戻した): ImGui へ合成入力を送って実ウィジェットを操作。ピッカーで `ProbeNs.ZzFields` を選択、count (Ctrl+クリックで 42)・speed (2.5)・alert (チェック)・label ("set")・offset.x (4) を編集 → 保存した `.bt.json` に `"fields": {"alert":true,"count":42,"label":"set","offset":[4,2,3],"speed":2.5}`。Undo 1 回で最後の label だけが戻った。Runtime.exe の Play で `[probe] ZzFields OnStart count=42 speed=2.5 alert=True label=set offset=(4,2,3)`。fields に ghost (名前なし) と count="x" (型違い) を入れると、WARN が各 1 回、count は既定の 7 のまま、speed=3 (整数値の数) は float に入った。
- 画像 (scratchpad): `r4.png.00050.png` (ピッカーを開いた状態)、`r4.png.00220.png` (フィールド欄)、`nm.png.00070.png` (MyeScripting.dll を一時的に退避して C# 未読み込みにした状態 = 文字入力)。
- 途中で見つかった不具合: BtTaskCatalog の 2 回目の呼び出しで終端 NUL 分の 1 バイトが欠けて JSON が壊れ、一覧が空になっていた (プローブで発見、ネイティブ側を length + 1 で渡すよう修正)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ 1〜5 を確認 (自動テスト、一時プローブの Play ログで OnStart 時に値、防波堤の警告、画像 3 枚、replay_verify 16 PASS)。実マウスの代わりに ImGui の合成入力で実ウィジェットを操作した判断を承認。NUL 欠落の不具合は実走で見つけて修正済み
