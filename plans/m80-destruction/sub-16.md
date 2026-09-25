# sub-16: review-1 — 資産の経路 (セッションをまたぐ解決・保存名・読み直し・資産キャッシュのキー・読み込みの検査・Inspector の一致表示)

- 依存: sub-15
- 状態: OK (コミット待ち)
- 往復: 1

## 出所

C:\HAL\MyEngin\plans\m80-destruction\review-1.md の指摘 #1 (blocker)・#2 (blocker、planner 宛で spec §2 に裁定)・#8・#9・#12。reviewer のプローブと画像は scratchpad の `review_probe.diff` / `probe*.log`。

## やること

1. **#1 `fractureAsset` の表現をそろえる**:
   - 問題: 今は `HashStr("guid://<hex>")` を保存している (`src/Editor/FractureBakeCommit.cpp:70`)。解決側はこれを GUID として `ResolvePath` に渡している (`FractureSystem.cpp` の資産解決)
   - 直し方: 他のファイル資産の AssetRef (例: `Collider.physMaterial` の `.physmat.json`、マテリアル) が何を保存しているかを確かめ、**同じ表現**にそろえる。保存側・解決側・Inspector の資産欄の表示・`--fracture-demo` の GUID なし登録 (`fracture://demo...`) の 4 か所で、表現が一致すること
2. **#1 読み込みの時期**:
   - シーンを読み込んだとき、**最初の物理 tick より前**に、Destructible が参照する `.mfrac` を FractureLibrary に登録する (凸包の登録も含む。1 tick 目に凸包が未登録で shape=5 が無視される問題の解消)
   - シーンの読み込み経路は Editor / Runtime / TickRunner の LoadScene の 3 か所がある (M74c の申し送り)。全部で効くこと
   - 起動時の資産走査で `.mfrac` を読むか、シーン読み込みで参照されたものだけを読むかは coder 判断 (後者が推奨)
3. **#2 保存名を内容から決める** (spec §2 の裁定):
   - 保存名は `Fracture/<エンティティ名>_<16hex>.mfrac`。16hex は焼きの入力 (ソースメッシュの頂点と index のバイト列、seed、pieceCount、openMeshMode、voxelResolution) と焼き方式の版 `kFractureBakeVersion` (新設の名前付き定数。焼きの出力が変わる変更をしたら上げる) から作る 64bit ハッシュ
   - 入力が同じなら同じファイルになってよい
4. **#2 書いた資産が読み込み済みなら読み直す**:
   - `FractureLibrary` に「同じパスの資産を読み直して、同じ AssetID のままメッシュと凸包を差し替える」口を作る (MeshLibrary の同名登録は差し替えになる。凸包は `Register` で差し替え)
   - `FractureBakeCommit` が書いた直後にこれを呼ぶ。RtScene の BVH のように同じ ID で中身が変わると古いまま残るキャッシュがあれば、合わせて無効化する (`RtScene.cpp:64-120` は同じ ID の中身の変化を検知しない。sub-11 の申し送り)
5. **#9 資産キャッシュのキー**: `FractureSystem` の `assetCache_` のキーを、ルートのエンティティではなく **`fractureAsset` の値**にする (`FractureSystem.cpp:759-781`)。Play 中に `fractureAsset` を差し替えても、新しい資産で判定されること
6. **#12 読み込みの検査**: `FractureAsset::Deserialize` (`FractureAsset.cpp:117-131`) で次を検査し、違反は失敗にする (落ちない):
   - 破片メッシュの index が頂点数の範囲内
   - 隣接先の index が破片数の範囲内で自分自身でない
   - 隣接の対称性 (i→j があれば j→i)
7. **#8 Inspector の一致表示**: 割れた後 (`broken == true`) は、FractureSystem と同じ規則 (sub-07 の `PiecesMatchAssetNow`: 重複なし・範囲内のみ) で判定する。Play 中に一度割れた後も、赤字の「一致していません」を出さない。Inspector 用の照会 (`CountFracturePieceChildren`) は、この規則の関数を共有する形にする (判定を 2 か所に書かない)

## やらないこと (このサブでは)

- 古い `.mfrac` の自動削除 (AGENTS §1)
- sub-17 / sub-18 の範囲

