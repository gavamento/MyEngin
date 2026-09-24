# sub-13: 凸包生成の無限ループ修正とトーラスの焼き検証 (sub-02 から切り出し)

- 依存: sub-02
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
