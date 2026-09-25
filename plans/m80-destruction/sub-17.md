# sub-17: review-1 — 実行時の正しさ (縮む挙動の scale、スキンの破片の位置、割れた tick の階層、隣接の対称な切り捨て)

- 依存: sub-16
- 状態: 未着手
- 往復: 0

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

## 実装メモ (coder が追記)

## フィードバック履歴
