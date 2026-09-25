# sub-03: 破片資産 `.mfrac` と FractureLibrary

- 依存: sub-02
- 状態: OK (commit 8c9d984)
- 往復: 1

## やること

spec §4.2「`.mfrac`」と、破片メッシュ・凸包の登録 (spec §2 「破片メッシュの参照方法」) を作る。

1. **形式**: `src/Engine/Core/ByteIo.h` の `ByteWriter` / `ByteReader` で、magic `"MFRC"`・版 1。中身は spec §4.2 の列挙どおり (スキンの骨名は版 1 から欄だけ用意し sub-10 で埋めてよい。空文字 = 骨なし)。凸包は `SerializeConvexHull` / `DeserializeConvexHull`。書き出しは同じ入力で同じバイト列、読みは境界検査付きで壊れたファイルでも落ちない
2. **AssetType**: `AssetDatabase.h:13-32` の enum に `Fracture` (`.mfrac`) を**末尾追加**。`.meta` の GUID が振られること
3. **FractureLibrary** (Engine 層。名前は coder 判断): `.mfrac` を AssetID で読み込み、破片 i について
   - MeshLibrary に `guid://<mfracGuid16hex>#frag<i>` (外側面) と `#frag<i>#cap` (蓋) を `MeshVertex` で登録 (ボーン欄は 0)
   - ConvexColliderLibrary に `#frag<i>#hull` の AssetID で凸包を `Register`
   - 読み込み結果 (破片数、隣接表、原点、体積、状態) を引ける API
   - GUID の無いメモリ上の焼き結果を登録する口 (登録名の接頭辞を渡す。sub-06 の demo が `fracture://demo...` で使う)
   - **再登録**: `ConvexColliderLibrary::Clear()` が呼ばれる経路 (シーン切替・ホットリロード等。coder が全呼び出し箇所を洗う) の後に、読み込み済みの凸包を登録し直す。漏れると shape=5 が null 解決され破片がすり抜ける (黙った壊れ方)
   - 読み込みの時期: シーンのロード時 / Destructible を初めて見たとき (tick の前)。**tick 中に遅延で読むなら同期で、結果が実行タイミングに依らない**ことを保証する (TerrainAsset の同期読み込みと同じ)
4. **失敗の局所化** (spec §4.1 エッジケース): 見つからない / 読めない / 版違いは「その資産だけ無効」+ ERROR 1 回。落ちない
5. モジュール注入の流儀 (`convexcol::Install` と同じ) で、EngineLoop が起動時に Install / 終了時に外すかは coder 判断

## やらないこと (このサブでは)

- コンポーネント・エンティティ構築 (sub-06)
- ホットリロード (`ReloadHub` に `.mfrac` を足すのは任意。足すなら同じ AssetID で中身を差し替え、凸包キャッシュも差し替える)

## 触る場所 (planner の見立て)

- 新規 `src/Engine/Engine/Asset/FractureAsset.h/.cpp` (形式) と `src/Engine/Engine/Physics/FractureLibrary.h/.cpp` (登録)。分け方は coder 判断
- `src/Engine/Engine/AssetDatabase.h/.cpp` (AssetType 末尾追加、拡張子の対応)
- `src/Engine/Renderer/GpuResources.h` の `MeshLibrary::Register` (使うだけ)
- `src/Engine/Engine/Physics/ConvexColliderLibrary.h/.cpp` の `Register` / `Clear` の呼び出し元 (再登録の差し込み)
- 前例: `src/Engine/Engine/Asset/TerrainAsset.cpp` (magic / 版 / Reader)、`ModalSoundLibrary` (`guid://<16hex>#modal#<id>` 登録名)
- `FractureSelfTest.cpp` にケース追加

## 受け入れ条件 (このサブ)

