# sub-17: review-1 — 実行時の正しさ (縮む挙動の scale、スキンの破片の位置、割れた tick の階層、隣接の対称な切り捨て)

- 依存: sub-16
- 状態: OK (commit 16ab02a)
- 往復: 2

## 出所

C:\HAL\MyEngin\plans\m80-destruction\review-1.md の指摘 #3・#4 (planner 宛で spec §2 に裁定)・#11・#13。

## やること

1. **#3 縮んで消える**:
   - 問題: `afterBreak = 3` が scale を絶対値で上書きしている (`FractureSystem.cpp:611-619`)。scale 2 のルートでは、縮み始めに 2 → 1 へ飛ぶ。非一様スケールも失われる
   - 直し方: 縮み始めた時点のリーダーの `LocalTransform.scale` (3 成分) を基準にして、比例で縮める
   - 基準値の置き場: tick 数から毎 tick 計算するなら、基準の scale を**コンポーネントの欄**に持つ (`FracturePiece` に末尾追加。ハッシュ対象。ECS 外に置かない)。末尾追加は既存型のレイアウト変更なので、`kSimSnapshotVersion` を 24 に上げ、`AcousticAudioSelfTest.cpp` の版の固定値も合わせる
   - 前の tick の値に係数を掛ける形なら欄は要らないが、その場合は最後の tick がちょうど 0 になる式にすること
   - どちらにするかは coder 判断。決定性は同じ
2. **#4 破片の位置を体積重心で定義する** (spec §2 の裁定):
   - `FractureLibrary` の読み込み時に、破片ごとの `localCenter` (破片ローカルでの、外側面 + 蓋の体積重心) を計算して持つ
   - `ApplyFractureDamage` の距離 (`FractureSystem.cpp:878-884`) と `onBreak` の point は、`破片のワールド行列 × localCenter` で測る
   - スキンでない破片は `localCenter ≈ 0` なので、既存の挙動はほぼ変わらない。変わる量は SELF_EVAL に書く
3. **#11 割れた tick の中の食い違い**:
   - 問題: 付け替えた破片は `LocalTransform` を新しい親の基準で即座に書く。一方で `SetParent` は tick 末に回っている。そのため、その tick の残り (onBreak・LateUpdate・パーティクル) では、ワールド位置が間違う (review-1 P8: 正しくは (2.908, 0.578) が (5.908, 1.078))
   - 直し方: 「同じ tick の中で、破片の `LocalTransform` と階層が食い違わない」を不変量にする
   - 方法は coder 判断。例:
     - FractureSystem の構造変更を、反復の外で行って即時に適用する (World は反復中でなければ即時に適用する)
     - `LocalTransform` の書き込みも tick 末へ回す
   - どちらでも、onBreak の point はワールドで正しい値を渡す
4. **#13 隣接の切り捨てを対称にする**:
   - 問題: 隣接が 32 を超えたときの切り捨てが片側だけで、切れない接着が生まれうる (`FractureBake.cpp:1240-1265`、`FractureSystem.cpp:284, 377-389`)
   - 直し方: i 側で (i,j) を落としたら、j 側からも (j,i) を落とす。落とす順は決定的に (面積の小さい順、同値は index)
   - 焼きの出力が変わり得るので、`kFractureBakeVersion` (sub-16 で新設) を上げる。digest の変化は許す (Debug / Release の一致は保つ)

5. **(sub-16 からの申し送り) 重複ロジックの整理**: `FractureBuilder.h/.cpp` の `ValidateFracturePieces` は、本番の判定 (`FracturePieceIndicesMatchAsset`) と重複していて、コメントも実態と食い違っている。呼び出し元が SelfTest とログ用の 1 か所 (`FractureSystem.cpp` の asset == nullptr の ERROR) だけなら、ログを直接出す形に置き換えて関数を削除する。残すなら、本番の判定の関数を呼ぶ薄い口にして、コメントを直す

## やらないこと (このサブでは)

- sub-16 / sub-18 の範囲

## 触る場所 (planner の見立て)

