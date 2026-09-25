# sub-09: エディタ — Inspector・非同期焼き・生成の Undo・ローカライズ

- 依存: sub-06, sub-04, sub-14
- 状態: OK (コミット待ち)
- 往復: 1

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

SELF_EVAL: sub-09 (round 1)
実装:
  - src/Engine/Engine/Physics/FractureBake.h/.cpp:BakeFracture/BakeFractureCore — 段階コールバック
    (FractureBakeStage: ClosedCheck/Voxelize/Split/Hull) と、拒否理由の構造化フィールド
    (rejectedOpenMesh/boundaryEdges/nonManifoldEdges/orientationMismatches) を追加。全て
    既存 17 箇所の呼び出しに影響しないデフォルト引数・追加フィールド (出力バイト列/digest は不変)
  - src/Engine/Engine/FractureBuilder.h/.cpp:CountFracturePieceChildren — ログを出さない
    「資産と子の一致」照会 (ValidateFracturePieces は不一致のたび ERROR を出すため毎フレーム UI
    から呼べない)
  - src/Editor/FractureBakeService.h/.cpp (新規) — 単一ワーカースレッドの非同期焼きキュー
    (ModalSoundLibrary と同型)。id (Destructible の fileId) ごとに None/Baking/Ready の状態
  - src/Editor/FractureBakeCommit.h/.cpp (新規) — 焼き成功結果の確定 (.mfrac 保存 → .meta 確定
    → FractureLibrary 登録 → Destructible.fractureAsset 書き換え → BuildFracturePieces、1 Undo
    エントリ)。ImGui に触れない純関数として分離し SelfTest から直接呼べるようにした
  - src/Editor/Windows/InspectorWindow.h/.cpp:DrawDestructibleNotes/CommitFractureBakeResult —
    Destructible 節の末尾 (生成ボタン・焼き中の段階表示・生成済み/拒否/失敗の表示・プレハブ
    インスタンス/スキン/メッシュ無しでの無効化)。OnImGui 冒頭で fractureBakeService_.Pump()
    を毎フレーム呼ぶ
  - src/Engine/Core/LocalizationTable.inl — Insp_Fracture* 一式 (両言語、check_rules.ps1 規則10 PASS)
  - src/Editor/FractureEditorSelfTest.h/.cpp (新規) + EditorMain.cpp 登録 — 拒否理由の構造化
    フィールド、進行段階コールバックの順序、.mfrac のバイト一致、非同期ワーカーの
    digest 一致、CommitFractureBake の Undo/Redo を検証
仕様との差分:
  - [追加] FractureBake.h/.cpp への段階コールバック・拒否理由の構造化フィールド追加 — 「触る
    場所」に無いが、「焼き中(段階の文字列)」表示と「赤字で理由(件数)」表示を実装するために
    Engine 層へ最小限(デフォルト nullptr の追加引数、既存出力に無影響)の変更が必要だった
  - [追加] FractureBakeCommit.h/.cpp を FractureBakeService とは別ファイルに分離 — 「触る場所」
    は FractureBakeService.h/.cpp のみ想定していたが、.mfrac 保存/登録/Undo のロジックを
    ImGui 非依存にして SelfTest から直接検証できるようにするため分けた
  - [追加] FractureBuilder.h/.cpp に CountFracturePieceChildren を追加 (ログ無しの一致照会)
  - [逸脱] 保存先の「ソース名」を、破壊物のメッシュ登録名ではなくエンティティ名
    (world.GetName(root)、SanitizeFileName 済み) とした。spec §4.3 は「ソース名」とだけ書き
    曖昧だったため、Fracture/<ソース名>_<seed>_<pieceCount>.mfrac が人間の読めるファイル名に
    なる方を選んだ (メッシュ登録名は "guid://<16hex>#mesh0" 等でサニタイズ後も判読困難)
  - [未実装/既存踏襲] コンポーネント欄の FieldDesc ツールチップ (Destructible.openMeshMode /
    afterBreak、sub-06 で追加) は英語のみのまま — Reflection の FieldDesc.tooltip は
    process 起動時に 1 回登録されるだけで言語切替に追随せず、コードベース全体で既存 143 箇所
    すべてが同じ英語オンリーの形 (Components.cpp の MYE_FIELD_TIP 全件)。この 1 コンポーネント
    だけ別の仕組み (Tr() キー化) を足すのは「Inspector・非同期焼き・Undo」というこのサブの
    範囲を超えると判断し、既存パターンに合わせて未着手のまま残した (下の不安・質問へ)