1. sub-02 の焼き結果 → 書く → 読む → 書く がバイト一致。読んだ破片メッシュ・凸包・隣接が焼き結果と一致 — `--selftest`
2. 切り詰め・magic 違い・版違い・件数が上限超えのファイルで、落ちずに失敗を返す — `--selftest`
3. 読み込みで MeshLibrary に `#frag<i>` / `#frag<i>#cap`、`convexcol::Resolve(#frag<i>#hull)` が非 null。`ConvexColliderLibrary::Clear()` を呼んだ後も (再登録の経路を通して) 非 null — `--selftest`
4. GUID のない登録口 (メモリ上の焼き結果) でも同じ登録が行われる — `--selftest`
5. 既存 SelfTest・`check_rules.ps1` に変化なし。既存シーンの挙動に影響なし (FractureLibrary は `.mfrac` が無ければ何もしない) — `--selftest`、`check_rules.ps1`
6. WIP ファイル不変 — `git status`

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
実装:
  - `src/Engine/Engine/Asset/FractureAsset.h/.cpp` (新規) — `.mfrac` の保存形式 (`FractureData`/`PieceRecord`/`NeighborRecord`)。`ByteWriter`/`ByteReader` (`Engine/Core/ByteIo.h`) で `Serialize`/`Deserialize`、`WriteFileReplacing` (`PathUtil.h`) で原子的な `Save`、`Load` はファイル読込 + `Deserialize`。magic `"MFRC"`・版 1。凸包は `SerializeConvexHull`/`DeserializeConvexHull` の生バイトを長さ前置きで埋め込む。境界検査: 破片数は resize 前に `kMaxFracturePieces` で、隣接数は読み込み後に `kMaxFractureNeighbors` で検算し、超過は失敗を返す (bad_alloc を作らない)
  - `src/Engine/Engine/Physics/FractureLibrary.h/.cpp` (新規) — `.mfrac` の読み込みと `MeshLibrary`/`ConvexColliderLibrary` への登録。登録名は `"guid://<mfracGuid16hex>#frag<i>"` 系 (`assetkey::SubAssetKeyPrefix` 由来)、GUID の無いメモリ焼き結果は呼び出し側が渡す任意の接頭辞 (`RegisterBaked`)。`FractureAssetHandle` が登録済み `AssetID` と元の `FractureAsset::FractureData` (凸包データ込み) を保持し、`ReregisterAll()` が `ConvexColliderLibrary::Clear()` 後に凸包を登録し直す。`BuildFractureAssetData` が `FractureBakeResult` (`FractureVertex`) → `.mfrac` 形式 (`MeshVertex`) への詰め替えを行う。読み込み失敗はパスごとに ERROR 1 回 + nullptr (再試行しても再ログしない)
  - `src/Engine/Engine/AssetDatabase.h/.cpp` — `AssetType::Fracture` を末尾追加、`.mfrac` の `ClassifyPath`/`TypeName`/`ParseTypeName` を配線 (`.terrain.json` と同じ並び)
  - `src/Engine/Engine/EngineLoop.cpp` — `FractureLibrary fractureAssets` を追加し、`convexColliders.Init` の直後に `Init(&resources, &convexColliders)` + `fracturelib::Install`。終了時に `fracturelib::Install(nullptr)` (`convexcol::Install(nullptr)` と対称)。sub-03 の「EngineLoop への Install はコーダー判断」の指示に従い実施 (Destructible が無いシーンでは何も呼ばれないので存在ゲート違反にはならない)
  - `src/Engine/Engine/Physics/FractureMesh.h` — 申し送りどおり、ヘッダコメントの sub-02/sub-03 という作業経緯の参照を除去 (理由の文は残し、詰め替え先を「FractureLibrary」と具体名で書き直した)
  - `src/Engine/Engine/Physics/FractureSelfTest.h/.cpp` — セクション 13 として `.mfrac` の往復・壊れた入力・`FractureLibrary` 登録/`Clear()`後の再登録・ファイル往復・欠落ファイルのテストを追加 (27 チェック)
  - `src/Engine/Engine/AssetDatabaseSelfTest.cpp` — `.mfrac` の `ClassifyPath`/`TypeName`往復チェックを追加 (自分が触った `AssetType::Fracture` の検証)
  - `build/Engine.vcxproj` / `build/Engine.vcxproj.filters` — `tools\gen_project_files.ps1` で新規ファイルを登録
仕様との差分:
  - [追加] EngineLoop への `fracturelib::Install` 配線を実施した (sub-03 本文が「coder 判断」としていた選択)。既存シーンは `FractureLibrary` を誰も呼ばないため挙動不変
  - [追加] `AssetDatabaseSelfTest.cpp` に `.mfrac` 分類の検証を 1 件追加した (「既存 SelfTest に変化なし」は退行が無いことと解釈し、自分が追加した `AssetType::Fracture` 自体の検証は追加した)
  - [未実装] `ReloadHub` への `.mfrac` 追加は sub-03 本文で明示的に「任意」とされているため見送った
  - [未実装] スキンの `boneName` 欄は版 1 から用意したが常に空文字 (sub-10 で埋める、sub-03 本文の指示どおり)
検証:
  - `powershell -File tools\gen_project_files.ps1` (pwsh 経由) → Engine.vcxproj に新規 4 ファイルを登録
  - MSBuild `Debug|x64` / `Release|x64` フルビルド → エラー 0 (警告は既存の C4127 のみ、無関係)
  - `bin\x64\Debug\Editor.exe --selftest` → 全体 exit code 0、FAIL: 0 件。`==== Fracture mesh core self test: ALL PASS ====`、新規 27 チェック含め全 PASS を確認
  - `bin\x64\Release\Editor.exe --selftest` → 全体 exit code 0、FAIL: 0 件。Fracture セクション全 PASS。既存の bake digest (lshape/torus) が Debug と同一値であることも確認 (0xA62D9B06031B23C8 / 0x87A81F882E68E6DE)
  - `tools\check_rules.ps1` → 0 error(s), 0 warning(s)
  - `git status --short` → WIP (`WaterPass.cpp`、deepmodal 系、`.agents/`、`SKILL.md`) は未変更。触ったのは新規 4 ファイルと上記 7 ファイルのみ