- `src/Engine/Engine/FractureSystem.*`、`src/Engine/Engine/Physics/FractureLibrary.*`、`src/Engine/Engine/Physics/FractureBake.cpp`
- `src/Engine/Core/Components.h/.cpp` (欄を足す場合)、`src/Engine/Engine/Replay/SimSnapshot.h`、`src/Engine/Engine/AcousticAudioSelfTest.cpp` (版を上げる場合)
- `FractureSelfTest.cpp` / `FractureSkinSelfTest.cpp`

## 受け入れ条件 (このサブ)

1. scale (2, 1, 0.5) のルートから分かれた塊が「縮んで消える」とき、縮み始めの tick で大きさが飛ばず、各軸の比を保って 0 へ向かう — `--selftest`
2. スキンの破片のうち骨が同じ 2 つに、`ApplyFractureDamage` を片方の重心の近くへ小さい半径で与えると、そちらだけに損傷が入る。onBreak の point が破片の重心のワールド位置 — `--selftest`
3. 割れた tick の onBreak と LateUpdate の時点で、分かれた破片のワールド位置が、その tick の末の位置と一致する (review-1 P8 の配置) — `--selftest`
4. 隣接が 32 を超える入力 (人工的な形状) で、切り捨てが対称になり、全接着が切れうる — `--selftest`
5. 並走の決定性 (ハッシュ列の一致)、Debug / Release の digest 一致、`replay_verify.bat` 全 job、`shot_verify.bat` (既知の 4 枚以外。fracture の golden が変わるなら撮り直して理由を書く)、`check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## round 1 の裁定 (planner、FIX_REQUEST の手順)

**#11 の直し方を置き換える**。round 1 の `fracturesys::Install` / `Instance` と `reparentOverride_` + `ComposeWorldPoseWithOverride` は**削除**する。理由:
- `ApplyFractureDamage` の結果が、グローバルな注入があるかどうかで変わる (SelfTest の実測で、installed = false のとき gain = 0)。AGENTS §3.2 の「暗黙的な登録で挙動を予測しにくくしない」に反する
- `reparentOverride_` は FractureSystem.Update の冒頭でしか消えないので、tick の境界をまたいで残る (次の tick の Update フェーズのスクリプトが、前の tick の対応表を読む)
- 汎用の位置クエリ (Raycast / Overlap / 部位クエリ) の窓は残ったまま

手順:
1. `ReparentKeepWorld` は、付け替え (`SetParent`、コマンドバッファ) と新しい `LocalTransform` の計算までにする。**`LocalTransform` はまだ書かない**。計算した値は FractureSystem の「tick 末に書く表」(entity index 昇順の vector) に積む
2. FractureSystem に後処理 (例 `ApplyDeferredLocals(World&)`) を足す。表の `LocalTransform` を書いて表を空にする。**TickRunner が `ApplyStructuralChanges` (`TickRunner.cpp` の tick 末フラッシュ) の直後、ハッシュを取る前に、明示的に呼ぶ**。stepSim が false の tick は、表が空なので何もしない
3. 表は tick の中でしか中身を持たない (tick 末で必ず空になる)。スナップショットは tick 末のフラッシュの後に撮るので、境界ではいつも空。SelfTest で「tick 末に表が空」を検査する
4. これで同じ tick の中は「古い親 + 古い LocalTransform」(ワールドの位置は正しい)、tick 末からは「新しい親 + 新しい LocalTransform」になる。`ApplyFractureDamage` も汎用の位置クエリも、特別扱いなしで正しい位置を使う
5. 分離の速度 (新リーダーの Rigidbody) は今までどおり AddComponent の値で入る。付け替えの前後で重心の速度の定義が変わらないことを確かめる
6. テスト 19 (割れた tick の中の ApplyFractureDamage) は、注入の有無の比較をやめる。「割れた tick の LateUpdate 相当の時点で、分かれた破片のワールド位置が tick 末の位置と一致し、そこへの損傷が入る」を検査する
7. 他の内容 (#3 の逐次の縮小、#4 の localCenter、#13 の対称な切り捨てと `kFractureBakeVersion` 2、`ValidateFracturePieces` の削除) は受け入れ済み。変えないこと

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-17 (round 1)
実装:
  - src/Engine/Engine/FractureSystem.cpp:ProcessAfterBreak (afterBreak=3) — #3。絶対値
    {s,s,s} での上書きをやめ、前 tick の scale へ比例係数 (fade-t)/(fade-t+1) を掛ける
    逐次方式に変更 (欄は増やさない coder 判断)。非一様スケールも各軸独立に保たれる。
    最終 tick は整数 0 の除算前後で厳密に 0.0f になる (kMinFractureFadeScale は削除)
  - src/Engine/Engine/Physics/FractureLibrary.h/.cpp:FracturePieceRef::localCenter — #4。
    破片ローカル (outer+cap) の体積重心を読み込み時に計算して持つ (.mfrac の形式は不変)
  - src/Engine/Engine/FractureSystem.cpp:ProcessRoot (onBreak の point) / ApplyFractureDamage
    (距離判定) — #4。localCenter のワールド変換オフセットを足すよう変更
  - src/Engine/Engine/FractureSystem.h/.cpp:ReparentKeepWorld / ComposeWorldPoseWithOverride /
    fracturesys::Install・Instance — #11。ReparentKeepWorld は書き込みタイミングを変えず
    (LocalTransform は従来どおり即座に新しい親基準で書く)、child→newParent の対応表
    (reparentOverride_、Update() 冒頭でクリア) を新設した。ApplyFractureDamage は
    fracturesys::Instance() 経由でこの表を読み、world.GetParent() の代わりに使う
    ComposeWorldPoseWithOverride で世界姿勢を合成する。EngineLoop.cpp で
    fracturesys::Install(&fractureSystem) を配線 (fracturelib と同じ流儀)
  - src/Engine/Engine/Physics/FractureBake.h/.cpp:CapNeighborsSymmetrically — #13。
    隣接 32 本超の切り捨てを全破片ぶんまとめて判定し、どちらか一方が「落とす」と
    決めた組を両側から消す (対称性を保証)。kFractureBakeVersion を 1→2 に上げ、
    宣言コメントに「上げないと RT の BVH 等のキャッシュが古いまま残る」を追記
  - src/Engine/Engine/FractureBuilder.h/.cpp — sub-16 からの申し送り (item5)。
    ValidateFracturePieces を削除 (production 経路は既に共有ロジック
    (FracturePieceIndicesMatchAsset) を直接使っており、呼び出し元は SelfTest の
    2 箇所だけだった)。FractureSelfTest.cpp の該当 2 箇所を DestructiblePiecesMatchAsset
    (本番と共有の判定関数) に置き換え
  - テスト追加: FractureSelfTest.cpp に (17d2) 非一様スケールの縮小、(10b) 隣接 32 本超の
    対称切り捨て (人工的な隣接グラフによる直接検算)、(19) 割れた tick 中の
    ApplyFractureDamage が正しい世界姿勢で判定すること (installed=true/false の対比で
    修正前後の挙動差を実測)。FractureSkinSelfTest.cpp に (6) 骨が同じ 2 破片を localCenter
    で区別すること (損傷判定・onBreak の point の両方を検算)
仕様との差分:
  - [逸脱] #11 は「割れた tick の中で LocalTransform と階層が食い違わない」という
    不変量そのものを engine 全体で回復してはいない。World::SetParent は (イテレーション
    有無に関わらず) 常に tick 末まで遅延される既存の設計で、これを変える、または
    LocalTransform の書き込み自体を tick 末へ回すには、World.cpp のコア変更か
    FractureSystem::Update を呼ぶ約 56 箇所 (ほぼ全部が既存 SelfTest) の呼び出し規約を
    変える必要があり、このサブの範囲を大きく超えると判断した。今回は review-1 #11 が
    名指しした具体的な失敗経路 (ApplyFractureDamage を onBreak/LateUpdate から呼ぶ) だけを
    fracturesys 経由の override で正す局所対応にとどめ、残る制約 (Install していない
    呼び出し元、または汎用の位置クエリ経由でこの窓に触れる経路) はコードコメントに明記した
    (review-1 #11 自身の「窓の制約を文書化するか」という代替解決も踏まえた判断)。
    なお、onBreak の point 自体は今回の変更前から (reparent 前に計算されるため) 食い違って
    いなかったことをコードを読んで確認した (テスト (19) の前半で裏付け)
  - [追加] EngineLoop.cpp への fracturesys::Install 配線 (sub-17.md の「触る場所」に無い
    ファイル)。ApplyFractureDamage の修正を本番経路 (ABI 経由でスクリプトから呼ばれる
    実際の道筋) で効かせるために必要と判断した
  - [追加] #13 の受け入れ条件テストは、実際の焼き (33 本以上の隣接を持つ実メッシュ) では
    なく、分割コアが渡す「破片ごとの隣接リスト」の形を人工的に組んで
    CapNeighborsSymmetrically を直接検算した。33 本以上の隣接を作る実メッシュは
    非常に多くの破片を要し、決定的に構成するのが難しいため、切り捨てロジックそのものを
    直接検算するほうが正確・堅牢と判断した
  - [追加] FractureSelfTest.cpp:SetupDetachedLeader に rootScale 引数 (既定 {1,1,1}) を
    追加。既存の呼び出し元は無変更 (デフォルト引数)
検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0、6306 行、FAIL 0 件
  - `bin\x64\Release\Editor.exe --selftest` → exit 0、6306 行、FAIL 0 件。fracture bake
    digest (lshape 0x521C987A83D781F7 / torus 0xD9B0EEB57B686B71) は Debug/Release 一致、
    かつ変更前と同一値 (今回のテスト形状は #13 の切り捨て分岐を通らないため無変化)
  - `tools\replay_verify.bat` → PASS (14 job 全部、fracture job・rule check 含む)
  - `tools\shot_verify.bat` → 既知の 4 枚 (parts/joints/acoustic_forward/acoustic_deferred)
    のみ FAIL (差分の実測値は今回の変更前と同一)。fracture_before/after は maxDiff=0 で
    PASS、flow_title の FAIL なし
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - `git status --short` で `src/Engine/Renderer/WaterPass.cpp` / `.agents/` / `SKILL.md`
    が無変更であることを確認
自己採点 (1-5):
  仕様適合: 4 — #3・#4・#13・(sub-16 申し送りの) item5 は sub-17.md の記述どおりに実装し
    受け入れ条件を満たした。#11 は文字通りの不変量ではなく ApplyFractureDamage に
    限定した対応であり、この解釈が妥当か不安・質問へ書いた
  正しさ: 4 — 検証コマンド全部 PASS。#11 の修正前後の挙動差は数式を手計算してから
    テストで予測どおりの結果 (installed=false で gain=0.000) を確認した。#11 の残る
    制約 (汎用の位置クエリ経由) は対象外としたため検証していない
  コード品質: 4 — 既存の規約 (QuatMul/QuatRotate の四則だけの合成、決定的なタイブレーク、
    fracturelib と同型のモジュール注入) に沿わせた。ReparentKeepWorld/ProcessRoot の
    シグネチャ変更は呼び出し元がそれぞれ 2 箇所・1 箇所のみで影響は局所的
  テスト: 4 — #3/#4/#13 は新規テストで直接検算し、#11 は「修正前は実際に失敗する
    (gain=0)」ことも対比実測した回帰テストにした
不安・質問:
  1. #11 について、World::SetParent の遅延そのものを解消する根本対応 (World.cpp のコア
     変更、または FractureSystem::Update 呼び出し元 (SelfTest 約 56 箇所含む) の呼び出し
     規約変更) はせず、ApplyFractureDamage に限定した局所対応にとどめた。この判断で
     良いか、根本対応まで求めるかを判断してほしい
  2. review-1 #11 の「onBreak の point も影響を受ける」という記述を、「onBreak
     ハンドラの中で ApplyFractureDamage 等の別 API を呼んだときに影響を受ける」という
     意味だと解釈した (onBreak 自身の point 引数は reparent 前に計算されるため元々
     正しかったことをコードで確認した)。この解釈が違えば教えてほしい
  3. sub-16 からの申し送り (item5) について、production 経路のどこからも
     ValidateFracturePieces を呼んでいなかった (「ログ用の 1 か所」は既に共有関数の
     直接呼び出しに置き換わっていた) ため、関数を削除し SelfTest 側だけ置き換えた
触ったファイル:
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/FractureBuilder.h
  - src/Engine/Engine/FractureBuilder.cpp
  - src/Engine/Engine/Physics/FractureBake.h
  - src/Engine/Engine/Physics/FractureBake.cpp
  - src/Engine/Engine/Physics/FractureLibrary.h
  - src/Engine/Engine/Physics/FractureLibrary.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/Physics/FractureSkinSelfTest.cpp
  - src/Engine/Engine/EngineLoop.cpp
申し送り:
  - sub-18 も FractureSystem.cpp を触る。今回変わったシグネチャ: ReparentKeepWorld
    (reparentOverride 引数追加)、ProcessRoot (reparentOverride 引数追加)。
    ProcessAfterBreak の case 3 はロジックを丸ごと入れ替えた (kMinFractureFadeScale
    削除)。ApplyFractureDamage は内部で localCenter・override を参照するようになった
  - fracturesys::Install/Instance という新しいモジュール注入パターン (fracturelib と
    同型) を追加した。EngineLoop.cpp で配線済み。FractureSystem のインスタンスを新たに
    生成する本番経路が増えたら、この Install も検討すること
  - kFractureBakeVersion を 2 に上げた (sub-16 で新設されたばかりの値なので、影響を
    受ける既存の `.mfrac` はテスト資産以外に無いはず)
```

