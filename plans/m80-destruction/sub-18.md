# sub-18: review-1 — 調整と操作性 (strength の既定値、ボクセル解像度の上限と取り消し、デモの壁の配置、Inspector のウェイト照会のキャッシュ)

- 依存: sub-17
- 状態: 未着手
- 往復: 0

## 出所

C:\HAL\MyEngin\plans\m80-destruction\review-1.md の指摘 #5・#6・#7 (planner 宛で spec §2 に裁定)・#10。

## やること

1. **#5 `strength` の既定値を実測で決める** (spec §2 の基準):
   - 既定の Destructible (質量 1 kg・16 破片・一辺 1 m の箱) で、次の 3 つを同時に満たす範囲を二分探索などで測る:
     - (a) 3 m から床へ落とすと割れる
     - (b) 床に 600 tick 置いても割れない
     - (c) 1 m から落としても割れない
   - 範囲の中央 (対数で) を既定値にする (`Components.h` の既定値、ツールチップ、spec §4.2)
   - 範囲が無ければ実装を変えずに、測った値を添えて返す (荷重モデルの見直しになるので planner が判断する)
   - デモ・ベンチ・テストで下げている値 (100〜200) は、既定値で足りるなら既定値に戻す。戻さないものは理由をコメントに書く
2. **#6 ボクセル解像度の上限**:
   - 開いた箱・16 破片で、解像度 80 / 96 / 128 が失敗する理由を、1 回ずつの実行で突き止める (焼きの失敗理由の文字列、どの段階か)
   - 数値や上限による失敗 (例: 破片数・頂点数の上限、libtess2 の限界) なら記録する。不具合なら直す (直すのが大きければ、理由を添えて planner に返す)
   - そのうえで、`voxelResolution` の Inspector の範囲の上限を「開いた箱・16 破片で成功する最大」(実測。直した場合はその後の値) に下げる
3. **#6 焼きの取り消し**:
   - `FractureBakeService` に取り消しを入れる。焼きの段階の合間と、切断のループの合間で旗を見る (`BakeFracture` に取り消しの旗を渡す口。既存の呼び出しは既定値で影響なし)
   - Inspector の焼き中の表示に「取り消し」ボタン
   - `Shutdown` は、取り消してから join する (エディタの終了で焼き終わりを待たない。待つのは最後の切断 1 回ぶんまで)
4. **#7 デモの壁の配置**:
   - `--fracture-demo` の固定の壁で、弾を**壁の上の縁**に当てる。外れた塊が重力で落ちて、穴が見えるようにする。壁の strength は既定値 (1 で決めた値) を基本にし、足りなければ調整する
   - golden `fracture_before` / `fracture_after` を撮り直す
   - 撃った所に穴が見える frame を、SELF_EVAL に画像パスで示す (spec 受け入れ条件 26)
5. **#10 Inspector のウェイト照会**:
   - 問題: スキンの Destructible を選んでいる間、毎フレーム `.mmdl` のクックキャッシュ全体を読んでいる (`ModelCook.cpp:355-376`)
   - 直し方: メッシュごとに結果をキャッシュする (Inspector 側、または `ModelCook` 側で。資産のホットリロードで無効化)

## やらないこと (このサブでは)

- 打ち抜き (spec §3 後回し。ユーザー回答待ち #13 / #14)

## 触る場所 (planner の見立て)

- `src/Engine/Core/Components.h/.cpp`、`src/Engine/Engine/DemoContent.cpp`、`src/Engine/Engine/Physics/FractureBenchmark.cpp`、テスト類
- `src/Engine/Engine/Physics/FractureBake.*`、`FractureVoxel.*`、`src/Editor/FractureBakeService.*`、`src/Editor/Windows/InspectorWindow.cpp`、`src/Engine/Core/LocalizationTable.inl`
- `src/Engine/Engine/Asset/ModelCook.cpp` または Inspector 側のキャッシュ
- `tests/golden/fracture_*.png`、`tools/shot_verify.bat`

## 受け入れ条件 (このサブ)

1. 既定の strength で (a)(b)(c) を満たす SelfTest。測った範囲と中央値を SELF_EVAL に書く (spec 受け入れ条件 24)
2. 解像度 80 / 96 / 128 の失敗理由が SELF_EVAL にある。Inspector の上限の解像度で、開いた箱・16 破片の焼きが成功する SelfTest がある (spec 受け入れ条件 25)
3. 取り消しの SelfTest: 焼きを始めて取り消すと、有限時間で「取り消し」の状態になる。Shutdown が焼きの途中でも速やかに戻る (手動で、エディタの終了の体感を 1 行書く)
4. デモの壁に穴が見えるスクショ、golden の撮り直し (spec 受け入れ条件 26)
5. Inspector のウェイト照会が、毎フレームではなくメッシュごとに 1 回になる (照会の回数を数える SelfTest、または計測)
6. `replay_verify.bat` 全 job (fracture の replay は録り直しで一致)、`shot_verify.bat` (既知の 4 枚以外)、`--selftest` Debug / Release、`check_rules.ps1` PASS、WIP 不変

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
