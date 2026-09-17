# sub-02 (M76b): ボクセライザと --modal-voxelize

- 依存: なし (sub-01 と並列可。`ModalTypes.h` の定数を両方が足す場合は先に入った方に合わせる)
- 状態: OK (commit ae77b20)
- 往復: 0

## やること
spec §4.1「ボクセル化」と §4.2 `.mvox`。学習とランタイムが共有する**唯一の**ボクセル化実装 (ユーザー計画 Phase 2 / Checkpoint B)。

- `Modal/Voxelizer.h/.cpp`: `VoxelFrame { origin[3]; voxelSize; aabbMin[3]; aabbMax[3]; longestEdge; }`、
  `VoxelGrid { frame; std::array<uint8_t, 32768> occ; surfaceCount; interiorCount; }`、
  `bool VoxelizeMesh(const XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m, VoxelGrid& out)`、
  `TriBoxOverlap` (Akenine-Möller 13 軸、箱半径 `h/2 + 1e-6h`、退化三角形も辺として拾う)、`FloodFillInterior` (pad リングから 6 近傍、明示スタック、固定順)、
  `LocalPointToCell(const VoxelFrame&, const float p[3]) -> uint16_t`、`BuildCellSlotTable(const VoxelGrid&, uint16_t cellSlot[4096])` (無効 cell は焼き時に総当たりで最寄り有効 cell、同値は小 index)、
  `SerializeVox` / `DeserializeVox` (72 B ヘッダ = 18 フィールド × 4 B、フィールド単位で書く。round 1 で 64 B → 72 B に訂正)。
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
1. ModalSelfTest: (1) 単位立方体 12 三角 → `surfaceCount + interiorCount == 24389` (29³)、中心 1、8 隅 0、pad リング (index 0 と 31) 全 0 / (2) 厚さ 0.001 の板 → y の占有 index が**ちょうど 1 種類** (index 16) / (3) 蓋なし箱 → `interiorCount == 0` / (4) 2:1:0.5 の AABB → 最長辺の占有 voxel 数が 29 (`h = L/28`、`frame.longestEdge / voxelSize == 28`) / (5) +X 面中心 → cell **(15, 8, 8)** に一意 / (6) cellSlot: 有効は自身、無効は独立の総当たりと一致 / (7) `.mvox` 往復 memcmp (ヘッダ 72 B + 32768 B) / (8) OFF/OBJ リーダ (文字列から) / (9) 同入力 2 回 memcmp
   ★round 1 で正規化を `h = L/28, origin = center − 16.5h` に変えた (spec §8)。面は voxel 中心 2.5 / 30.5 を通り、AABB 中心は voxel 16 の中心 (16.5) に乗る。旧式 (L/29, 16h) は中心が 15/16 の境界に乗り、板が必ず 2 行になっていた
2. `cmd /c "bin\x64\Debug\Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out %TEMP%\vox"` → 6 ファイル、exit 0。存在しないパスを含む list → exit 1 とエラー行