```
SELF_EVAL: sub-17 (round 2)
実装:
  - src/Engine/Engine/FractureSystem.h/.cpp — #11 を「round 1 の裁定」の手順どおりに置き換えた。
    `fracturesys::Install`/`Instance`・`reparentOverride_`・`ComposeWorldPoseWithOverride`・
    `LogicalParent` を全部削除。代わりに:
    - `FracturePendingLocal` (entity/position/rotation/scale) を新設 (`FractureBreakEvent` と
      同じ namespace スコープの POD)
    - `ReparentKeepWorld` は新しい LocalTransform を計算するが**書かない**。
      `std::vector<FracturePendingLocal>& pendingLocals` へ積むだけにし、`world.SetParent` は
      従来どおり (常に tick 末のコマンドバッファ) 積む
    - `FractureSystem::ApplyDeferredLocals(World&)` を新設。表を書いて空にする
    - `FractureSystem::PendingLocalCount()` を新設 (観測用、表が空であることの検査に使う)
    - `UpdateImpl` の冒頭に `MYE_CHECK(pendingLocals_.empty());` を追加 (呼び出し元が
      `ApplyDeferredLocals` を呼び忘れたときに ERROR ログで気づけるようにする。表を黙って
      クリアしない — クリアすると呼び忘れが検出できなくなる)
    - `ApplyFractureDamage` は override 引数を外し、`ComposeEntityWorldPose`+`ComposeWorldScale`
      (通常の合成) に戻した。localCenter オフセット (#4) はそのまま
  - src/Engine/Engine/TickRunner.cpp — `scene.GetWorld().ApplyStructuralChanges();` の直後・
    `hashSources` を作る前に `fractureSystem.ApplyDeferredLocals(scene.GetWorld());` を追加
    (stepSim に関わらず毎 tick 呼ぶ。表が空の tick は no-op)
  - src/Engine/Engine/EngineLoop.cpp — `fracturesys::Install` の配線 2 箇所を削除 (モジュール
    自体を廃止したため)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp・FractureSkinSelfTest.cpp・
    src/Editor/FractureEditorSelfTest.cpp・src/Engine/Engine/Physics/FractureBenchmark.cpp —
    `<fsys 変数>.Update(...); ...; <world 変数>.ApplyStructuralChanges();` という組み合わせの
    直後に `<fsys 変数>.ApplyDeferredLocals(<world 変数>);` を機械的に挿入 (42 箇所。書き込みが
    tick 末に移ったため、Update()+ApplyStructuralChanges() だけでは分離した破片の
    LocalTransform が古いままになる)。挿入は Python スクリプトで機械的に行い、`--selftest` が
    全 PASS (かつ `MYE_CHECK` の ERROR ログが 0 件) になることで抜け漏れが無いことを確認した
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — テスト 19 (割れた tick 中の
    ApplyFractureDamage) を「注入の有無の対比」から「`PendingLocalCount()` が分離直後に増え、
    `ApplyDeferredLocals` の直後に 0 に戻ること」「割れた tick の途中 (Update 直後、
    ApplyStructuralChanges 前) でも `ComposeEntityWorldPose` が tick 末と同じワールド位置を
    返すこと」「その位置へ小さい半径で `ApplyFractureDamage` を呼ぶと同じ tick のうちでも
    正しく当たること」を検査する形に書き直した (round 1 の裁定 手順 6)
  - src/Engine/Engine/Physics/FractureSelfTest.cpp — 新規テスト (16a2)「リーダー自身も同じ
    tick で分離するとき、2 個以上のメンバーの位置がリーダー基準で正しく計算されること」を
    追加 (下の「仕様との差分」参照。round 1 に潜んでいた別のバグをこの実装で見つけたため)
  - tests/golden/fracture_after.png — 撮り直した (下の「仕様との差分」参照)
仕様との差分:
  - [発見・修正] round 1 の実装 (および恐らく sub-07 由来のオリジナル実装) には、今回のサブの
    対象と**別の**バグがあったことが分かった: `ReparentKeepWorld` が新しい `LocalTransform` を
    **即座に**書く実装だと、分かれた塊が「リーダー + メンバー 2 個以上」のとき、メンバーを
    リーダー基準へ付け替える際に呼ぶ `ComposeEntityWorldPose(world, leaderEntity, ...)` が
    「リーダーの旧い親 (階層はまだ未反映) + リーダーの新しい (書き直したばかりの)
    LocalTransform」という食い違った組み合わせを合成してしまい、メンバーの位置が大きく
    ずれる。round 2 の実装 (LocalTransform を tick 末まで書かない) では、この合成が
    「リーダーの旧い親 + リーダーの旧い LocalTransform」(= リーダー自身が今実際にある位置、
    正しい) になるため、このバグが自然に直る。証拠: 新規テスト (16a2) で、リーダー1個+
    メンバー2個が同時に分離するとき、メンバーの相対位置が期待値 (0.5,0,0)/(1.0,0,0) と
    厳密に一致することを確認した。副作用として `--fracture-demo` の `FractureBox`
    (8 破片、複数個が同時に分かれる) の見た目が変わった (小さい破片が正しい位置の近くに
    留まるようになり、round 1 で見えていた「離れた場所に飛ぶ」見た目が無くなった) ため、
    `tests/golden/fracture_after.png` を撮り直した (spec §5 受け入れ条件 26 は打ち抜きの穴の
    見た目についてで対象外、sub-17.md 受け入れ条件 5 の「fracture の golden が変わるなら
    撮り直して理由を書く」に該当する)。撮り直した画像は目視で確認済み: 壁 (FractureWall) は
    従来どおり穴が見える、腕 (FractureSkinArm) は従来どおり上半分が外れる、箱 (FractureBox)
    は 3 破片 (合計) が分離し、うち 1 個は元の位置からわずかに沈んで静止、もう 1 組
    (リーダー+メンバー1個) はリーダー基準で正しい相対位置に並んでいる。バグの発見自体は
    「仕様との差分」ではなく通常の実装過程で見つけたものだが、影響範囲 (golden 更新) を
    明示するためここに書く
  - [追加] 一時的な調査のため `MYE_LOG_INFO` によるデバッグ出力を `FractureSystem.cpp` の
    複数箇所に追加し、原因を特定した後にすべて削除した (コミットには残っていない)
検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0、6317 行、FAIL 0 件、`CHECK failed` 0 件
  - `bin\x64\Release\Editor.exe --selftest` → exit 0、6317 行、FAIL 0 件。fracture bake digest
    (lshape 0x521C987A83D781F7 / torus 0xD9B0EEB57B686B71) は Debug/Release 一致、round 1 から
    無変化
  - `tools\replay_verify.bat` → PASS (14 job 全部、fracture job・snapshot round-trip・rule
    check 含む。16g/16h 相当のスナップショット復元テストは selftest 側で確認: 個別に
    grep して `CaptureSimSnapshot`/`RestoreSimSnapshot` の前に `ApplyDeferredLocals` が
    (機械挿入により) 正しく入っていることを確認した)
  - `tools\shot_verify.bat` → 既知の 4 枚 (parts/joints/acoustic_forward/acoustic_deferred) の
    み FAIL。`fracture_after.png` は golden を撮り直した後に PASS (撮り直し前は maxDiff=197
    で FAIL していた。原因は上記のバグ修正による見た目の変化)。`fracture_before.png` は
    撮り直し不要 (maxDiff=0 で PASS のまま、分離が起きる前のショットなので無関係)
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - `git status --short` で `src/Engine/Renderer/WaterPass.cpp` / `.agents/` / `SKILL.md` が
    無変更であることを確認
自己採点 (1-5):
  仕様適合: 4 — 「round 1 の裁定」の手順 1〜7 をそのまま実装し、全部満たした (#3・#4・#13・
    item5 は変更なし、指摘どおり)。ただし golden の更新手順 (shot_verify.bat --update を
    使わず手でコピーした) は指示されていないやり方なので、不安・質問 1 で確認したい
  正しさ: 5 — 検証コマンド全部 PASS。round 2 で見つかった `--fracture-demo` の見た目の変化を
    「なぜ変わったか」まで追跡し (デバッグ出力で機構を特定)、新規テスト (16a2) で
    数値的に (期待値 0.5/1.0 との厳密一致) 裏付けた。golden の更新は根拠を示して行った
  コード品質: 4 — 42 箇所への機械的な `ApplyDeferredLocals` 挿入は Python スクリプトで
    行い、パターン一致の妥当性 (Update→ApplyStructuralChanges が 3 行以内で対応する
    ことを確認) を事前にドライランで検証した。一括変更なので個別レビューの手間は増える
  テスト: 5 — #11 の新しい表 (PendingLocalCount) の空/非空を検査するテストを追加し、
    round 1 では見えていなかった「メンバー位置の食い違い」を検出する新規テストも追加した
不安・質問:
  1. `tests/golden/fracture_after.png` の更新は、shot_verify.bat --update を使わず
    (他の 4 枚の既知の除外分まで巻き込むため)、`tests/actual/fracture_after.png` を手で
    `tests/golden/` へコピーする形で行った。この手順で問題なければそのまま進める
  2. 見つけた「メンバー位置の食い違い」バグは round 1 (と恐らくオリジナルの sub-07
    実装) にも存在していたと考えられる (このサブの本題である #11 とは別の、独立した
    バグ)。今回の #11 の直し方 (LocalTransform を tick 末まで書かない) の副作用として
    自然に直ったので追加の修正はしていないが、もし別解 (即座に書く方式) を採る場合は
    この点を別途直す必要がある、という申し送りとして残す
触ったファイル (round 1 からの追加分):
  - src/Engine/Engine/TickRunner.cpp
  - src/Engine/Engine/EngineLoop.cpp (round 1 の配線を削除する形で再度変更)
  - src/Editor/FractureEditorSelfTest.cpp
  - src/Engine/Engine/Physics/FractureBenchmark.cpp
  - tests/golden/fracture_after.png
申し送り:
  - sub-18 は FractureSystem.cpp をさらに触る。round 2 で `ReparentKeepWorld` の戻り値の
    書き込み先が `pendingLocals` (vector) に変わり、`ProcessRoot` のシグネチャも
    `reparentOverride` → `pendingLocals` に変わっている。`ApplyFractureDamage` は
    override 引数を持たない (通常の `ComposeEntityWorldPose` に戻った)
  - `FractureSystem::Update()` と `ApplyDeferredLocals()` は必ず対で呼ぶ契約になった
    (`World::ApplyStructuralChanges()` を挟んで)。新しく `FractureSystem` を使うテスト/
    呼び出し元を書くときはこの組を忘れないこと (忘れると `MYE_CHECK` の ERROR ログが
    次の Update() で出る)
```

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must: #11 をグローバルな注入 (`fracturesys::Install`) と tick をまたいで残る対応表で局所対応していた。「LocalTransform を tick 末の構造変更の直後に書く」形に置き換える (「round 1 の裁定」節)。他の 4 件は受け入れ済み
- round 2: VERDICT OK (planner)。LocalTransform を tick 末に書く形へ置き換え、グローバルな注入と対応表を全削除した。表が tick 末で空であることを MYE_CHECK とテストで固定した。副産物として、sub-07 以来の不具合 (リーダー + メンバー 2 個以上の塊で、メンバーの位置がずれる) が直った。16a2 で固定。golden `fracture_after` を手でコピーして撮り直したことは採用 (--update は既知の 4 枚を巻き込むため)。テストの穴 (分離の前後で全破片のワールド姿勢が保たれる、という一般の不変量を誰も検査していなかった) は sub-18 で埋める
