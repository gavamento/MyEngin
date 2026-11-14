# sub-16: ライブのシーンの復元 (M104q)

- 依存: sub-15
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。Phase 5 の最後のサブ。

## やること

restore §3.3、§4.2。

sub-01 の試作 4 の結果 (2026-11-14 の見直し。restore §4.2 が正): C# コンポーネントのフィールドは `FieldFromJson` では書けない (ECS の中身は managed のハンドルだけで、呼べばハンドルを壊す)。**C# コンポーネントの Field は別の分岐にし、`ManagedHost::DeserializeComponent` へ戻したいフィールド 1 つだけの JSON を渡す** (managed の `ApplyJson` は JSON にあるキーだけを書く。`C:\HAL\MyEngin\src\Scripting\ScriptRuntime.cs` 655 行目)。Entity / Component の単位は `ApplyPartial` がそのまま通る。
この分岐は実走していない。次の 3 つをこのサブのテストで確かめ、1 つでも成り立たなければ、C# コンポーネントは Entity 単位の復元だけにして、その理由を restore §4.2 に書く: (a) 1 キーの JSON で他のフィールドが変わらない (b) `UndoStack::Record` 経由の復元が Undo で戻る (c) プレハブの override の記録が C# コンポーネントで保たれる。C# レーンは replay の被覆外なので、スイートで実走させる。

1. 対象が今エディタで開いているシーンならライブの `Scene` に照合して適用する。開いていなければディスク上の JSON に照合し、sub-15 のファイルの経路で書く。
2. 照合 (restore §3.3 の表): Field / Component (追加・削除) / Entity (追加・削除) / Prefab Override。値は JSON の字句で比べる。`documentId` は必ず照合する。`EntityRef` は `fileId` で比べ、指す先がいなければ依存の欠落として止める。
3. 適用は必ず `UndoStack::Record(label, scene, selection, fids, structural, edit)` の中で行う (Ctrl+Z で戻る。`CaptureAfter` がプレハブの override 記録を取り直す。**この経路を通さずに値を書くと override の記録が取りこぼされ、静かにデータが失われる**)。
   - Entity: `SceneSerializer::ApplyPartial`。Field: `FieldFromJson`。Component の追加・削除を戻すときは `StructuralChanges::Apply` を指定する。
4. ゲート: restore §2 の表のシーンの列 (Playing と NetActive で止める。ビルド中やマージ中は止めない)。
5. シーンの復元はディスクを書かない。シーンは dirty になる。ジャーナルには対象と状態だけ (内容は持たない)。
6. `PrefabPropagation` は戻す対象にしない。原因の変更へ案内する。
7. 字句の比較のために、ライブの値を JSON の字句へ直す経路が要る。`SaveToJson` / `EntityToJson` は採番と構造変更の適用を伴う (arch §1) ので、**編集モードのセーフポイントでだけ**呼ぶ。Play 中はゲートで止まるので呼ばれない。数値の字句がシリアライザの出力と記録で一致する前提を、固定の値の表で確かめる。
8. Editor Snapshot の送信 (`editor_snapshot` / `blob_put`) が要るなら、このサブで足す (見直しで確定)。

## やらないこと (このサブでは)

- 開いていないシーンのエンティティをメモリへ読んで戻すこと (ファイルの経路で書く)。
- Undo で戻されたかどうかの記録 (restore §4.2: 記録しない)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\src\Editor\ChangeTracking\RestoreManager.*`、`SceneRestore.*`、`AiChangesWindow.*`
- 参考: `C:\HAL\MyEngin\src\Editor\Undo\UndoStack.h` (113 行目の `Record`)、`C:\HAL\MyEngin\src\Engine\Engine\Scene\SceneSerializer.h`、`C:\HAL\MyEngin\src\Engine\Core\Util\JsonUtil.h`

## 受け入れ条件 (このサブ)

1. Field を 1 つ戻すと値が Before になり、Undo で After に戻り、Redo で Before になる → `changetracking` スイート (要求 §16.4 の 2 番目)。
2. 今の値が After と違う Field は競合になり、World が変わらない (`HashWorld` が同じ) → スイート。
3. 別のシーンを開いているとき、同じ `fileId` のエンティティに当たらない → スイート。
4. Component の追加を取り消す / 削除を取り消す、Entity の追加を取り消す / 削除を取り消す。親がいない、`EntityRef` の先がいない場合は止まる → スイート。Undo でそれぞれ戻る (何も戻らない Undo エントリができない)。
5. プレハブのインスタンスの override 済みフィールドを戻した後、`overrides` の記録が正しい (取りこぼしが無い)。保存して読み直しても同じ → スイート。
6. Play 中とネットセッション中は止まる。ビルド中は止まらない → スイート。
7. C# コンポーネントの Field 復元について、上の (a) (b) (c) をそれぞれ確かめるテストがある。成り立たなかった場合は Entity 単位だけに落とし、Field の復元を選ぶと理由が表示される → スイート (managed ホストが要る。無い環境で SKIP になるなら、SKIP を成功扱いにせず報告する)。
8. シーンの復元はファイルを書かず、シーンが dirty になる → スイート。
9. A20 (Phase 5 の横断): `tools\check_rules.ps1` が 0 error、`Editor.exe --selftest-full` が通る、`tools\replay_verify.bat` が追跡の有効 / 無効で一致、ABI・SimSnapshot・TypeId が変わっていない。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`

## 実装メモ (coder が追記)

## フィードバック履歴