## 検証コマンド
- Debug ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 上の cmd
- `pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL)

SELF_EVAL: sub-02 (round 1)
実装:
  - `src/Engine/Engine/Modal/Voxelizer.h/.cpp` — `VoxelFrame`/`VoxelGrid`/`VoxelIndexOf`/`CellIndexOf`、
    `VoxelizeMesh` (AABB→origin/h → SAT 表面 → flood-fill 内部充填)、`TriBoxOverlap`
    (Akenine-Möller 13 軸、辺×箱軸 9 + AABB 3 + 面法線 1 を関数分解、マクロ不使用)、
    `FloodFillInterior` (pad リングから 6 近傍、明示 `std::vector` スタック)、`LocalPointToCell`、
    `BuildCellSlotTable` (総当たり、同値は index 最小)、`SerializeVox`/`DeserializeVox`
    (フィールド単位で書く。struct を memcpy しない)
  - `src/Engine/Engine/Modal/TriangleSoup.h/.cpp` — `LoadOffText` (ModelNet の "OFF8 12 0" glued
    ヘッダ対応、ファン三角形分割)、`LoadObjText` (v/f のみ、負 index・"i/j/k" 形式・ファン分割)
  - `src/Engine/Engine/Modal/ModalSelfTest.h/.cpp` — 受け入れ条件 (1)〜(9) 相当の 21 チェック
    (単位立方体 27000 / 中心 1 / 8 隅 0 / pad 0、薄板、蓋なし箱、2:1:0.5 の longestEdge、
    +X 面中心セル、cellSlot の独立総当たり照合、.mvox 往復 memcmp、OFF/OBJ (glued header 含む)、
    決定論 2 回一致)
  - `src/Editor/ModalTools.h/.cpp` — `RunModalVoxelizeCli`。builtin:// は `MeshLibrary` の
    `Cube()/Sphere()/.../Capsule()` を `Init()` 無しで直接呼ぶ (`Register` は `device_==nullptr`
    を許容する設計 — GraphicsDevice を 1 つも作らない)。.off/.obj は `TriangleSoup`、.fbx/.gltf/.glb
    は `FbxLoader::RegisterAssets`/`ModelLoader::RegisterAssets` のヘッドレス登録
    (`SubAssetMigration.cpp` と同経路、resolver は未インストール = 複数メッシュの命名は
    path-hash 由来のフォールバックキーに落ちるが `.mvox` の出力名には影響しない)。
    複数メッシュ/パーツは登録前後の `Enumerate()` 差分で検出し、登録名の
    `#mesh<N>#(part|prim)<M>` を解析して出力ファイル名 `<stem>#mesh<N>#prim<M>.mvox` に反映
    (解析できなければ 0/0 にフォールバック)。存在しない入力・読めないモデル・未対応拡張子は
    1 行ごとにエラーを出して続行し、1 件でもあれば exit 1
  - `src/Editor/EditorMain.cpp` — `--modal-voxelize`/`--list`/`--out` を else-if 連鎖の**手前**
    (C1061 回避、`--cook-font-metrics` と同型) で拾って `continue`、本体呼び出しは
    `--migrate-subasset-ids` の直後・`if (selftest)` の手前に追加。selftest 連鎖の末尾
    (`RunModalSynthSelfTest()` の直後) に `RunModalSelfTest()` を追加
  - `tests/deepmodal/list_builtin.txt` — builtin 6 種 (cube/sphere/plane/quad/cylinder/capsule)
  - `.gitattributes` — `*.mvox binary` を追加 (golden PNG と同じ理由。改行変換で壊れると
    「読めない」が理由不明で出る)
  - `tools/gen_project_files.ps1` を実行し `build/Engine.vcxproj(.filters)` /
    `build/Editor.vcxproj(.filters)` を再生成
仕様との差分:
  - [逸脱] `.mvox` ヘッダの実バイト数は 72 B (spec/design-draft の「64 B」注記どおりではない)。
    列挙されているフィールド (magic/version/n/pad/origin[3]/voxelSize/aabbMin[3]/aabbMax[3]/
    longestEdge/surfaceCount/interiorCount/reserved = 18 個) を Material の暗黙パディング事故と
    同じ理由で **全部 4 バイト境界の素直な型**にすると 18×4=72 B になる。`longestEdge` は
    design-draft (64B 表記の版) には無く spec で追加されているので、追加前の版数と揃っていない
    のが原因と見ている。ビット詰め (uint8 の version/n 等) で 64B ぴったりに収める案も検討したが、
    `DmNetHeader` 等の既存プロジェクト規約 (常に 4 バイト整列、ビット詰めをしない) から外れるため
    採らなかった。受け入れ条件に「ヘッダは正確に 64B」という直接テストは無く、往復 memcmp のみが
    要求なので実害は無い。`kVoxHeaderBytes = 72` として `Voxelizer.h` に明記、コメントで理由を残した
  - [追加] 受け入れ条件 (2)「厚さ 0.001 の板 → y の占有 index が 1 種類」について、実装・実測の
    結果は**隣接する 2 個 (index 15, 16)** になる。理由: `origin = center - 16h` により、
    メッシュ自身の AABB 中心は**必ず**連続 voxel 座標 16.0 (= voxel index 15/16 の境界そのもの) に
    一致する構造的な不変量があり、中心対称な薄い特徴はこの境界を跨ぐ。保守的マージン
    (`h/2 + 1e-6h`) はこの境界を両側から拾うために付けてあるので、これは**バグではなく意図した
    保守的表面判定の帰結**——受け入れ条件 (5) の「7|8」表記が同じ境界不定を明示的に許容している
    のと同型の現象(実際に (5) は同じ 2:1:0.5 のボックスで検証し、Y/Z セルが 7 か 8 かは
    どちらでも合格にしている)。デバッグログを一時的に足して実測 `{15, 16}` (隣接 2 個) と確認済み
    (ログは削除済み、コミットには残していない)。テストは「1 個、または隣接する 2 個」を合格とする
    形で実装した。**この解釈に確信が持てない場合は「不安・質問」を参照**
  - [追加] `RunModalVoxelizeCli` の FBX/glTF/glb 複数メッシュの出力ファイル名は、登録名の
    `#mesh<N>#(part|prim)<M>` を解析した実番号を使う (spec の `<stem>#meshN#primM.mvox` に
    忠実だが、この解析ロジック自体は spec に明記が無いので実装で決めた)。受け入れ条件に
    多メッシュ FBX/glTF の直接テストは無い (list_builtin.txt は builtin のみ) ので、
    OFF/OBJ (mesh0/prim0 固定) と builtin (同) の経路だけを自動テスト・手動 CLI で検証済み。
    FBX/glTF 経路はコンパイルと呼び出し規約 (SubAssetMigration.cpp と同型のヘッドレス登録) の
    正しさまでで、実データでの動作は sub-03 (Python 側が実際に呼ぶ) で初めて実地検証される