## 触る場所 (planner の見立て)

- `src/Editor/FractureBakeCommit.*`、`src/Editor/FractureBakeService.*`、`src/Editor/Windows/InspectorWindow.cpp`
- `src/Engine/Engine/Physics/FractureLibrary.*`、`src/Engine/Engine/Asset/FractureAsset.*`、`src/Engine/Engine/FractureSystem.*`、`src/Engine/Engine/FractureBuilder.*`
- シーン読み込みの 3 経路 (Editor / Runtime / TickRunner の LoadScene)
- `src/Engine/Engine/RayTracing/RtScene.cpp` (同じ ID の中身の差し替えを反映させる必要があれば)
- `FractureSelfTest.cpp` / `FractureEditorSelfTest.cpp`

## 受け入れ条件 (このサブ)

1. **セッションをまたぐ SelfTest**: 焼いて保存 → シーンを保存 → 新しい AssetDatabase・FractureLibrary・World でシーンを開き直す。次のことを確かめる:
   - 最初の物理 tick の前に凸包が登録済みで、Destructible が有効になっている
   - 当てると割れる
   - "fracture asset is not loaded" の ERROR が出ない
   - Inspector 用の照会が「一致」を返す
   (spec 受け入れ条件 21)
2. 名前が同じ 2 つのエンティティで、中身が違えば別のファイル、同じなら同じファイルを指す。焼き直しで中身が変わると、ライブラリの破片メッシュと凸包が新しい中身になる (spec 受け入れ条件 22)
3. Play 中に `fractureAsset` を差し替えると、新しい資産で割れる
4. 壊れた `.mfrac` (index の範囲外・隣接先の範囲外・非対称) で落ちずに失敗する
5. 割れた後の Inspector 用の照会が「一致」を返す
6. 既存の SelfTest、`replay_verify.bat` 全 job、`shot_verify.bat` (既知の 4 枚以外)、`check_rules.ps1` が PASS。WIP 不変

## 検証コマンド

