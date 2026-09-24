# sub-04: ボクセル化 + surface nets (開いたメッシュの経路)

- 依存: sub-01 (閉じ判定)。sub-02 / sub-03 と並列可
- 状態: 未着手
- 往復: 0

## やること

spec §4.1 焼き 1 の「`openMeshMode == 1`: ボクセル化を許容」の経路を純関数で作る。

1. **解像度可変のボクセル化**: 入力メッシュ (開いていてよい) の AABB の最長辺を `voxelResolution` (16..256) セルに割った立方セルの格子。外周に 1 セル以上の空きを置く。表面の三角形に触れるセルを占有とし、外側から塗りつぶして「外に繋がらない空洞」を内部として埋める (開いた箱は殻だけが残るので殻の厚み = 1 セル以上の立体になる)
2. **surface nets**: 占有格子から閉じた三角形メッシュを作る (境界は空きで囲んであるので必ず閉じる)。法線は面から、UV は箱投影
3. 出力メッシュが sub-01 の閉じ判定を通ることを関数の中で確かめ、通らなければ失敗を返す
4. **流用しない**: `src/Engine/Engine/Modal/Voxelizer.*` (32³ 固定、Deep-Modal と学習・実行時で共有する契約) は読んでよいが変えない・呼ばない。`TriBoxOverlap` 相当が必要なら別に書くか、共有してよいかを SELF_EVAL で planner に問う (既定: 別に書く)
5. 決定論: 同じ入力で同じバイト列。並列化しない
6. 焼きの入口 (sub-02 の関数) に `openMeshMode` と `voxelResolution` を渡せるようにし、閉じていなければ 0 = 拒否 (理由付き)、1 = このサブの関数で閉じたメッシュにしてから分割、を分岐させる。sub-02 がまだ入っていない場合は入口の配線だけ後のサブに回してよい (その旨を申し送り)

## やらないこと (このサブでは)

- Inspector (sub-09)
- 元の UV を保つこと (ボクセル化経路の外側面の UV は箱投影でよい。制限として spec に書かれている扱い)

## 触る場所 (planner の見立て)

- 新規 `src/Engine/Engine/Physics/FractureVoxel.h/.cpp` (名前は coder 判断)
- sub-02 の焼きの入口
- `FractureSelfTest.cpp` にケース追加
- **触らない**: `src/Engine/Engine/Modal/*`、`tools/deepmodal/*`、`assets/deepmodal/*`

## 受け入れ条件 (このサブ)

1. 蓋のない箱・平面 (quad)・二重の壁を持つメッシュをボクセル化 → surface nets した結果が閉じ判定を通る — `--selftest`
2. 解像度 16 と 64 で出力の三角形数が増え、元の AABB との差がセル 2 個分以内 — `--selftest`
3. 開いた入力を `openMeshMode = 0` で渡すと理由付きで拒否、`= 1` で分割まで通り全破片が幾何的に閉じる (体積 > 0、ベクトル面積の和が表面積の 1e-4 以下。spec 変更 2026-09-25) — `--selftest` (sub-02 が入っていれば)
4. 同じ入力で 2 回のバイト列一致 — `--selftest`
5. 処理時間 (解像度 64 / 128 / 256) を記録 — SELF_EVAL
6. 既存 SelfTest (特に Deep-Modal の Voxelizer 系) と `check_rules.ps1` に変化なし。WIP ファイル不変 — `--selftest`、`check_rules.ps1`、`git status`

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
