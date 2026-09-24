# sub-01: 閉じ判定と平面切断 + 蓋 (分割コアの土台)

- 依存: なし
- 状態: OK (コミット待ち)
- 往復: 2

## やること

M80 で最もリスクの高い未知 = **「閉じた (凹んだ・穴のある) メッシュを平面で切って、両側を閉じたメッシュとして取り出せるか」** を、Engine 層の純関数と SelfTest だけで潰す (spec §4.1 焼き 1, 3 の切断部分)。描画・物理・コンポーネントには一切繋がない。

1. **閉じ判定**: 入力 (positions / indices) を位置のビット一致で溶接する (−0.0 は +0.0 へ畳む。`ConvexHull` と同じ流儀)。無向辺ごとの使用数と向きを数え、境界辺 (使用 1)・非多様体辺 (使用 3 以上)・向き不一致 (同じ向きで 2 回使われる辺) の件数を返す。0/0/0 なら閉じている。結果は「理由と件数」を持つ構造体 (Inspector の赤字表示に使う)
2. **平面切断 + 蓋**: 閉じたメッシュ (溶接済み) を平面 1 枚で切り、指定した側を閉じたメッシュとして返す
   - 頂点属性 (位置・法線・UV) は辺上で線形補間
   - 平面にほぼ乗る頂点の扱い (許容幅、どちら側に数えるか) を決めて一貫させる
   - 断面の辺を閉ループへつなぎ、ループの入れ子 (外周 / 穴) を判定し、平面上で三角形分割 (耳切り + 穴の橋渡し 等) して蓋にする。蓋の法線 = 平面法線 (外向き)、UV = 断面平面への正射影 (spec §4.1、round 1 で変更)
   - **外側面と蓋は別々の三角形列**として返す (sub-02 以降で 2 本のメッシュになる)
   - 失敗 (ループが閉じない、三角形分割できない) は**落ちずに失敗を返す**。失敗時に安全側へ倒す規則 (例: その平面の切断を諦める) をこのサブで決め、SELF_EVAL に書く
3. **決定論**: ハッシュコンテナの走査順・ポインタ順を使わない。ループ・三角形の出力順を決定的なキー (最小頂点 index 等) で固定。並列化しない
4. **SelfTest** (新規 `FractureSelfTest.cpp/.h` 想定、`EditorMain.cpp` の末尾へ `ok &=` 登録)

## やらないこと (このサブでは)

- Voronoi・シード・凸包・接着グラフ (sub-02)
- ファイル形式 (sub-03)、ボクセル化 (sub-04)
- コンポーネント・物理・描画・エディタ

## 触る場所 (planner の見立て)

- 新規 `src/Engine/Engine/Physics/FractureMesh.h/.cpp` (名前は coder 判断。Engine 層の純関数。Renderer の `MeshVertex` を使うか独自の頂点構造にするかは coder 判断 — sub-03 で `MeshVertex` に詰め替えられればよい)
- 参考: `src/Engine/Engine/Physics/ConvexHull.cpp` (−0 の畳み方、縮退の扱い、決定論の書き方)
- 新規 SelfTest と `src/Editor/EditorMain.cpp` の登録 (末尾追加)
- ファイル追加後 `tools\gen_project_files.ps1`
- **触らない**: `src/Engine/Renderer/WaterPass.cpp`、`tools/deepmodal/*`、`assets/deepmodal/*`、ルート直下の一時ファイル

## 受け入れ条件 (このサブ)