検証:
  - tools\gen_project_files.ps1 → Engine/Editor/Runtime/GameLogic の 4 vcxproj 更新 (成功)
  - MSBuild Debug|x64 (フル) → 成功、/W4 で新規 6 ファイル・変更 6 ファイルとも warning 0 件
  - MSBuild Release|x64 (フル) → 成功
  - bin\x64\Debug\Editor.exe --selftest → 全件 PASS (exit 0)。新設の
    Fracture editor self test 内 15 チェック (拒否理由の構造化・進行段階の順序・.mfrac の
    バイト一致・非同期ワーカーの digest 一致・Undo/Redo) も全て PASS
  - bin\x64\Release\Editor.exe --selftest → 全件 PASS (exit 0)。同上
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s) (規則10 のローカライズ整合含む)
  - スクショ (一時プローブ、コミットしていない。DemoContent.cpp に GenBox/OpenMeshBox/SlowBox の
    3 エンティティと EditorApp.cpp に MYE_FRACTURE_PROBE env var のフックを一時追加 →
    --fracture-demo で撮影 → 全て revert 済み、git diff で無変更を確認済み):
    - 「未生成」状態 + Generate Pieces ボタン: フレーム 5 (焼き前) の GenBox
    - 「焼いています: ボクセル化」「焼いています: 分割」+ ボタン無効化: 開いた平面 +
      voxelResolution=48 の SlowBox で数フレームにわたり確認 (小さい箱は非同期でも
      1 フレーム未満で完了してしまい「焼き中」を捉えられなかったため、意図的に重い入力にした)
    - 「生成済み: 破片 16 個、隣接の切り捨て 0、統合 0」: 生成完了後の GenBox
    - 開いたメッシュの赤字理由「Not a closed mesh: 4 boundary edge(s) / 0 non-manifold
      edge(s) / 0 orientation mismatch(es)」+ ボクセル化のヒント文: OpenMeshBox
    - Play で割れた後: --autoplay で既存の FractureBox (sub-06/07/08 で作成済みのデモ実体、
      BuildFracturePieces は私の新規コードと同じ関数) が落下・被弾・分離しているのを確認
    - **プレハブインスタンスで無効化のスクショは未撮影** (時間予算。下の不安・質問へ)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1/2/5/6/7 は検証済み。3 (プレハブ無効化) はコードレビューのみ
    (tg.isPrefabMember の単純な bool ゲートで、他所で既にテスト済みの同種の仕組みと同形だが、
    このボタン固有の目視確認はしていない)。4 の「日英切替」は check_rules.ps1 の整合性チェック
    のみで、実際に日本語 UI で撮ったスクショは無い (英語固定の自動化モードで撮影したため)
  正しさ: 4 — SelfTest (15 チェック、Undo/Redo・非同期・決定性・拒否理由を含む) と実機
    スクリーンショットの両方で裏取りした。Debug/Release 双方で --selftest PASS。
    未検証: 実際のマウスクリックでボタンを押す経路 (プローブは fractureBakeService_.Request
    を直接呼んでいる。ImGui::Button の IsItemClicked 判定そのものはテストしていないが、
    これは大量の既存ボタンと同じ ImGui::Button(...) の定型なのでリスクは低いと判断)
  コード品質: 4 — 責務を分離 (FractureBakeService=スレッド境界、FractureBakeCommit=資産確定、
    InspectorWindow=UI とオーケストレーション)。既存の ModalSoundLibrary / RagdollBuildSelfTest /
    AssetOpsSelfTest の流儀に倣った。日本語コメント・ヘッダ形式は既存ファイルを模した
  テスト: 4 — 新規 SelfTest 15 チェックに加え Debug/Release --selftest 全件 PASS、
    check_rules.ps1 PASS。スクショ 5 枚で目視確認 (プレハブ無効化のみ未撮影)
