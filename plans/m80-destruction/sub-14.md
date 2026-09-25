# sub-14: 断面の三角形分割を掃引法 (libtess2) に置き換える

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## 出所

断面 (蓋) の三角形分割 `EarClip` (`src/Engine/Engine/Physics/FractureMesh.cpp`) の失敗が、同じ種類で 3 回出た:
- sub-01: 穴の橋渡しで詰まる
- sub-02: 閉じていない入力を渡していたことが主因だが、耳切りも破綻していた
- sub-04: ボクセル化した開いた箱で、解像度 48 / 64 のとき失敗。Release で 12.5 秒 / 24.2 秒と遅い。共線点の対策と surface nets を入れた後も残った

耳切りは次の 3 点で弱い。局所の対症療法を積み増すのをやめ、方式を置き換える:
- **O(n³)** で遅い
- **単純多角形を前提にしている**: 薄い壁の断面のように、輪郭どうしが接したり重なったりすると破綻する
- 穴の橋渡しの自前実装がある

「複雑な素材まで対象」(ユーザー要件) を満たすには、高ポリゴンの実モデルの断面でも焼けることが要る。

## 方式 (planner 裁定、`[ユーザーに聞ける]` #11)

**libtess2** (GLU tessellator の後継。SGI Free Software License B 2.0 = MIT 相当の許諾) を `external/libtess2/` に取り込み、断面の三角形分割に使う。
- 掃引線法、O(n log n)
- 輪郭の自己交差・接触・重なり・穴を、巻き数の規則 (odd / nonzero) で正しく扱う
- 乱数を使わない。演算順は入力順で固定

却下案:
- 制約付き Delaunay (CDT) の自作 / 取り込み: 制約辺の交差を前提にしない実装が多く、接触・重なりには別の前処理が要る
- 耳切りの改良 (反射頂点の索引、z-order): 速くはなるが、単純多角形の前提は残る
- 解像度 32 で打ち止め: 高ポリの実メッシュで同じ失敗が出る

## やること

1. `external/libtess2/` に取り込む (上流の版とコミットを `external/VERSIONS.md` に記録。ライセンス文を同梱)。ビルドは Engine プロジェクトの一部として、エンジンと同じコンパイラ設定 (`/fp:precise`) でコンパイルする (`tools\gen_project_files.ps1` が external の扱いをどうしているか確認し、既存の external と同じ流儀で)
2. `CapLoops` の三角形分割を libtess2 に置き換える: 全ループを輪郭として渡し、`TESS_WINDING_ODD` (内包数の偶奇。今の外周 / 穴の判定と同じ意味) で三角形を得る。出力の頂点は `tessGetVertexIndices` で入力頂点へ戻す。`TESS_UNDEF` (交差で新しくできた頂点) は、隣の入力頂点から属性 (法線 = 平面法線、UV = 平面への正射影) を作る
3. **閉じの検算の扱い**: 輪郭が正常な入力 (新しい頂点が 0) では、今までどおり外側面 + 蓋が厳密に閉じる (sub-01 の受け入れ条件を保つ)。新しい頂点が出たとき (輪郭が接触・交差した縮退入力) は、厳密な閉じの代わりに幾何的な閉じ (ベクトル面積の和 ≤ 表面積 × 1e-4、sub-02 と同じ判定) で合否を決める。破片はもともと幾何的な閉じで判定しているので (spec §4.1 焼き 3)、下流への影響はない
4. 不要になった `EarClip` / `BridgeHoleIntoOuter` / 内包数による外周 / 穴の分類 / 凸フォールバックを削除する (使われないコードを残さない)
5. **受け入れ条件 8 の回収**: sub-04 で記録のみにした「開いた箱の解像度 48 / 64 の焼き」を合否に戻す。解像度 32 / 48 / 64 の焼き時間を計測し直し、Release で 10 秒以内に焼ける最大の解像度を既定値の候補として SELF_EVAL に書く (Components の欄は sub-06 で反映)
6. 決定性: 同じ入力で同じ出力になること、Debug / Release で digest が一致すること。libtess2 の内部でハッシュやポインタの順序に依存する箇所が無いかを確認する (priority queue の同値の扱いなど)。あれば SELF_EVAL に書く

## やらないこと (このサブでは)

- 焼きの他の段階の最適化 (sub-11)
- libtess2 の改変 (必要なら最小限に留め、`VERSIONS.md` に差分を記録)

## 触る場所 (planner の見立て)

- 新規 `external/libtess2/` と `external/VERSIONS.md`
- `src/Engine/Engine/Physics/FractureMesh.cpp` (`CapLoops` と周辺の削除)
- `tools\gen_project_files.ps1` の external の扱い (必要なら)、`build/Engine.vcxproj(.filters)`
- `FractureSelfTest.cpp`
- **触らない**: WIP ファイル

## 受け入れ条件 (このサブ)

1. sub-01 の全テスト (閉じ判定・平面切断・穴あき蓋・エッジケース・決定論) が PASS。正常な入力では外側面 + 蓋が厳密に閉じる — `--selftest`
2. 輪郭が接触する断面 (例: 2 つの箱が 1 辺で接する閉じたメッシュを、その辺を通る平面で切る) で、落ちずに幾何的に閉じた結果を返す — `--selftest`
3. 開いた箱と平面を `openMeshMode = 1`・`pieceCount = 16` で、解像度 32 / 48 / 64 の**全部**について焼けて成功する。Release の時間を記録し、既定値の候補を出す — `--selftest` + SELF_EVAL
4. 箱・L 字・トーラスの焼きの digest が Debug / Release で一致する (値は変わってよい) — 両構成の `--selftest` ログ
5. 1 万三角形を 16 平面で切る時間が sub-01 の記録 (Release 17 ms / 切断) 以下 — SELF_EVAL
6. `check_rules.ps1` PASS (external の扱いが規則に引っかからないこと)、WIP 不変

## 検証コマンド

```
tools\gen_project_files.ps1   (pwsh で実行)
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

## フィードバック履歴
