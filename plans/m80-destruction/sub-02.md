# sub-02: Voronoi 分割・凸包・接着グラフ・決定論

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること

sub-01 の切断を使って、spec §4.1「焼き」の 2〜8 を純関数として完成させる。入力はメッシュ (溶接前の positions / indices / normals / uvs)、`seed`、`pieceCount`、出力は「破片の列 + 接着グラフ + 焼きの記録」。

1. **内部シード**: `Pcg32` (`src/Engine/Core/Random.h`) を `seed` で初期化し、AABB 内の点を棄却法でメッシュ内部に `pieceCount` 個とる。内外判定は決定的なレイのパリティ (軸平行レイ等。縮退で頂点・辺を通る場合の扱いを固定)。試行回数に上限を置き、足りなければ取れた数で進む (記録に残す)
2. **セル多面体**: 膨らませた AABB の箱を、他のシードとの二等分面で切った凸多面体として作る。メッシュは**セルの面だけで**順に切る (全シード対の平面で切らない。計算量のため)
3. **非連結の分離**: 切った結果を連結成分 (溶接後の三角形の辺連結) に分け、別の破片にする
4. **極小片の統合**: 体積が全破片の平均 × `minVolumeRatio` (既定 0.1) 未満の破片を、接着面積が最大の隣へ統合 (同値は index 小)。統合後の破片は凹んでよい
5. **破片ごとの出力**: 体積重心を原点にした外側面メッシュと蓋メッシュ (別の三角形列)、原点のソース空間位置、体積、`BuildConvexHull` による凸包 (外側 + 蓋の全頂点。頂点 64 上限は既存どおり)
6. **接着グラフ**: 破片 i と j の蓋が同一平面上で重なる面積を隣接 (i,j) の面積とする (同じセル面から生まれた蓋の対なので、面の由来で対応を取れる見込み。方法は coder 判断)。隣接が 32 を超える破片は面積の小さい順に切り捨て、切り捨て数を記録
7. **index の振り方**: 決定的な規則で固定し (例: シード index → 成分の最小溶接頂点 index)、SELF_EVAL と コメントに書く
8. **上限**: 破片 256、隣接 32 (`kMaxFracturePieces` / `kMaxFractureNeighbors` 等の名前付き定数)
9. **digest**: 出力全体を決定的に直列化したバイト列の 64bit ハッシュを返す関数 (SelfTest がログに出し、Debug / Release で比較する)

## やらないこと (このサブでは)

- ファイル形式・MeshLibrary / ConvexColliderLibrary への登録 (sub-03)
- 開いたメッシュのボクセル化 (sub-04。入口で「閉じていない」を返すところまで)
- スキン (骨の割り当ては sub-10。出力に骨の欄を足すのは sub-10 で行う)

## 触る場所 (planner の見立て)

- sub-01 の `FractureMesh.*` の続き、または新規 `src/Engine/Engine/Physics/FractureBake.h/.cpp` (分け方は coder 判断)
- `src/Engine/Engine/Physics/ConvexHull.h` の `BuildConvexHull` (使うだけ。変えない)
- `FractureSelfTest.cpp` にケース追加

## 受け入れ条件 (このサブ)

1. 箱 (pieceCount 8 / 32)・L 字・トーラスで、全破片が閉じ判定を通り、外側面 + 蓋の体積の和が元と相対 1e-4 以内、凸包がすべて `Valid()` — `--selftest`
2. トーラスをまたぐセルなど非連結が出る入力で、成分ごとに別の破片になる (破片数が seed 数より増える) — `--selftest`
3. 極小片の統合が起こる入力 (近接したシード) で、統合後に平均 × 0.1 未満の破片が無い — `--selftest`
4. 接着グラフ: 箱を軸平行に 2 分割した入力で隣接 1 本・面積が断面積と相対 1e-4 以内。各破片の隣接 ≤ 32、対称 (i→j があれば j→i) — `--selftest`
5. 同じ入力で 2 回焼いた digest が一致し、seed を変えると変わる。**Debug と Release の `--selftest` ログに出る digest が一致する** — 両構成の `--selftest` ログ比較
6. 焼き時間を記録 (例: 箱 2,000 三角形 / 32 破片、トーラス 1 万三角形 / 64 破片、256 破片) — SELF_EVAL に数値
7. 既存 SelfTest・`check_rules.ps1` に変化なし。WIP ファイル不変 — `--selftest`、`check_rules.ps1`、`git status`

## 検証コマンド

```
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
（両ログの digest 行を比較）
tools\check_rules.ps1
```

## sub-01 からの申し送り (planner)

- `CapLoops` のコメント (`FractureMesh.cpp:579-582`) を直す: t×b = capNormal なので、外向きの外側面の境界辺から取ったループは外周が常に CW・穴が常に CCW になる。強制揃えは残してよいが、「どちらにもなり得る」という説明は誤り
- 切断 1 回 (1 万三角形) の時間の半分強は `WeldedIds` (全頂点の位置ソート) で、`ExtractBoundaryEdges` と `VerifyCapOrientation` 内の `CheckClosedMesh` でそれぞれ走っている。受け入れ条件 6 の焼き時間が 256 破片で数十秒を超えるなら、溶接結果の使い回し (出力のバイト列を変えない内部最適化) をこのサブで入れてよい。入れたら前後の digest が一致することを確認する

## 実装メモ (coder が追記)

## フィードバック履歴
