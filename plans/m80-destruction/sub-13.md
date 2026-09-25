# sub-13: 凸包生成の無限ループ修正とトーラスの焼き検証 (sub-02 から切り出し)

- 依存: sub-02
- 状態: OK (commit d873eac)
- 往復: 1

## 出所

sub-02 round 3 (差し戻し上限) で、トーラス (`MakeTorus(2.0, 0.6, 24, 16)`, pieceCount 8) の破片 1 つ (rank 7、三角形 234) について、`BuildConvexHull` が終わらなかった。sub-02 は箱・L 字で受け入れ条件を満たしたので OK とし、トーラス分 (sub-02 の受け入れ条件 1・6 のうちトーラスに関わるもの) をこのサブへ移した。

## 原因 (planner がコードで特定済み)

`src/Engine/Engine/Physics/ConvexHull.cpp:219-307` の逐次追加ループ:
- 最遠点 `pick` は `dmax > eps` なので `keep` に入り、`outside.swap(keep)` の後も **`outside` に残る**
- 地平線の新面が縮退して `MakeTriPlane` が失敗すると (`ok == false`)、`continue; // pick は既に outside から外れている` で次の周回へ進む。しかしコメントに反して pick は outside に残っている
- `tris` も `outside` も変わっていないので、次の周回で**同じ pick が選ばれ、同じ理由で失敗する** = 無限ループ。`vertCount` も増えないので `kConvexMaxVerts` の上限でも止まらない

ヘッダの契約 (`ConvexHull.h:20-26`「途中で破綻したらその点を捨てて先へ進める」) どおりに、失敗した pick を捨てれば止まる。

## やること

1. `ConvexHull.cpp`: `ok == false` のとき、`pick` を `outside` から取り除いてから `continue` する (コメントも実態に合わせる)。それ以外は変えない
2. **既存へのビット一致の根拠**: 修正前は `ok == false` の経路に入った時点で必ず無限ループになる (状態が変わらないため)。したがって、**修正前に終了していた全入力はこの経路を一度も通っていない** = 結果は 1 ビットも変わらない。これをコメントに書き、検証 3 で実測でも確かめる
3. 回帰テスト: sub-02 で止まった破片の点群 (rank 7) を、SelfTest のフィクスチャとして再現する (焼きの途中の点を取り出すか、同じトーラス・seed で焼いてから取り出す。どちらでもよい)。`BuildConvexHull` が有限時間で終わり、`Valid()` であること。`ConvexSelfTest.cpp` に置くか `FractureSelfTest.cpp` に置くかは coder 判断
4. sub-02 のトーラスのテストを戻す (spec §5 受け入れ条件 3、sub-02 の受け入れ条件 1〜3・6 のトーラス分): 外向きの `MakeTorus`、pieceCount 8 と 32。幾何的に閉じる (体積 > 0、ベクトル面積の和 ≤ 表面積 × 1e-4)、体積和が元と相対 1e-4、凸包がすべて `Valid()`、焼き時間の記録 (Debug / Release)。sub-02 の digest テストに、トーラスの digest も Debug / Release の一致対象として足す
5. トーラス 768 三角形・pieceCount 32 が Release で 10 秒を超えるなら、段階ごとの内訳を SELF_EVAL に書く (最適化は sub-11)

## やらないこと (このサブでは)

- 凸包アルゴリズムの変更 (QuickHull 化、上限の変更、eps の調整)。止まらないことを直すだけ
- 焼きの方式の変更

## 触る場所 (planner の見立て)

- `src/Engine/Engine/Physics/ConvexHull.cpp` (219-307 のループ内、`if (!ok)` の所)
- `src/Engine/Engine/Physics/ConvexSelfTest.cpp` または `FractureSelfTest.cpp`
- **触らない**: WIP ファイル

## 受け入れ条件 (このサブ)

