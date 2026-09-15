# sub-02 (M76b): ボクセライザと --modal-voxelize

- 依存: なし (sub-01 と並列可。`ModalTypes.h` の定数を両方が足す場合は先に入った方に合わせる)
- 状態: 未着手
- 往復: 0

## やること
spec §4.1「ボクセル化」と §4.2 `.mvox`。学習とランタイムが共有する**唯一の**ボクセル化実装 (ユーザー計画 Phase 2 / Checkpoint B)。

- `Modal/Voxelizer.h/.cpp`: `VoxelFrame { origin[3]; voxelSize; aabbMin[3]; aabbMax[3]; longestEdge; }`、
  `VoxelGrid { frame; std::array<uint8_t, 32768> occ; surfaceCount; interiorCount; }`、
  `bool VoxelizeMesh(const XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m, VoxelGrid& out)`、
  `TriBoxOverlap` (Akenine-Möller 13 軸、箱半径 `h/2 + 1e-6h`、退化三角形も辺として拾う)、`FloodFillInterior` (pad リングから 6 近傍、明示スタック、固定順)、
  `LocalPointToCell(const VoxelFrame&, const float p[3]) -> uint16_t`、`BuildCellSlotTable(const VoxelGrid&, uint16_t cellSlot[4096])` (無効 cell は焼き時に総当たりで最寄り有効 cell、同値は小 index)、
  `SerializeVox` / `DeserializeVox` (64 B ヘッダ、フィールド単位で書く)。
- `Modal/TriangleSoup.h/.cpp`: OFF (ModelNet の「OFF」直後に数字が続く癖に対応) / OBJ (v と f のみ、負 index と `a/b/c` 形式を許容) の最小リーダ。
- `src\Editor\ModalTools.h/.cpp`: `RunModalVoxelizeCli(list, outDir)` — 1 行 1 パス (`.off/.obj/.gltf/.fbx/builtin://cube` 等)。FBX/glTF は `SubAssetMigration.cpp` と同じヘッドレス登録経路、`builtin://` は `MeshLibrary`。出力 `DIR\<stem>#meshN#primM.mvox`。exit 0 / 1 (入力欠落・読めない)。
- `EditorMain.cpp`: `--modal-voxelize --list F --out DIR` を **else-if 連鎖の外** (`if (arg == L"--modal-voxelize") { ...; continue; }` を `if (arg == L"--selftest")` の手前) で拾う (C1061)。処理本体は `--cook-font-metrics` (EditorMain.cpp:369-380) の隣に「ウィンドウも D3D も作らない」早期 return として置く。
- `Modal/ModalSelfTest.h/.cpp` (sub-05/06 で積み増す)、`tests\deepmodal\list_builtin.txt` (builtin 6 種)。

## やらないこと (このサブでは)
推論、`.msfm`、ライブラリ、Python。

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Modal\Voxelizer.h/.cpp`、`TriangleSoup.h/.cpp`、`ModalSelfTest.h/.cpp`
- 新規 `src\Editor\ModalTools.h/.cpp`、`tests\deepmodal\list_builtin.txt`
- `src\Editor\EditorMain.cpp` (引数: 140-200 付近、本体: 369 付近、selftest 連鎖)
- 参考: `src\Editor\SubAssetMigration.cpp` (ヘッドレス登録)、`GpuResources.cpp:151` `MeshLibrary::Register` (positions/indices/aabb は全メッシュで保持される — 確認済み)、`AcousticGrid` の `CellIndex` (x 最内)
- `.gitattributes` に `*.mvox binary`
- ソース追加後 `pwsh -File tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
spec §5 の 4, 5。
1. ModalSelfTest: (1) 単位立方体 12 三角 → `surfaceCount + interiorCount == 27000`、中心 1、8 隅 0、pad リング全 0 / (2) 厚さ 0.001 の板 → y の占有 index が 1 種類 / (3) 蓋なし箱 → `interiorCount == 0` / (4) 2:1:0.5 の AABB → 最長辺 29 voxel / (5) +X 面中心 → cell (15, 7|8, 7|8) / (6) cellSlot: 有効は自身、無効は独立の総当たりと一致 / (7) `.mvox` 往復 memcmp / (8) OFF/OBJ リーダ (文字列から) / (9) 同入力 2 回 memcmp
2. `cmd /c "bin\x64\Debug\Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out %TEMP%\vox"` → 6 ファイル、exit 0。存在しないパスを含む list → exit 1 とエラー行

## 検証コマンド
- Debug ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 上の cmd
- `pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