```
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-16 (round 1)
実装:
  - src/Editor/FractureBakeCommit.cpp:CommitFractureBake — 保存名を `<エンティティ名>_<焼きの入力16hex>.mfrac` に変更 (ComputeFractureBakeInputHash: ソースメッシュの頂点/index バイト列・seed・pieceCount・openMeshMode・voxelResolution・kFractureBakeVersion から FNV-1a)。`comp->fractureAsset` を `HashStr("guid://<hex>")` ではなく、他の AssetRef (Collider.physMaterial 等) と同じ「GUID の値そのもの」に変更。書いた直後は `FractureLibrary::LoadFromFile` ではなく新設の `ReloadFromFile` を呼び、既に読み込み済みでも必ず読み直して登録し直す
  - src/Engine/Engine/Physics/FractureBake.h — `kFractureBakeVersion` (焼き方式の版、既定 1) を新設
  - src/Engine/Engine/Physics/FractureLibrary.h/.cpp — `ReloadFromFile(path)` を追加 (キャッシュを無視して読み直し、同じ登録名で RegisterInternal をやり直す)。`LoadFromFile` は「キャッシュに無ければ ReloadFromFile を呼ぶ」実装に整理
  - src/Engine/Engine/FractureSystem.h/.cpp — `ResolveFractureAsset` をファイルスコープの匿名名前空間から export し、ヘッダで宣言 (Inspector と共有)。`PreloadFractureAssets(World&)` を新設 (Destructible を走査して `ResolveFractureAsset` を呼ぶだけ)。`PiecesMatchAssetNow` の中身を `FracturePieceIndicesMatchAsset(assetPieceCount, indices, broken, outReason)` として export し、`PiecesMatchAssetNow` はそれに委譲する形にリファクタ。`FractureSystem::assetCache_` のキーを `EntityKey(root)` から `job.dc->fractureAsset.value` に変更 (`erroredOnce_`/`piecesByRoot` のキーは root のまま維持)
  - src/Engine/Engine/FractureBuilder.h/.cpp — `DestructiblePiecesMatchAsset(world, root, asset, broken)` を新設。World 中の全 FracturePiece から `root==root` なものを (階層を辿らず) 集めて `FracturePieceIndicesMatchAsset` へ渡す
  - src/Editor/Windows/InspectorWindow.cpp:DrawDestructibleNotes — 資産の解決を `lib->FindByAssetId` の直呼びから `ResolveFractureAsset` に変更。一致判定を `CountFracturePieceChildren` (直子限定カウント) から `DestructiblePiecesMatchAsset` (FractureSystem と同じ規則、broken 後は個数の完全一致を求めない) に変更
  - src/Engine/Engine/Asset/FractureAsset.cpp:Deserialize — 境界検査を追加: 外側/蓋メッシュの index が頂点数の範囲内、隣接先 index が破片数の範囲内かつ自分自身でない、隣接の対称性 (i→j があれば j→i)。違反はいずれも false (壊れたファイルとして安全に失敗)
  - src/Editor/EditorApp.cpp / src/Runtime/RuntimeMain.cpp / src/Engine/Engine/TickRunner.cpp — シーンロード直後 (Editor 起動時ロード・`LoadSceneFromPath` 共通経路・Runtime 起動時ロード・TickRunner の実行時 `LoadScene`) に `PreloadFractureAssets(world)` を追加。最初の物理 tick より前に .mfrac の凸包/メッシュが登録される
  - src/Editor/FractureEditorSelfTest.cpp — 保存名の書き換えに合わせて (5) の実ファイル名検査を prefix 一致に変更。新規 (6) セッションをまたぐ解決 (実 AssetDatabase でセッション 1 を焼いて保存 → セッション 2 で新しい AssetDatabase/FractureLibrary/World を作って開き直し、最初の物理 tick 前の凸包登録・Inspector 相当の一致判定・実際に割れることを検証)。新規 (7) 同名 2 エンティティが中身の違いで別ファイル、同じ中身の再焼きで同じファイルになることを検証
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — (13c2) 壊れた `.mfrac` (範囲外 outer/cap index・範囲外/自己参照の隣接・非対称な隣接) が Deserialize で失敗することを検証。(16g) 同じ `FractureSystem` インスタンスのまま `fractureAsset` を差し替えても (Play 中の Inspector 操作を模す)、新しい資産で正しく解決・破断することを検証 (資産キャッシュが root キーのままだと index out of range で壊れていたケース)
仕様との差分:
  - [追加] 起動時の資産走査 (`RegisterAssetLibraries`/`DemoContent.cpp`) に `.mfrac` を足さず、「シーン読み込みで参照されたものだけを読む」方式にした。sub-16.md のやること2 で明示的に coder 判断とされ、後者が推奨と書かれていたため
  - [未実装] やること4 の「RtScene の BVH のように同じ ID で中身が変わると古いまま残るキャッシュがあれば、合わせて無効化する」は実装していない。内容ハッシュ命名にしたことで「同じパス・別内容」が起こるのは `kFractureBakeVersion` を上げ忘れた場合の保険的なケースに限られ (通常の再焼きは中身が同じか、パス自体が変わる)、かつ `MeshColliderLibrary`/`RtScene` には部分無効化の API が無く、この保険的ケースのためだけに無関係な RT サブシステムへ手を入れるのはスコープが広すぎると判断した。実測でも本サブの範囲では発生しない (shot_verify の fracture_before/after は無関係のまま PASS)。プランナーの判断を仰ぎたい (不安・質問参照)
  - [逸脱] spec §4.3 の「保存先: `Fracture/<エンティティ名>_<seed>_<pieceCount>.mfrac`」は sub-16.md のやること3・spec §2 (review 由来の裁定行) の「内容の16hexハッシュ」を優先して実装した。§4.3 本文はまだ更新されておらず、§2 の追記行と矛盾したまま残っている (プランナー側の記述整理が必要)
検証:
  - Debug|x64 / Release|x64 ビルド (Engine.vcxproj / Editor.vcxproj / Runtime.vcxproj) → 全て成功 (警告 0)
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0、FAIL 0 件。新規テストすべて PASS (session:* / duplicate names:* / asset cache:* / fracture asset: an outer index.../a cap index.../a neighbor index.../a self-referencing.../a one-sided...)
  - `bin\x64\Release\Editor.exe --selftest` → exit 0、FAIL 0 件。fracture bake digest は Debug と一致 (`0x521C987A83D781F7`)
  - `tools\replay_verify.bat` → 14 job 全 PASS (fracture 含む)
  - `tools\shot_verify.bat` → FAIL 4 枚 (parts / joints / acoustic_forward / acoustic_deferred、台帳記載の既知除外と一致)。fracture_before / fracture_after / flow_title を含む残り全て PASS
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - `git status` → WIP (WaterPass.cpp / .agents / SKILL.md) 無変更、コミット対象は本サブで触った 16 ファイルのみ (新規ファイル無し、gen_project_files.ps1 不要)
自己採点 (1-5):
  仕様適合: 5 — #1 #2 #8 #9 #12 の全項目を実装し、受け入れ条件21・22 相当の新規 SelfTest で実測確認した。§4.3 の未更新記述との食い違いは仕様側の記述漏れとして差分に記載
  正しさ: 5 — 修正前は再現する形 (旧コードに戻すと FAIL する構成) でバグを踏む新規テストを書き、修正後に PASS することを実行して確認した。replay_verify/shot_verify で回帰も確認済み
  コード品質: 4 — 判定・解決ロジックの重複を解消し共有関数化した。一方で `src/Engine/Engine/FractureBuilder.h` の `ValidateFracturePieces` の既存コメント (「呼び出し元 (FractureSystem)」) は元から実態と食い違っており (実際は FractureSystem 独自実装を使っていた)、今回のリファクタでさらに紛らわしくなったが、指摘外のコードなので触っていない (申し送り参照)
  テスト: 5 — 新規 SelfTest 6 系統 (セッションをまたぐ解決・保存名衝突/再利用・資産キャッシュのキー・壊れた.mfracの5種の検査) を追加し、Debug/Release 双方で実行して PASS を確認した
不安・質問:
  - RtScene/MeshColliderLibrary の「同じ ID で中身が変わったときの無効化」を本サブで実装しないことにした判断 (仕様との差分の [未実装] 参照) の是非をプランナーに確認したい。却下でなければ sub-17/18 か新規サブへ回してほしい
  - spec §4.3 の保存先の記述が §2 の review 由来の裁定 (16hex 命名) と矛盾したまま残っている。プランナー側で §4.3 本文の更新をお願いしたい
触ったファイル:
  - src/Editor/EditorApp.cpp
  - src/Editor/FractureBakeCommit.cpp
  - src/Editor/FractureBakeCommit.h
  - src/Editor/FractureEditorSelfTest.cpp
  - src/Editor/Windows/InspectorWindow.cpp
  - src/Engine/Engine/Asset/FractureAsset.cpp
  - src/Engine/Engine/FractureBuilder.cpp
  - src/Engine/Engine/FractureBuilder.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/Physics/FractureBake.h
  - src/Engine/Engine/Physics/FractureLibrary.cpp
  - src/Engine/Engine/Physics/FractureLibrary.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/TickRunner.cpp
  - src/Runtime/RuntimeMain.cpp
申し送り:
  - `src/Engine/Engine/FractureBuilder.h` の `ValidateFracturePieces` (sub-07 由来) は本番経路のどこからも呼ばれておらず (FractureSelfTest.cpp のテストからのみ使用)、FractureSystem.cpp は独自の等価ロジックを持っていた (今回そちらを export して Inspector と共有した)。コメントに「呼び出し元 (FractureSystem)」とあるのは実態と食い違っている。整理するなら次のサブで (今回はスコープ外として触っていない)
  - sub-17 は `strength`/縮み挙動/スキン位置/隣接対称切り捨てで `FractureSystem.cpp` を再度触る。本サブで `assetCache_` のキーと `ResolveFractureAsset`/`PiecesMatchAssetNow` の export 位置を変えたので、diff の起点は本サブのコミット後にすること
```

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1〜6 を満たした。RT / MeshColliderLibrary の無効化の見送りを採用 (名前が内容から決まるので、同じ ID の中身が変わる経路は版の上げ忘れだけ)。spec §4.3 の本文は planner が直した。`ValidateFracturePieces` の重複ロジックの整理は sub-17 へ