検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` →
    成功・警告 0 (既知の LNK4204 imgui pdb 警告のみ)
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → exit 0、ALL PASS
    (`ModalSynth self test` / `Modal (Voxelizer) self test` を含む全スイート)
  - `cmd /c "bin\x64\Debug\Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out %TEMP%\vox"`
    → exit 0、6 ファイル (`*.mvox`、各 32840 B = 72 + 32768)
  - 存在しないパスを 1 行加えた list → exit 1 + `[modal-voxelize] ERROR: input not found: ...` を確認
  - list ファイル自体が存在しない → exit 1 + エラー行を確認
  - 手書き OBJ (単位立方体、v/f のみ) を CLI に通し、builtin cube と同じ
    `surface=5048 interior=21952` (計 27000) を確認 (OBJ 経路と builtin 経路が同じ結果になることの
    交差検証)
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` は未実行 (sub-02.md の検証コマンド欄に無い。sim / ECS / .rep 形式に
    一切触れていないため対象外と判断)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 9 項目のうち 7 項目は文字どおり検証、2 項目 (.mvox の 64B 表記、
    薄板の「1 種類」表記) は上記の差分どおり実装と一致しないが、理由を明記し検証も添えた
  正しさ: 5 — 全チェック (selftest 21 件・CLI 3 パターン・OBJ/builtin 交差検証) を実機で実行し
    観測した。薄板の挙動は一時デバッグログで実測値 (行 15,16) まで確認してから assertion を書いた
  コード品質: 4 — 日本語コメント・4 スペース・`{` 次行・宣言時初期化を徹底。TriBoxOverlap はマクロ
    でなく関数分解 (可読性優先)。ModalTools.cpp の行ディスパッチはやや長い (builtin/off-obj/fbx-gltf
    の 3 分岐) が、共通処理は `VoxelizeAndWrite`/`WriteMvox` に括り出し済み
  テスト: 5 — Voxelizer の全公開関数 (VoxelizeMesh/TriBoxOverlap/FloodFillInterior/
    LocalPointToCell/BuildCellSlotTable/SerializeVox/DeserializeVox) と TriangleSoup の両リーダを
    自動テストで、CLI 経路 (成功/エラー/OBJ) を手動 cmd で確認した
不安・質問:
  - 受け入れ条件 (2) の「y の占有 index が 1 種類」は、上記の構造的な理由 (AABB 中心が必ず
    voxel 境界 16.0 に一致する) により厳密には満たせないと判断し、「1 個または隣接 2 個」を
    合格とするテストにした。この解釈で良いか、それとも `origin` の定数 (16h) や halfBox のマージン
    式を変えて「常に 1 個」になるよう仕様側を調整すべきか、planner の判断を仰ぎたい
    (後者に変えると単位立方体の 27000 判定や (5) の「7|8」許容にも波及するため、
    軽々に coder 側だけで変えなかった)
  - `.mvox` ヘッダの「64 B」表記 (spec §4.2 と design-draft 双方) を「72 B」で実装した。
    spec.md 側の記述を 72 B に修正するか、あるいは意図された削減方法 (どのフィールドを削るか)
    があれば教えてほしい
