# sub-03: 破片資産 `.mfrac` と FractureLibrary

- 依存: sub-02
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