自己採点 (1-5):
  仕様適合: 5 — spec §4.2 の列挙 (ソース識別・破片数・破片ごとの外側/蓋メッシュ・凸包・隣接・骨名欄・焼きの記録) を全て実装し、受け入れ条件 1-6 を選定テストで検証済み
  正しさ: 5 — 実ビルド (Debug/Release 両方) 通過、`--selftest` 全体 exit code 0 を確認。バイト往復・壊れた入力・上限超過・Clear 後の再登録・欠落ファイルはすべて故障条件そのもので検証した
  コード品質: 4 — 既存の `TerrainAsset`/`ConvexColliderLibrary`/`ModalSoundLibrary` の流儀 (Init/Install、ByteIo、失敗の局所化) に揃えた。nit: `RegisterInternal`/`ReregisterAll` はテストでのみ実行され、本番の呼び出し元 (Clear() を呼ぶ側) がまだ無い
  テスト: 5 — 新規 27 チェックを Debug/Release 両方で実行し全 PASS を確認。故障条件 (切り詰め・magic 違い・版違い・件数超過・欠落ファイル) を実データで再現して確認した
不安・質問:
  - `ConvexColliderLibrary::Clear()` は現状コードベース全体で本番経路からの呼び出しが 1 件も無い (`PhysicsSelfTest.cpp`/`ModalSelfTest.cpp` のローカルインスタンスのみ)。今回作った `FractureLibrary::ReregisterAll()` は SelfTest から直接呼んで契約を固定したが、実際に `convexColliders.Clear()` を呼ぶ経路 (シーン切替等、まだ存在しない) ができたときに `fracturelib::Library()->ReregisterAll()` を必ず併せて呼ぶ、という規約はコード上のコメントでしか強制していない。sub-06 以降でシーン切替相当の処理を足すときに、この規約を思い出せる場所 (ADR や sub の申し送り) に残すべきか planner の判断を仰ぎたい
  - EngineLoop への配線 (Install/Uninstall) を本サブで実施した判断が妥当か。妥当でない場合は差し戻しで巻き戻す
触ったファイル:
  - src/Engine/Engine/Asset/FractureAsset.h (新規)
  - src/Engine/Engine/Asset/FractureAsset.cpp (新規)
  - src/Engine/Engine/Physics/FractureLibrary.h (新規)
  - src/Engine/Engine/Physics/FractureLibrary.cpp (新規)
  - src/Engine/Engine/AssetDatabase.h
  - src/Engine/Engine/AssetDatabase.cpp
  - src/Engine/Engine/AssetDatabaseSelfTest.cpp
  - src/Engine/Engine/EngineLoop.cpp
  - src/Engine/Engine/Physics/FractureMesh.h
  - src/Engine/Engine/Physics/FractureSelfTest.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - build/Engine.vcxproj (gen_project_files.ps1 の生成物)
  - build/Engine.vcxproj.filters (gen_project_files.ps1 の生成物)
申し送り:
  - sub-06 (Destructible / root proxy) は本サブの `FractureLibrary::LoadFromFile` / `RegisterBaked` / `Find` をそのまま使える。シーンロード時や Destructible を初めて見たときに `LoadFromFile` を呼ぶ設計で問題ない (同期・べき等)
  - `.mfrac` の実際の保存先 (`assets/Fracture/<ソース名>_<seed>_<pieceCount>.mfrac`) と Inspector からの `Save` 呼び出しは sub-09 の担当。`FractureAsset::Save` は親ディレクトリを自動作成するので、そのまま使ってよい
  - `ConvexColliderLibrary::Clear()` の実呼び出し元がまだ存在しない件は上記「不安・質問」を参照。sub-06 以降でシーン切替等により Clear() を呼ぶ経路を作る場合、`fracturelib::Library()->ReregisterAll()` を必ず対で呼ぶこと

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1〜6 を満たした (Debug / Release の selftest FAIL 0、digest 一致、check_rules 0/0、WIP 不変)。EngineLoop への Install の配線は採用 (`convexcol::Install` と同じ流儀、`EngineLoop.cpp:289-290, 2589`)。`Clear()` の本番呼び出しは 0 件なので仕組みは足さず、契約を ADR-021 と `Clear()` の宣言コメントに書く (sub-12 へ)。ReloadHub の見送りと boneName が空のままであることは仕様どおり