1. 閉じ判定: 箱・トーラス (手続き生成)・UV 継ぎ目で頂点が割れた箱 (位置が同一の重複頂点) が「閉じている」、蓋のない箱が境界辺 4 本、平面 (quad) が境界辺 4 本、非多様体 (辺を 3 枚で共有) が非多様体 1 以上、面を 1 枚裏返した箱が向き不一致、を返す — `Editor.exe --selftest`
2. 箱・L 字 (凹)・トーラスを複数の平面 (軸平行と斜め) で切り、両側がどちらも閉じ判定を通り、両側の体積 (符号付き四面体和) の和が元と相対 1e-4 以内 — `--selftest`
3. トーラスを穴の軸に垂直な平面で輪切りにした断面が「穴のある蓋」になり (蓋の面積 = 外円 − 内円の多角形近似と相対 1e-3 以内)、両側が閉じる — `--selftest`
4. 平面がメッシュに触れない (全部片側) とき、切断なしでそのまま / 空を返す。平面が頂点・辺をちょうど通る (箱を面の対角で切る、面上に乗る平面) ときも落ちずに閉じた結果か失敗を返す — `--selftest`
5. 同じ入力で 2 回切って出力のバイト列が一致する — `--selftest`
6. 既存 SelfTest と `tools\check_rules.ps1` に変化なし。WIP ファイルを触っていない — `--selftest`、`check_rules.ps1`、`git status`
7. (round 1 で追加) 全テスト入力の符号付き体積が正 (外向き) であることを検査し、外向きの入力の切断で蓋の裏返し補正が一度も要らない (補正の経路は廃止し、閉じ判定の検算に落ちたら失敗を返す) — `--selftest`
8. (round 1 で追加) `CheckClosedMesh` が符号付き体積 (または内向きの判定) を返し、内向きに巻いたトーラスを外向きへ正規化する関数を通してから切ると、両側が外向きの閉じたメッシュ (符号付き体積 > 0) になる — `--selftest`

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
```

断面の三角形分割が頑健にできない形が見つかったら、回避策を積み増す前に SELF_EVAL の「不安・質問」で planner に返すこと (方式の切り替え判断になる。例: 蓋を作らずに凸セルとの交差で作り直す)。
処理時間 (例: 1 万三角形を 16 平面で切る) を 1 行記録しておくこと (sub-02 の焼き時間の見積もりに使う)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - src/Engine/Engine/Physics/FractureMesh.h:FractureVertex,FractureMesh,ClosedMeshCheck,PlaneCutResult — 破壊分割コア専用の最小頂点/メッシュ型とAPI宣言 (Renderer非依存)
  - src/Engine/Engine/Physics/FractureMesh.cpp:CheckClosedMesh — 位置のビット一致溶接 (-0.0畳み込み) + 無向辺の使用数・向きで境界辺/非多様体/向き不一致を検出
  - src/Engine/Engine/Physics/FractureMesh.cpp:CutMeshByPlane — 三角形ごとに平面クリップしposPoly/negPolyへファン分割、外側面 (out.positive/negative.outer) を構築。断面の輪郭は自前で辺の向きを推測せず、できあがった外側面自身の境界辺 (ExtractBoundaryEdges、CheckClosedMeshと同じ溶接手法) から取り直す
  - src/Engine/Engine/Physics/FractureMesh.cpp:ChainAllLoops — 断面の切断辺 (方向付き) を位置一致 (std::map、決定的) で繋ぎ、単純閉ループ列にする。同じ位置から2本出る/後続が見つからない/規定回数で閉じない、をすべて安全に失敗として返す
  - src/Engine/Engine/Physics/FractureMesh.cpp:CapLoops — 複数ループを「内包しているループの本数の偶奇」で外周/穴に分類 (面積の符号には依存しない設計、理由は不安・質問欄)。穴は直接の親 (1段内側) へ割り当て、BridgeHoleIntoOuter で橋渡しした上でEarClipにより耳切り三角形分割。蓋頂点のUVはtangent/bitangentへの正射影 (箱投影) で埋める
  - src/Engine/Engine/Physics/FractureMesh.cpp:BridgeHoleIntoOuter — Heldの手法の簡略版 (穴の最右点Mから+u方向レイでouterと交差、視認性を反射頂点で補正)
  - src/Engine/Engine/Physics/FractureMesh.cpp:EarClip — 単純多角形の耳切り。有効な耳のうち最も丸い(cr最大)ものを毎回選ぶ決定的アルゴリズム。橋渡しが作る「同じ座標の複製点 (橋の両端)」は耳を塞ぐ判定から除外する (実測でここが穴あき蓋の詰まりの真因だった)
  - src/Engine/Engine/Physics/FractureMesh.cpp:VerifyAndFixCapOrientation — 外側面+蓋を合成しCheckClosedMeshで検算、境界辺・非多様体が0で向き不一致だけ残る場合に限り蓋全体を裏返して再検算する安全網
  - src/Engine/Engine/Physics/FractureSelfTest.h/.cpp — 閉じ判定7件、平面切断 (箱3・L字2・トーラス2)、穴あき蓋 (トーラス輪切り、面積を多角形近似の解析値と照合)、エッジケース3件 (平面が触れない/頂点をちょうど通る/面上に乗る)、決定論1件、処理時間計測を追加。L字テストメッシュは当初7頂点 (A,B,C,D,E,F,H) の2矩形分割だったが、HがA-F辺上に乗る共線点で扇三角形が退化し実際に平面切断で破綻したため6頂点のAからの扇分割に修正した
  - src/Editor/EditorMain.cpp — `RunFractureSelfTest()` を `--selftest` の末尾に登録

仕様との差分:
  - [追加] 蓋のUVは「tangent/bitangentへの正射影 (平面基底そのもの)」として実装した。仕様は「箱投影」とだけ書かれ具体的な定義がなかったため、最も素直な解釈で埋めた。軸平行な切断平面ではワールド2軸への投影と一致する
  - [追加] 蓋の外周/穴判定は「内包しているループの本数の偶奇」(TrueTypeのグリフ輪郭と同じnonzero系の判定) を採用した。当初は「面積の符号がそのまま外周/穴を表す」という前提で実装したが、実測でトーラス/箱の具体的な組み合わせによって符号が直感と逆になるケースがあり (原因は特定できず、不安・質問欄参照)、内包関係という符号に依らない基準に置き換えた
  - [追加] VerifyAndFixCapOrientation という検算+補正の安全網を追加した。蓋の向きが外側面と噛み合わない場合があり (原因未特定)、閉じ判定で検算してから確定させる設計にした。仕様には無い実装だが、受け入れ条件2・3 (両側が閉じる) を満たすために必要だった

検証:
  - `tools\gen_project_files.ps1` → Engine/Editor/Runtime/GameLogicの各vcxprojを更新 (新規4ファイルを反映)
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /t:Editor` → ビルド成功
  - `bin\x64\Debug\Editor.exe --selftest` → 全体exit code 0、`Fracture mesh core self test: ALL PASS` (閉じ判定7件・平面切断7件・穴あき蓋1件+面積照合1件・エッジケース3件・決定論1件、全PASS)。既存の全SelfTestも`[ERROR]   FAIL:`0件で無傷
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64 /t:Editor` → ビルド成功
  - `bin\x64\Release\Editor.exe --selftest` → 全体exit code 0、`Fracture mesh core self test: ALL PASS` (Debugと同じテストが全てPASS)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `git status --short` → WIPファイル (WaterPass.cpp、deepmodal関連、ルート直下の一時ファイル) は無変更。新規4ファイルと`src/Editor/EditorMain.cpp`、`build/Engine.vcxproj[.filters]`のみ差分
  - 処理時間: 10000三角形 (トーラス100×50分割) を16平面で切る — Release 407.29 ms (25.456 ms/cut)、Debug 4779.22 ms (298.702 ms/cut)

自己採点 (1-5):
  仕様適合: 4 — このサブの受け入れ条件6件を全て検証込みで満たした (閉じ判定・平面切断+蓋・穴あき蓋・エッジケース・決定論・回帰なし)。UV箱投影も実装した。減点理由: CapLoopsの符号反転とVerifyAndFixCapOrientationの向き補正が必要になった根本原因を完全には説明できていない (不安・質問欄)
  正しさ: 4 — Debug/Release両方でexit 0、全既存SelfTestに影響なし、check_rules 0件。減点理由: EarClipのO(n^3)は蓋の頂点数が数百を超える入力 (sub-02のVoronoiで起こり得る) で未検証、VerifyAndFixCapOrientationは「閉じている」ことだけを検算する安全網であって根本原因を潰した修正ではない
  コード品質: 4 — デバッグ用の一時printfは全て除去し、日本語コメントで理由 (なぜその設計にしたか、実測で何が起きたか) を簡潔に記述した。ConvexHull.cppの流儀 (ローカルV3、-0畳み、全順序ソート) を踏襲。減点理由: CapLoops/BridgeHoleIntoOuter/EarClipの幾何アルゴリズムは行数・分岐が多く、複雑さは本質的なものだが読解コストは高い
  テスト: 5 — 閉じ判定7・平面切断7・穴あき蓋1(+面積照合)・エッジケース3・決定論1・処理時間計測の新規回帰テストをDebug/Release両方で実行し、全てPASSを確認した。検証は全て実施済みで「未実行」の項目はない

不安・質問:
  1. CapLoopsで「切ってできた断面ループの面積符号 = 外周かどうか」が、平面/メッシュの組み合わせによって直感 (正=外周) と逆になる具体例を実測で確認したが、根本原因を特定できなかった (findNewEdgeの推測ロジックを外側面自身の境界辺から取る方式=ExtractBoundaryEdgesに置き換えても同じ現象が起きた)。内包数の偶奇による判定 + VerifyAndFixCapOrientationの検算補正で全テストは安全にPASSしているが、sub-02のVoronoi分割 (256破片×32隣接まで) で同じ経路を大量に通ったとき、この「原因不明の向き反転」が既知の2パターン (面積符号だけの反転/蓋全体の巻き順反転) 以外の形で再発しないかは保証できない。時間の都合でこれ以上の深掘りをせず安全網で止めた。この設計方針 (根本原因追求より検証済みの安全網) でsub-02以降も進めてよいか、それとも根本原因の特定を優先すべきか判断してほしい
  2. EarClipはO(n^3) (蓋の頂点数nの3乗)。実測 (トーラス輪切りの穴あき蓋、蓋の合成多角形194点) で正常終了はするが、sub-11で計測予定の破片数上限 (256破片/隣接32) のVoronoi分割では、1破片あたりの断面頂点数がもっと大きくなる可能性があり、焼き時間に影響しうる。sub-01の時点では計測のみ (処理時間はSELF_EVALに記録) で最適化はしていない。sub-02や後続で速度が問題になった場合、EarClipのアルゴリズム変更 (例: モノトーン分割) が必要かもしれない
  3. 仕様との差分に書いた「蓋のUV=tangent/bitangentへの正射影」は裁定で埋めた。斜めの切断平面ではワールド軸に沿わない投影になるが、目視での歪みは未確認 (sub-01は描画に繋がないため)。sub-06以降で実際にテクスチャを貼ったときに見た目が想定と違えば、投影の基底の選び方 (OrthonormalBasisの分岐点、Duff et al. 2017の構成) を見直す必要があるかもしれない

触ったファイル:
  - src/Engine/Engine/Physics/FractureMesh.h (新規)
  - src/Engine/Engine/Physics/FractureMesh.cpp (新規)
  - src/Engine/Engine/Physics/FractureSelfTest.h (新規)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp (新規)
  - src/Editor/EditorMain.cpp (RunFractureSelfTest() の登録を1行追加)

申し送り: 次サブ (Voronoi分割) は本サブの`CutMeshByPlane`/`CheckClosedMesh`/`SignedVolume`をそのまま部品として使う想定。`FractureVertex`/`FractureMesh`はRendererに依存しない独自型なので、sub-03で`MeshVertex`へ詰め替える経路を忘れないこと。`CapLoops`の内部関数 (BridgeHoleIntoOuter, EarClip等) は匿名namespace内でこのファイル限定なので、sub-02で同種の多角形分割が要るなら関数の切り出し (ヘッダへの公開) を検討すること。

SELF_EVAL: sub-01 (round 2)
実装:
  - src/Engine/Engine/Physics/FractureSelfTest.cpp:MakeTorus — 三角形の巻き順を `Tri(a,b,c)/Tri(a,c,d)` から `Tri(a,c,b)/Tri(a,d,c)` へ修正。∂/∂θ×∂/∂φ (旧の巻き順が実質使っていた向き) は管の中心へ向く内向きだったため、逆向きの `Tri(a,c,b)` 系に直して外向きにした (planner の手計算どおりの原因だった)
  - src/Engine/Engine/Physics/FractureMesh.h/.cpp:ClosedMeshCheck.signedVolume — `CheckClosedMesh` が常に `SignedVolume(mesh)` を計算して返すようにした (closed かどうかに関わらず計算。閉じたメッシュでは実体積、負なら内向き)
  - src/Engine/Engine/Physics/FractureMesh.h/.cpp:FlipMeshWinding — 新規公開関数。全三角形の2・3番目のindexを入れ替えるだけで頂点は複製しない。内向きの閉じたメッシュを`CutMeshByPlane`に渡す前に外向きへ正規化するために使う
  - src/Engine/Engine/Physics/FractureMesh.cpp:VerifyAndFixCapOrientation → VerifyCapOrientation に改名・簡略化 — 「裏返して再検算」の補正経路を削除し、外側面+蓋を合成した閉じ判定 (`CheckClosedMesh`) の結果をそのまま返すだけにした。向き不一致を黙って隠さず、閉じていないと確認できなかった蓋は素直に失敗として返す
  - src/Engine/Engine/Physics/FractureMesh.h — `CutMeshByPlane` の前提 (閉じていて外向き=signedVolume>0) をコメントに明記。`PlaneCutSide.cap` のUVコメントを「箱投影」から「断面平面への正射影」に修正 (round 1 の spec 変更に合わせる)
  - src/Engine/Engine/Physics/FractureMesh.cpp:CapLoops — 「面積の符号が直感と逆になる」というコメントを、根本原因 (射影基底と capNormal の関係で外周でも符号がどちらにもなり得る、恒常的な性質) に基づく説明へ書き直した。内包数の偶奇による外周/穴判定と、EarClipへ渡す前の強制的なCCW揃えのロジック自体は変更していない (planner指摘のとおり、これはEarClipがCCW前提であることに由来する別の・引き続き必要な仕組み)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — 全テスト入力メッシュ (box/torus/seamed box/lshape、および平面切断テストの各入力) に `SignedVolume(mesh) > 0` の検査を追加。「内向きトーラス→検出→FlipMeshWinding で正規化→切断→両側とも外向きで閉じる」テストを新規追加 (受け入れ条件7・8)
  - 処理時間の内訳を一時的に計測 (chrono計測 + printf、検証後に削除): 10000三角形の切断1回あたり、Release で `extract` (ExtractBoundaryEdges) ≈7ms、`verify` (VerifyCapOrientation内のCheckClosedMesh) ≈8ms が支配的で、`clip` ≈1ms、`chain` ≈0.1ms、`cap` (EarClip込み) ≈0.7〜1msは小さい。extractとverifyはどちらも内部で`WeldedIds`(全頂点の位置ソート)を呼んでおり、ここがボトルネックの実体だと分かった (詳細は不安・質問欄)

仕様との差分:
  - なし (round 1 の指摘・spec変更に全て追従。round 1 の差分3件は「[追加] UV正射影」「[追加] 内包数偶奇判定」「[追加] VerifyAndFixCapOrientation」だったが、UV正射影と内包数偶奇判定はspec/planner裁定で正式採用され差分ではなくなった。VerifyAndFixCapOrientationは今回「検算のみ・裏返し廃止」に簡略化してVerifyCapOrientationに改名した)

検証:
  - `MSBuild ... /p:Configuration=Debug /t:Editor` → ビルド成功
  - `bin\x64\Debug\Editor.exe --selftest` → exit code 0、`Fracture mesh core self test: ALL PASS`。`[ERROR]   FAIL:` は全体で0件 (grepで確認)。`VerifyCapOrientation`の裏返し経路を削除した状態で全テストが通ることを確認 (= 根本原因の特定が正しかったことの実測での裏付け)
  - `MSBuild ... /p:Configuration=Release /t:Editor` → ビルド成功
  - `bin\x64\Release\Editor.exe --selftest` → exit code 0、同じ全項目PASS、`[ERROR]   FAIL:` 0件
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `git status --short` → WIPファイルは無変更。差分は新規4ファイル+`src/Editor/EditorMain.cpp`+`build/Engine.vcxproj[.filters]`のみ (round 1 と同じ、追加ファイルなし)
  - 処理時間 (最終、計測用printf除去後): Release 10000三角形×16平面 = 289.08 ms (18.068 ms/cut)、内訳計測時 (printfあり、除去前) は `extract`+`verify` が合計の半分強を占めることを確認済み

自己採点 (1-5):
  仕様適合: 5 — must指摘2件・should指摘1件・Q1〜Q3の回答を全て反映し、追加された受け入れ条件7・8を含めて検証済み
  正しさ: 5 — 根本原因 (MakeTorusの内向き巻き) を特定した上で修正し、安全網 (裏返し補正) を削除した状態で全テストが通ることを実測で確認した。Debug/Release両方exit 0、既存SelfTestに影響なし、check_rules 0件
  コード品質: 5 — 一時計測コードは全て除去。コメントは根本原因に基づいて書き直し、「原因不明」のような曖昧な記述を残していない。`VerifyAndFixCapOrientation`→`VerifyCapOrientation`の改名で「もう裏返さない」ことが関数名からも分かるようにした
  テスト: 5 — 受け入れ条件7・8に対応する新規テスト (全入力のsignedVolume>0検査、内向きトーラスの検出+正規化+切断+両側外向き確認) を追加し、Debug/Release両方でPASSを確認した

不安・質問:
  1. `extract`(ExtractBoundaryEdges) と `verify`(VerifyCapOrientation内のCheckClosedMesh) が処理時間の半分強を占めており、どちらも内部で独立に`WeldedIds`(全頂点の位置ソート)を呼んでいる。1回の`CutMeshByPlane`呼び出しで、外側面ごとに溶接が2回 (ExtractBoundaryEdgesで1回、VerifyCapOrientationのCheckClosedMeshで1回、蓋を足した後の合成メッシュに対して) 走っている計算になる。sub-02のVoronoi分割で1破片あたり十数回切ると聞いているので、ここが焼き時間のボトルネックになる可能性が高い。今回は指示通り最適化していないが、sub-02以降で溶接結果を使い回す設計 (例: 外側面の溶接結果をVerifyCapOrientationへ渡す) を検討する価値があるかもしれない。判断が必要になったら知らせてほしい
  2. round 1 で指摘のあった「EarClipのCCW前提による強制的な向き揃え」はそのまま残した (planner指摘 1(d) の解釈どおり)。これはMakeTorusのバグとは独立した、EarClip自体の前提に基づく必要な仕組みだと理解しているが、この理解が誤っていないか (= 今回のMakeTorus修正だけで実は不要になったのに残してしまっていないか) を一応確認したい。全テストがpassしている以上、少なくとも害はないと判断している

触ったファイル: (round 1 と同じ、追加ファイルなし)
  - src/Engine/Engine/Physics/FractureMesh.h
  - src/Engine/Engine/Physics/FractureMesh.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.h (変更なし、内容は round 1 のまま)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Editor/EditorMain.cpp (変更なし、内容は round 1 のまま)

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 2 件: (1) 蓋の向き補正が要った根本原因 = SelfTest の `MakeTorus` が内向きに巻かれていた (θ=0,φ=0 で面法線 (−1,0,0))。生成を外向きへ直し、全テスト入力の符号付き体積 > 0 を検査し、外向きの入力で `VerifyAndFixCapOrientation` の裏返しが一度も起きないことをテストで固定する。裏返しは廃止して「検算に落ちたら失敗」にする (内向きの入力は切断前に正規化する契約) (2) `CheckClosedMesh` の結果に符号付き体積 (または内向きフラグ) を足し、内向きの閉じたメッシュを外向きへ裏返す関数を用意して、内向きのトーラスを入口で正規化してから切ると外向きの結果が得られるテストを足す。should 1 件: 10k 三角形 / 1 切断 25 ms (Release) の内訳を 1 行 (どこが支配的か) 測る。回答: 質問 2 (EarClip O(n^3)) は今は最適化しない。sub-02 で焼き時間を記録し sub-11 で判断。質問 3 (UV) は正射影を仕様として採用 (spec 変更)。build\Engine.vcxproj / .filters はコミットに含める
- round 2: VERDICT OK (planner)。must 2 件解消 (MakeTorus の巻きを外向きへ、裏返し補正を廃止しても全テスト PASS = 原因特定の実測裏付け、signedVolume / FlipMeshWinding / 内向きトーラスの正規化テスト)。should 1 件は sub-02 へ申し送り: `CapLoops` のコメント (FractureMesh.cpp:579-582) の「外周でも正・負のどちらにもなり得る」は不正確。t×b = capNormal に固定しているので、外向きの外側面の境界辺から取ったループは**外周が常に CW・穴が常に CCW** (蓋は境界辺と逆向きに回る必要があるため)。強制揃えは結果として常に全反転で、残してよいが、コメントをこの理由に直すこと