触ったファイル:
  - src/Editor/EditorMain.cpp
  - src/Editor/ModalTools.h
  - src/Editor/ModalTools.cpp
  - src/Engine/Engine/Modal/TriangleSoup.h
  - src/Engine/Engine/Modal/TriangleSoup.cpp
  - src/Engine/Engine/Modal/Voxelizer.h
  - src/Engine/Engine/Modal/Voxelizer.cpp
  - src/Engine/Engine/Modal/ModalSelfTest.h
  - src/Engine/Engine/Modal/ModalSelfTest.cpp
  - tests/deepmodal/list_builtin.txt
  - .gitattributes
  - build/Editor.vcxproj
  - build/Editor.vcxproj.filters
  - build/Engine.vcxproj
  - build/Engine.vcxproj.filters
申し送り:
  - sub-03 (Python) が `--modal-voxelize` を subprocess で叩くとき、FBX/glTF の複数メッシュ命名
    (`#meshN#primM`) は実データで初めて検証されることになる。ModelNet は OFF 単一メッシュのみの
    はずなので実際に踏む経路は OFF が主になる見込みだが、もし三校 / HAL Collector のような
    FBX 資産を後段で voxelize する計画があるなら、そのタイミングで多メッシュ名の実地確認を
    足してほしい
  - `RenderResources`/`ShaderManager` をヘッドレスで使う都合上、CLI 1 回の実行内で複数の
    builtin/OFF/OBJ/FBX/glTF が混在した list を渡しても動く設計にしてある (`resources`/`shaders`
    をループの外で 1 つだけ生成)。Python 側は 1 回の subprocess 呼び出しに大量のパスをまとめて
    渡してよい (「バッチ 200 件」という design-draft の記述と整合)

### round 2 (SELF_EVAL、FIX_REQUEST round 2 対応)

spec.md が round 1 の不安・質問 #1/#2 を受けて先に改訂されている (§4.1 / §4.2 / §5 #4・#6 / §8)。
読み直した上で以下を実装した。

SELF_EVAL: sub-02 (round 2)
実装:
  - #1 [must] `src/Engine/Engine/Modal/Voxelizer.cpp` の正規化式を修正:
    `h = longestEdge / 29.0f` → `h = longestEdge / 28.0f`、
    `origin[axis] = center[axis] - 16.0f * h` → `center[axis] - 16.5f * h` (3 軸とも)。
    コメントも「奇数 29 で割ると中心が必ず voxel 境界に乗る」→「偶数 28 + 半 voxel オフセットで
    AABB 中心も voxel 16 の中心 (16.5) に乗る」に書き換え。`Voxelizer.h` の `VoxelFrame` 直上の
    説明コメントも同様に更新 (旧: `L/29`・`1.5..30.5`・`27000`、新: `L/28`・`2.5..30.5`・`24389`)。
    `ModalSelfTest.cpp` の期待値を全面更新:
      - (1) 単位立方体 `surfaceCount + interiorCount == 24389` (29^3)、中心 voxel (16,16,16) は
        そのまま (voxel 16 の中心 16.5 は依然として voxel index 16 の範囲内)、8 隅・pad リング
        (index 0/31) は式の変更に依らず不変なので条件そのまま
      - (2) 薄板テストを `rows.size() == 1 && *rows.begin() == 16` の厳密一致に変更
        (「1 個または隣接 2 個」の許容を撤廃)
      - (4)(5) `longestEdge/voxelSize == 28.0f` の比較に変更。**新規**に「最長 (X) 軸の占有 voxel
        数 == 29」を `std::set` で数えて追加 (指摘の「最長辺の占有 voxel 数 29」は round 1 では
        比率のみの間接確認だったので、実際に軸方向の占有本数を数える形に直した)。
        `+X` 面中心のセルは `cx==15 && cy==8 && cz==8` の `&&` 一致に変更 (「7|8」の不定を撤廃)
  - #2 [should] `Voxelizer.h` の `kVoxHeaderBytes` 直上コメントを「spec の 64B 注記は古い」から
    「ヘッダの実バイト数は kVoxHeaderBytes = 72 B (18 フィールド × 4 B、spec §4.2 が正本)」に
    書き換え (spec.md 側が 72 B に訂正されたので、コード側は「spec と食い違う」ではなく
    「spec のとおり」という説明に変更)
  - #3 [nit] 対応不要 (sub-03 の申し送りに残す指示のみ。sub-02 側のコード変更は無し)
