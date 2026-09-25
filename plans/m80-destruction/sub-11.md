# sub-11: 計測・ベンチ・上限の決定・RT / 描画のスパイク対策

- 依存: sub-08, sub-10
- 状態: 未着手
- 往復: 0

## やること

推測で上限を決めない (AGENTS §3.5)。実測で「60 Hz の tick 予算に収まる破片数」を決め、既定値と Inspector の上限に反映する。

1. **プロファイル**: 物理内部 (収集 / 広域 / 狭域 / ソルバ / 書き戻し。sub-05 では入れていないので、このサブで足す) と FractureSystem に `MYE_PROFILE_SCOPE`。状態に影響しないこと
2. **ベンチ**: 破片数 N (16 / 32 / 64 / 128 / 256) の破壊物 1 つと、複数 (例 8 個) を並べたシーンを、フラグ付きの計測モード (例 `--fracture-bench N`、ヘッドレスで決まった tick 数を回して各スコープの平均と最大を出す。既存の `--perf-rate` 等のフラグの流儀に倣う) で測る。壊れる前 / 割れた瞬間 / 割れた後 (全塊が動く) の 3 区間を分けて出す
3. **焼き時間**: sub-02 / sub-04 の焼きを N ごとに計測
4. **上限の決定**: Release で「物理 + FractureSystem が 1 tick (16.6 ms) の半分程度に収まる」破片数を推奨値に、ハード上限 256 のまま Inspector の範囲と既定値を見直す (決め方の閾値は SELF_EVAL で数値と共に提示し、planner が判定する)。結果を `plans/m80-destruction/bench.md` に記録 (計測環境・コマンド・表)
5. **スパイク対策**: RT on (`--rt`) で割れた瞬間の BLAS 焼き直し (`RtScene.cpp:64-120`) の時間を計測。問題なら対策 (例: 破壊物の破片メッシュを最初から BLAS の集合に含める) を入れる。描画 (インスタンシングが破片で切れる) のコストも計測だけはする
6. 対策が状態・replay に影響しないこと (描画側のみ)

## やらないこと (このサブでは)

- 物理のアルゴリズム改善 (広域の分割など)。重すぎる場合は計測結果と改善案を SELF_EVAL に書き、planner が別サブにするか判断する

## 触る場所 (planner の見立て)

- `src/Engine/Engine/Physics/PhysicsSystem.cpp`、`FractureSystem.cpp` (スコープ)
- `src/Engine/Engine/EngineCli.cpp` (計測フラグ)、`DemoContent.cpp` (ベンチシーン)
- `src/Engine/Engine/RayTracing/RtScene.cpp` (対策するなら)
- `Components.cpp` の範囲、`InspectorWindow.cpp` の推奨表示
- `plans/m80-destruction/bench.md` (新規、計測記録)

## 受け入れ条件 (このサブ)

1. `bench.md` に N ごとの 3 区間の物理 / FractureSystem 時間 (Release)、焼き時間、RT の割れた瞬間の時間が表で残る
2. 推奨値と上限が表の数値から導かれ、Components.cpp の範囲 / 既定値と Inspector の表示に反映 — 差分
3. 対策を入れたなら前後の数値。replay / golden に影響なし — `replay_verify.bat`、`shot_verify.bat`
4. `check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
（Release|x64 をビルド）
bin\x64\Release\Editor.exe --fracture-bench ...   (名前は coder 判断)
bin\x64\Release\Runtime.exe --fracture-demo --rt ...
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

## フィードバック履歴
