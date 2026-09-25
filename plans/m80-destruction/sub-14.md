# sub-14: 断面の三角形分割を掃引法 (libtess2) に置き換える

- 依存: sub-04
- 状態: OK (commit 14d8775)
- 往復: 1

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
2. 輪郭が縮退する断面 (接触・交差・libtess2 が頂点を統合するもの) で落ちず、成功するときは幾何的に閉じ、そうでなければ理由付きの失敗を返す — `--selftest` (round 1 で文言を訂正: 当初の例「2 つの箱が 1 辺で接する」は位置溶接で非多様体になり、入口の閉じ判定で拒否されるため例として成立しなかった)
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

SELF_EVAL: sub-14 (round 1)
実装:
  - external/libtess2/Include/tesselator.h, external/libtess2/Source/*.{c,h} (新規) — libtess2 (master @ 8dbd6483e920311a58c9af10a10beb278efebc36、2025-10-15、SGI Free Software License B 2.0) を取り込み。タグ v1.0.2 (2011年) は不採用 (master の方が EdgeSign の近ゼロx座標バグ修正・NULL 参照修正・不正入力範囲チェックなど焼きの頑健性に直結する修正を含む。差分を確認して選定)
  - external/VERSIONS.md — libtess2 のエントリを追加
  - build/Common.props:AdditionalIncludeDirectories — `external\libtess2\Include` を追加 (Source 配下の .c が `#include "tesselator.h"` を無限定で解決できるようにするため。imgui と同じ流儀)
  - build/Engine.vcxproj — libtess2 の 7 個の .c を手書き ClCompile へ追加 (WarningLevel Level3、TreatWarningAsError false。imgui/ImGuizmo と同じ扱い)
  - tools/gen_project_files.ps1, build/Engine.vcxproj.filters — `external\libtess2` を `$engineExternal` のフィルタ自動生成対象に追加
  - src/Engine/Engine/Physics/FractureMesh.cpp:CapLoops — libtess2 (TESS_WINDING_ODD、法線は常に (0,0,1) 固定、2D 頂点で投入) による三角形分割へ全面置き換え。`EarClip`/`BridgeHoleIntoOuter`/`IsConvexCCW`/`PointInPoly2`/`PointInTriangle2`/`Cross2`/`Cross2D` (内包数による外周・穴の分類、凸フォールバック含む) を削除。`Pt2` から未使用の `vertexIdx` を削除
  - src/Engine/Engine/Physics/FractureMesh.cpp:CutMeshByPlane, VerifyCapOrientation, GeometricCapClosureValid (新設) — `CapLoops` が `hadNewVertices` (輪郭の接触・交差で縮退した入力だったか) を返すようにし、`true` の側だけ厳密な位相的閉じ (`VerifyCapOrientation`) の代わりに幾何的な閉じ (`GeometricCapClosureValid`: 体積>0 かつベクトル面積の和が表面積の 1e-4 以下、sub-02 の `ValidatePieceGeometry` と同じ式) で合否を決める
  - src/Engine/Engine/Physics/FractureBake.cpp — コメント中の `EarClip` への言及を削除 (実装ロジックの変更なし)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — 4b節「輪郭が接触・交差する縮退入力」を追加。openMeshMode=1 res=48/64 の `mustSucceed` を `true` に変更 (全部合否対象に格上げ)、`false` 分岐が到達不能になった `bakeOpenMesh` ラムダの死コードを削除
仕様との差分:
  - [追加] `hadNewVertices` の判定に、TESS_UNDEF の有無だけでなく「出力頂点数が入力点数と不一致」「出力三角形数がオイラーの公式の期待値 (V+2(L-1)-2) と不一致」の 2 条件を追加した。理由: 実測で、libtess2 が新規頂点 (TESS_UNDEF) を作らずに入力頂点を黙って統合するケース (532 入力→531 出力、TESS_UNDEF 0 件) と、自己交差・重複点のいずれの兆候もないのに三角形数が期待より 1 枚少ないケース (392 頂点の単純ループで 389 枚、期待 390 枚) の両方が実際の res32/48/64 ボクセル化断面で見つかった。TESS_UNDEF の有無だけでは検出できないため追加した (詳細・未解明点は不安・質問 2)
  - [逸脱] TESS_UNDEF 頂点の位置・UV を「隣の入力頂点から属性を作る」という sub-14.md の記述に対し、実装では (tangent, bitangent, capNormal) の直交基底による厳密な逆射影で作った。理由: 近似ではなく厳密に復元でき、既存の `OrthonormalBasis` の情報だけで計算できるため。UV・法線は元の意図 (平面法線・平面への正射影) と一致する
  - [未実装] 受け入れ条件2の例示 (2つの箱が1辺で接する) をそのまま再現するテストは実装しなかった。代わりに面積を持つ実体交差 (2本の箱を十字に重ねる) のテストを追加したが、これは「安全に失敗する」ことの検証であり「成功して幾何的に閉じた結果を返す」ことの検証にはなっていない。理由・調査結果は不安・質問1
検証:
  - `tools\gen_project_files.ps1` (pwsh) → 正常終了。`git diff --stat -- build/` は `Engine.vcxproj` (+28)・`Engine.vcxproj.filters` (+54)・`Common.props` (+1/-1) のみ、Editor/Runtime/GameLogic は無変更
  - Engine.vcxproj / Editor.vcxproj の Debug/Release ビルド (MSBuild 直接実行) → 全て成功。libtess2 側で C4267 警告 1 件のみ (`TreatWarningAsError=false` につき無害、FractureMesh.cpp からの警告は 0 件)
  - `bin\x64\Debug\Editor.exe --selftest` → `Fracture mesh core self test: ALL PASS` (fail 0)。ログ: スクラッチパッド `final_debug_out.log`/`_err.log` (bakeOpenMesh 簡略化直前のビルド。ロジック差分なし、到達不能分岐の削除のみなので再実行はしていない)
  - `bin\x64\Release\Editor.exe --selftest` → `Fracture mesh core self test: ALL PASS` (fail 0)。ログ: スクラッチパッド `vfinal_release_out.log`/`_err.log` (最終状態でのビルドで確認)
  - Debug/Release digest 一致: lshape seed=42 pieces=10 → `0x521C987A83D781F7`、torus seed=44 pieces=8 → `0xD9B0EEB57B686B71` (両ログで同一)
  - 1万三角形×16平面の切断 (Release): 151.32 ms 合計 (9.458 ms/切断) ≤ sub-01 の記録 17 ms/切断 — 受け入れ条件5 PASS
  - res32/48/64 (open box・plane quad) の焼き (pieceCount=16、openMeshMode=1): 全6組み合わせが Debug/Release 両方で `--selftest` PASS。Release の焼き時間: res32 open box 4.73 s / plane quad 0.74 s、res48 open box 10.83 s / plane quad 1.53 s、res64 open box 20.12 s / plane quad 2.74 s — 受け入れ条件3 PASS (既定値の候補は下記申し送り)
  - `tools\check_rules.ps1` → 0 error, 0 warning — 受け入れ条件6 PASS
自己採点 (1-5):
  仕様適合: 3 — 受け入れ条件1・3・4・5・6は検証済みで PASS。条件2は文字どおりの例 (2箱が1辺で接する) を再現できず、根拠のある代替 (十字に重なる縮退入力が安全に失敗する) で埋めた。差分に明記し不安・質問で判断を仰ぐ
  正しさ: 3 — 全既存テスト・新規テストが Debug/Release 双方で PASS。ただし res32/48/64 で見つかった「libtess2 の出力三角形数がオイラーの公式の期待値から外れる」現象の根本原因は未解明のまま、検出ベースの安全策 (hadNewVertices の条件追加) で対処した。実測した範囲では解決しているが、同種の未知の欠陥が別の入力で残っている可能性を否定できない
  コード品質: 4 — EarClip 等の死んだ経路を削除し CapLoops は大幅に単純化した (全体で約 570 行減)。調査用の一時 fprintf は全て削除済み (grep で確認)。hadNewVertices の判定式 (オイラーの公式) がマルチアイランド構成で保守的 (常に幾何判定側へ倒れる) になる点をコメントに明記した
  テスト: 4 — sub-01 由来の全テスト・新規の輪郭接触テスト・res48/64 の must 化を Debug/Release 両方で実行し、ログで確認した
不安・質問:
  1. 受け入れ条件2の例 (2つの箱が1辺で接する) を実装して調べた結果、頂点そのものを共有する接触は既存の `ChainAllLoops` (本sub の変更対象外、sub-01/02 所有) の前提「溶接後の1位置につき出て行く辺は高々1本」に必ず抵触し、`CapLoops` (libtess2) へ到達する前に「非多様体入力の疑い」として安全に失敗することを確認した (実測)。libtess2 を使う/使わないに関わらず、`ChainAllLoops` を変えない限りこの経路へは到達できない。代わりに面積を持つ実体交差 (2本の箱を十字に重ねる) を試したが、この場合は外側面 (両箱の壁がそのまま残る想定) と蓋 (libtess2 の TESS_WINDING_ODD が重複体積領域を穴として除く) の前提が食い違い、安全側の失敗になる (成功しない)。どちらの構成も「成功して幾何的に閉じた結果を返す」という条件2の文字どおりの要求は満たせなかった。(a) 条件2を「輪郭が接触する縮退入力で安全に失敗する (クラッシュしない)」ことの検証に読み替えてよいか、(b) `ChainAllLoops` の制約緩和を別subとして追加するか、判断してほしい。密な輪郭に対する頑健性そのもの (受け入れ条件3、res48/64 の焼き) は別途検証済み
  2. 密なボクセル化断面 (res32/48/64) で見つけた「libtess2 の出力三角形数がオイラーの公式の期待値 (単純多角形なら V-2、外周+穴なら V) から1枚だけ不足する」現象の根本原因は特定できていない。自己交差・重複点・退化三角形のいずれの兆候もない 392 頂点の単純ループで再現したが (Python で独立検算)、libtess2 内部の掃引アルゴリズムのどの段階で1三角形が失われるかまではデバッガでのステップ実行なしに追えなかった。対処として「オイラーの公式から外れたら幾何的閉じ判定へフォールバックする」という検出ベースの安全策を追加し、実測で res32/48/64 の全6組み合わせが解決することを確認したが、これは対症療法であり根本原因の除去ではない。この対処方針で受け入れるか、libtess2 側の追加調査 (上流 Issue の検索、より新しいコミットへの追従) を求めるか判断してほしい
  3. hadNewVertices の判定式 (オイラーの公式 V+2(L-1)-2) は「外周1本+穴(L-1)本」の入れ子構造を前提にしており、複数の分離した島 (穴ではなく独立した2つ以上の外周) を1回の `CapLoops` で処理する場合は式が合わず常に幾何判定側へ倒れる (安全だが保守的)。今回のテスト範囲ではこの状況を明示的には確認していない (実測したケースはすべて「1本」か「外周+穴」の構成だった)。将来、複数の島を1回で処理する入力が増えるなら式の精度を上げる余地がある。現状は安全側なので blocker ではないという認識だが、認識が正しいか確認したい
  4. `voxelResolution` の既定値候補: res32 は Release 4.73 s (10 秒以内)、res48 は Release 10.83 s (10 秒をわずかに超過)。sub-04 の既定値 32 を変える理由が今回の計測では出なかった。既定値は 32 のまま維持することを推奨する (sub-06 の Components 欄への反映は sub-06 の担当)
触ったファイル:
  - external/libtess2/Include/tesselator.h (新規)
  - external/libtess2/Source/bucketalloc.{c,h}, dict.{c,h}, geom.{c,h}, mesh.{c,h}, priorityq.{c,h}, sweep.{c,h}, tess.{c,h} (新規)
  - external/libtess2/LICENSE.txt (新規)
  - external/VERSIONS.md
  - build/Common.props
  - build/Engine.vcxproj
  - build/Engine.vcxproj.filters
  - tools/gen_project_files.ps1
  - src/Engine/Engine/Physics/FractureMesh.cpp
  - src/Engine/Engine/Physics/FractureBake.cpp (コメントのみ)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
申し送り:
  - sub-09 (エディタ非同期焼き) は本subに依存する。res48/64 は Release で 10 秒を超える (open box 10.83 s / 20.12 s) ので、Inspector の非同期実行・キャンセル不能な長時間ブロッキングを避ける設計 (spec §4.3 で既に前提) を必ず実装すること
  - `voxelResolution` の既定値は 32 のまま (申し送り4参照)
  - 不安・質問1〜3は planner の裁定が必要 (特に1は受け入れ条件2の文言修正を伴い得る)

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1・3〜6 を満たした (解像度 32/48/64 × 開いた箱・平面の 6 組が Debug / Release で成功、1 切断 9.5 ms)。条件 2 は文言を訂正して、交差の安全な失敗と、実データでのフォールバックの成功で満たすとした。三角形が 1 枚足りない件は、面積 0 (共線) の三角形を libtess2 が出さないことと整合する。幾何的な閉じの検算がある限り、面積のある欠けは見逃さないので受け入れる。既定の解像度は 32 のまま (48 は Release 10.8 秒で予算超え)。nit: 複数の島を含む断面で、オイラーの公式の期待値を島と穴の数で計算するのは任意