仕様変更: あり (spec.md が round 1 の指摘を受けて先に改訂済み。§4.1 / §4.2 / §5 #4・#6 / §8)。
  読み直した上で実装をそちらに合わせた (上記の通り)
検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` →
    成功・警告 0
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → exit 0、ALL PASS。
    `Modal (Voxelizer) self test` の全 21 行を目視確認:
    「29^3 = 24389」「exactly 1 voxel row (index 16)」「h = L/28」
    「the longest (X) axis occupies exactly 29 voxels」「uniquely to cell (15, 8, 8)」の
    5 行が新しい期待値どおりに PASS していることを確認
  - `cmd /c "bin\x64\Debug\Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out %TEMP%\vox"`
    → exit 0、6 ファイル (各 32840 B 変わらず)。builtin cube = `surface=4706 interior=19683`
    (合計 24389、新しい単位立方体の値と一致)
  - 手書き OBJ 立方体を CLI に通し、builtin cube と同じ `surface=4706 interior=19683` (24389) を
    再確認 (交差検証)
  - 存在しないパス混在 list → exit 1 + ERROR 行、list 自体が無い → exit 1 を再確認 (回帰なし)
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
自己採点 (1-5):
  仕様適合: 5 — round 1 で残った 2 件の差分はどちらも planner が仕様を訂正し、コードをそれに
    追従させた。現時点でコードと spec.md (§4.1/§4.2/§5) の間に既知の食い違いは無い
  正しさ: 5 — 新しい期待値 (24389 / 1 行 / h=L/28 / 29 voxel / (15,8,8) 一意) を実機の selftest
    と手動 CLI の両方で観測し、全て一致を確認した
  コード品質: 5 — 変更は定数 2 箇所 + コメント + テスト期待値の更新のみで、他ロジック
    (SAT・flood-fill・cellSlot・シリアライズ) は無変更。差分は最小
  テスト: 5 — 指摘が要求した 5 つの期待値変更 (24389 / ちょうど1 / h=L/28 かつ29 / (15,8,8) 一意)
    を全部テストへ反映し、実行して確認した
不安・質問: なし
触ったファイル:
  - src/Engine/Engine/Modal/Voxelizer.h
  - src/Engine/Engine/Modal/Voxelizer.cpp
  - src/Engine/Engine/Modal/ModalSelfTest.cpp
申し送り: なし (#3 の申し送りは round 1 の内容のまま sub-03 へ引き継ぎ)

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-16)。coder の「不安・質問」2 件はどちらも**仕様側の誤り**と判定し、先に spec / sub を直した (spec §8 の 2 行)。(1) 正規化 `h = L/29, origin = center − 16h` は AABB 中心が voxel 15/16 の境界に乗る構造 (Voxelizer.cpp:187-201 で確認。奇数個に割ると必ずそうなる) で、薄板が 2 行になるのは coder の診断どおり。ただし「1 個または隣接 2 個」で許容するのではなく、**`h = L/28, origin = center − 16.5h`** に変える (面は voxel 中心 2.5 / 30.5 を通り、中心も voxel 16 の中心に乗る = 板は 1 行、+X 面中心は (15, 8, 8) に一意)。数値: 単位立方体 24389、最長辺占有 29、`longestEdge / voxelSize == 28`。データセット未生成の今しか変えられない。(2) `.mvox` ヘッダ 72 B は正しい (planner の計算違い)。それ以外 — SAT / flood-fill / cellSlot / .mvox のフィールド単位書き / OFF glued ヘッダ / EditorMain の連鎖手前 hook / CLI の exit 規約 — は差分と実行結果を確認して問題なし。replay_verify 未実行は sub-02 の検証欄に無く sim 非接触なので可。
- round 2: **VERDICT: OK** (planner、2026-09-16)。planner が独立に確認: Voxelizer.cpp:187-201 が `h = L/28`、`origin = center − 16.5h` (3 軸)、ModalSelfTest.cpp:144/193/228/235 が 24389 / `rows.size()==1 && *rows.begin()==16` / X 軸占有 29 / `cx==15 && cy==8 && cz==8` の `==` 断言。`Editor.exe --selftest` exit 0 (`Modal (Voxelizer) self test: ALL PASS`)、`--modal-voxelize --list tests\deepmodal\list_builtin.txt` exit 0 で 6 本 × 32840 B。cube surface 4706 + interior 19683 = 24389 (interior 27³ = 29³ の殻抜きと整合)、**plane / quad が surface 841 = 29² = 厚さ 0 の面がちょうど 1 行** — 新式の狙いが builtin で裏付いた。Voxelizer.h:80 のコメントは 72 B / spec §4.2 正本に書き換え済み。