1. rank 7 の点群のフィクスチャで `BuildConvexHull` が終わり、`Valid()` — `--selftest`
2. トーラス pieceCount 8 / 32 が幾何的に閉じ、体積和が一致し、凸包がすべて有効。焼き時間を記録 — `--selftest` (Debug / Release)
3. **既存の凸包を使うものがビット一致**: `ConvexSelfTest` / `PhysicsSelfTest` PASS、`tools\replay_verify.bat` の `joints` job (`.mcvx` のクックを含む) と `physics` job が PASS。可能なら修正前のビルドで録った `--joint-demo` の rep を、修正後の Debug / Release で `--replay-verify` して全 tick 一致 (旧 rep を用意する手順は sub-05 と同じく、WIP を動かさない方法で)
4. トーラスの digest が Debug / Release で一致 — 両構成の `--selftest` ログ
5. `check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

SELF_EVAL: sub-13 (round 1)
実装:
  - src/Engine/Engine/Physics/ConvexHull.cpp:BuildTriHull — 逐次追加ループの `if (!ok)` 分岐で、
    地平線の新面が縮退して失敗した `pick` を `outside` から `std::remove`/`erase` で取り除いて
    から `continue` するよう修正 (`outside.erase(std::remove(outside.begin(), outside.end(), pick), outside.end());`)。
    実態と食い違っていたコメント (「pick は既に outside から外れている」) も書き直した
  - src/Engine/Engine/Physics/ConvexSelfTest.cpp / .h — トーラス (`MakeTorus(2.0, 0.6, 24, 16)`、
    pieceCount=8、seed=1) の焼きで rank=7 の破片に現れた点群 (`BuildConvexHull` へ渡す直前、
    位置の完全一致で重複除去した 195 点、一時的なログ出力で実測して取得) を `TorusRank7Points()`
    としてフィクスチャ化し、`BuildConvexHull` が有限時間で終わり `Valid()` を返すことを確認する
    回帰テストを追加
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — sub-02 でハング回避のためコメントアウト・
    見送りにされていたトーラスの Voronoi 焼きテスト (7 節: pieceCount 8/32)、digest 一致テスト
    (11 節)、焼き時間計測 (12 節、torus 8/32 pieces) を復元・追加。「BuildConvexHull で停止する」
    旨の古い NOTE コメントを削除
仕様との差分:
  - なし。sub-13.md の「やること」1〜5 をすべて実施
検証:
  - MSBuild で Debug|x64 / Release|x64 をビルド → 成功
  - bin\x64\Debug\Editor.exe --selftest → FAIL 0 件。rank7 回帰: `BuildConvexHull finished in
    2.128 ms` で PASS。voronoi torus/8, torus/32 (焼き・面欠けなし・凸包 Valid・体積保存) すべて
    PASS。digest (torus seed=44 pieces=8) = 0x87A81F882E68E6DE
  - bin\x64\Release\Editor.exe --selftest → FAIL 0 件。rank7 回帰: 0.349 ms で PASS。焼き時間:
    torus/8=56.57ms、torus/32=593.62ms (10 秒未満なので内訳計測は不要)。digest =
    0x87A81F882E68E6DE (Debug と一致)
  - tools\replay_verify.bat (フル 13 ジョブ) → 全 PASS、149.3 秒、exit 0
    (demo/parts/flow/mp/physics/joints/acoustic/ui の 8 シーンチェーン + タイムトラベル×2 +
    what-if×2 + 静的規則チェック)
  - tools\check_rules.ps1 → 0 error, 0 warning
  - 追加検証 (受け入れ条件 3 の「修正前ビルドとのビット一致」): `git worktree add` で基点コミット
    `e2175f9` (ConvexHull.cpp 修正前) を別ディレクトリへ展開して Debug ビルドし、そこで
    `--joint-demo --replay-record` により rep を録画。その rep を本体リポジトリ (修正後) の
    Debug / Release 双方で `--replay-verify` し、600 tick 全てハッシュ一致 (`VERIFY PASS`) を
    確認した。worktree は検証後に `git worktree remove` で削除済み (作業ツリーへの影響なし)
  - `tools\gen_project_files.ps1` を実行し、`build/*.vcxproj(.filters)` に差分が出ないこと
    (ファイルの追加・削除がないこと) を確認
自己採点 (1-5):
  仕様適合: 5 — sub-13.md の「やること」1〜5・受け入れ条件 1〜5 をすべて実装し検証した
  正しさ: 5 — 特定済みの原因通りに 1 行 (無効化) 修正し、修正前ビルドで録った rep が修正後
    Debug/Release で全 tick ビット一致することを実測で確認した (推測ではなく実測)
  コード品質: 4 — 既存の流儀 (コメント密度・命名) に合わせた。rank7 フィクスチャの 195 点
    ハードコード配列はやや長いが、実際のバグ再現条件そのものであり由来をコメントに明記した
  テスト: 5 — rank7 専用の回帰テストに加え、sub-02 で見送られていたトーラスの焼き・digest・
    timing テストを復元し、Debug/Release 双方で実行して結果を記録した
不安・質問: なし
触ったファイル:
  - src/Engine/Engine/Physics/ConvexHull.cpp
  - src/Engine/Engine/Physics/ConvexSelfTest.cpp
  - src/Engine/Engine/Physics/ConvexSelfTest.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
申し送り: なし (sub-06 は依存すると harness.md にあるが、本サブの修正・追加テストは
  ConvexHull.cpp の 1 分岐のみで完結しており、他サブへ影響する変更はない)

SELF_EVAL: sub-13 (round 2)
実装:
  - #1: FIX_REQUEST (planner VERDICT round 1 の should) の指摘どおり、コード中のコメントから
    作業経緯の参照 (`sub-13`、`round-2 裁定` 等) を除いた。対象は指摘に明示された
    ConvexHull.cpp / ConvexSelfTest.h / ConvexSelfTest.cpp / FractureSelfTest.cpp /
    FractureBake.cpp の 5 ファイル (指摘にあった FractureMesh.cpp は `sub-`/`round` の言及なし、
    無変更)。理由の文自体 (「修正前にここへ来た入力は…既存の結果は変わらない」等) は残した
仕様との差分:
  - [追加] 指摘は「sub 番号や round 番号」を対象にしていたが、FractureBake.cpp に同種の
    経緯情報として `should #3` (レビュー指摘番号) が 3 箇所あったため、AGENTS §5 の趣旨
    (レビュー指摘の番号をコードに残さない) に従い併せて除いた。挙動・検証結果への影響はない
検証:
  - MSBuild で Debug|x64 を再ビルド → 成功 (差分は今回の 5 ファイルのみ再コンパイル)
  - bin\x64\Debug\Editor.exe --selftest → FAIL 0 件。rank7 回帰 (2.138 ms)・voronoi torus/8・
    torus/32 (焼き・凸包 Valid・体積保存) すべて round 1 と同じ結果で PASS (挙動不変を確認)
  - tools\check_rules.ps1 → 0 error, 0 warning
  - Release ビルドと replay_verify.bat は指示どおり再実行していない (コメント文言のみの変更で
    挙動は変わらないため、Debug ビルドと check_rules のみで足りるという指示に従った)
自己採点 (1-5):
  仕様適合: 5 — 指摘 1 件 (#1) を対象ファイルすべてで解消し、範囲外 (FractureMesh.cpp) は
    無変更である根拠 (grep で該当なし) を確認した
  正しさ: 5 — Debug ビルドと該当セルフテストの結果が round 1 と数値まで一致し、コメントのみの
    変更で挙動が変わっていないことを確認した
  コード品質: 5 — 経緯参照を除いた分、コメントが AGENTS §5 によりよく従うものになった
  テスト: 4 — 指示どおり Debug ビルド + check_rules のみ再実行。Release ビルドと
    replay_verify.bat のフル再実行はしていない (未実行。挙動が変わらない変更のため指示で
    省略されたが、自己採点としては「全構成を再検証していない」ことを正直に反映する)
不安・質問:
  - FractureMesh.h (Physics/FractureMesh.h) に `sub-02`/`sub-03` の言及が残っている
    (`FractureMesh.h:19-20`)。指摘は「FractureMesh.cpp」のみを列挙しており .h は対象に
    含まれていなかったため触っていない。同種の指摘が再度出るなら知らせてほしい
触ったファイル:
  - src/Engine/Engine/Physics/ConvexHull.cpp
  - src/Engine/Engine/Physics/ConvexSelfTest.cpp
  - src/Engine/Engine/Physics/ConvexSelfTest.h
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/Physics/FractureBake.cpp
申し送り: なし

## フィードバック履歴
- round 1: VERDICT OK (planner)。修正は 1 行 (失敗した pick を outside から外す)。検証: rank 7 のフィクスチャで Debug 2.1 ms / Release 0.35 ms に終了、トーラス 8 / 32 の焼きが PASS、digest が Debug / Release で一致、replay_verify 13 job PASS、修正前のコミットで録った `--joint-demo` の rep が修正後の Debug / Release で 600 tick 一致。should 1 件: コード中のコメントの「sub-13 で特定」「(sub-13 回帰)」は作業経緯の参照なので、コミット前に除く (AGENTS §5)