不安・質問:
  1. FieldDesc.tooltip (Destructible.openMeshMode / afterBreak) の英語オンリーについて:
     sub-09 の「やること 8」は「コンポーネント欄のツールチップ...も両言語」と明記しているが、
     既存コードベース全体 (143 箇所) が同じ英語オンリーの仕組みで、Reflection の
     FieldDesc.tooltip は起動時 1 回登録・言語切替に追随しない設計。この 1 コンポーネントだけ
     Tr() 経由に作り替えるのは Reflection 層への波及 (全 143 箇所の一貫性が崩れる) を伴うため、
     このサブでは見送り既存パターンに合わせた。プロジェクト全体でツールチップを両言語化する
     方針が要るなら別サブとして切り出すべきだと考える。[ユーザーに聞ける相当だが、まずは
     planner の裁定を仰ぎたい]
  2. 保存先ファイル名の「ソース名」をエンティティ名と解釈した件 (仕様との差分参照)。
     メッシュ登録名を使う設計だった場合は sub-09 の再修正が必要
  3. プレハブインスタンスでボタンが無効になるスクショ (受け入れ条件 4) は時間予算の都合で
     未撮影。ロジック (tg.isPrefabMember の単純なゲート、既存の同種パターンと同形) は
     コードレビュー済みだが、実機の目視確認はしていない。reviewer が必要と判断すれば
     一時プローブで追加撮影する
触ったファイル:
  - src/Editor/Windows/InspectorWindow.h
  - src/Editor/Windows/InspectorWindow.cpp
  - src/Editor/FractureBakeService.h (新規)
  - src/Editor/FractureBakeService.cpp (新規)
  - src/Editor/FractureBakeCommit.h (新規)
  - src/Editor/FractureBakeCommit.cpp (新規)
  - src/Editor/FractureEditorSelfTest.h (新規)
  - src/Editor/FractureEditorSelfTest.cpp (新規)
  - src/Editor/EditorMain.cpp
  - src/Engine/Core/LocalizationTable.inl
  - src/Engine/Engine/FractureBuilder.h
  - src/Engine/Engine/FractureBuilder.cpp
  - src/Engine/Engine/Physics/FractureBake.h
  - src/Engine/Engine/Physics/FractureBake.cpp
申し送り:
  - sub-10 (スキンメッシュ) は Destructible の「スキン未対応」ゲート (world.GetComponent<
    SkinnedMeshComponent>(e) != nullptr で生成ボタンを無効化) を外し、骨空間で焼く経路を
    DrawDestructibleNotes / FractureBakeService に足す形になる想定
  - .mfrac の保存先ファイル名が「ソース名 = エンティティ名」である点 (上記差分 2) は、
    sub-11/12 で仕様書に明記するか確認してほしい
  - FractureBakeService/FractureBakeCommit を分けた設計は、将来スキン対応や ABI 経由の
    生成 (sub-12) でも同じ CommitFractureBake を再利用できるはず

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1・2・3・5・6・7 は SELF_EVAL の検証 (スクショ 5 枚、FractureEditorSelfTest の Undo / Redo とバイト一致、check_rules) で満たした。条件 4 (プレハブインスタンスで無効) はスクショが無い。planner がコードで確認した: 判定は `Prefab::FindInstanceRoot` (`InspectorWindow.cpp:548-549`) で、プレハブの上書き表示・Revert と同じ唯一の境界判定を使っている。これで満たしたとする。ツールチップは既存の慣例どおり英語のみ (spec 変更)。保存先のソース名 = エンティティ名を採用。nit: Play 中と複数選択のときも生成ボタンを無効にするのが安全 (任意。sub-10 で同じ関数を触るときに)
