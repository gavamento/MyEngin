# sub-09: エディタ — Inspector・非同期焼き・生成の Undo・ローカライズ

- 依存: sub-06, sub-04
- 状態: 未着手
- 往復: 0

## やること

spec §4.3 の Inspector を作る。焼きは Engine 層の純関数、UI とワーカーは Editor 層。

1. **`DrawComponentNotes`** (`InspectorWindow.cpp:1043-1086`) に Destructible の分岐を足し、専用関数 (例 `DrawDestructibleNotes`) へ。ModalSound (`:1083`, `DrawModalSoundNotes :1091-1155`) の書き方に倣う
2. **焼きのワーカー**: ModalSoundLibrary の形 (`std::thread` + キュー + 状態の列挙、`Pump` はメインスレッド) に倣う。状態 = 未生成 / 焼き中 (段階の文字列: 閉じ判定 / ボクセル化 / 分割 / 凸包) / 生成済み / 失敗 / 拒否 (閉じていない)。**焼き中も UI は固まらない**。入力の CPU メッシュはメインスレッドで写してからワーカーへ渡す (ワーカーから MeshLibrary を触らない)
3. **生成ボタン**: 完了したらメインスレッドで `.mfrac` を書き (保存先 spec §4.3)、AssetDatabase に認識させ (`.meta`)、FractureLibrary に読み込み、`Destructible.fractureAsset` を設定し、`BuildFracturePieces` で子を組み直す。**子の組み直しと欄の変更は UndoStack の 1 記録** (`UndoStack::Record(... StructuralChanges::Apply ...)`、`UndoStack.h:87-109`)。`.mfrac` ファイル自体は Undo で戻さない (同じ入力なら同じバイト列なので、戻した子は再焼きで同じ資産を指す — SELF_EVAL で確認)
4. **拒否の表示**: 閉じていないとき赤字で理由 (境界辺 / 非多様体辺 / 向き不一致の件数) と「ボクセル化を許容に切り替えると割れます」
5. **プレハブインスタンス**では生成ボタンを無効にし理由を出す (`Prefab.h:95`: 子の追加・削除は上書きとして記録されない)。プレハブ資産の編集中・プレハブでないエンティティでは有効
6. **ソースメッシュ**: そのエンティティの `MeshRenderer.mesh`。無い / CPU 頂点が無い / builtin の平面など → 理由付きで生成不可 (平面は「閉じていない」で拒否 → ボクセル化で可)。スキン (SkinnedMesh を持つ) は sub-10 まで「未対応」と表示して無効
7. **状態表示**: 生成済みなら破片数・隣接の切り捨て数・統合数・資産パス。資産と子の不一致 (sub-06 の整合確認) は赤字
8. **ローカライズ**: すべての文字列を `LocalizationTable.inl` に両言語で (`###` 識別子一致、`Tr()` を printf 書式に渡さない)。コンポーネント欄のツールチップ (enum 相当の値の意味) も両言語
9. `pieceCount` / `voxelResolution` 等の欄の範囲は `MYE_FIELD_RANGE`。上限は sub-11 の実測で見直す (ここでは spec §4.2 の範囲)

## やらないこと (このサブでは)

- スキン (sub-10)、ABI (sub-12)
- 数値の進捗バー (段階の文字列でよい)

## 触る場所 (planner の見立て)

- `src/Editor/Windows/InspectorWindow.cpp`
- 新規 `src/Editor/FractureBakeService.h/.cpp` (ワーカー。名前は coder 判断)
- `src/Engine/Core/LocalizationTable.inl`
- `src/Editor/Undo/UndoStack.*` (使うだけ)
- `src/Editor/EditorMain.cpp` (SelfTest 登録があれば)

## 受け入れ条件 (このサブ)

1. 箱のエンティティに Destructible を付けて生成を押すと、焼き中の表示が出て UI が固まらず、完了で `.mfrac` と破片の子ができ、Play で落として割れる — スクショ 3 枚 (焼き中 / 生成済み / Play で割れた後) の画像パス。撮り方は一時プローブ + `--screenshot` でよい (プローブはコミットしない)
2. Undo 1 回で子と欄が生成前へ戻り、Redo で戻る — `--selftest` (Undo の生成関数レベル) またはスクショ
3. 開いたメッシュ (平面) で赤字の理由が出る。ボクセル化を許容にすると生成できる — スクショ
4. プレハブインスタンスでボタンが無効で理由が出る — スクショ
5. 同じ設定で 2 回生成した `.mfrac` がバイト一致 — `--selftest` または `fc /b`
6. ローカライズ: 日英切替で文字列が出る。`check_rules.ps1` (規則 10) PASS
7. 既存 SelfTest PASS、WIP 不変

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 をビルド、Release は selftest のため）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
（一時プローブ + --screenshot でインスペクターを撮る。プローブはコミットしない）
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

## フィードバック履歴
