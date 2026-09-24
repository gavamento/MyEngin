# sub-08: 割れた後の 6 挙動

- 依存: sub-07
- 状態: 未着手
- 往復: 0

## やること

spec §4.1「割れた後」の表を FractureSystem に足す。単位は分かれた塊 (リーダー)。ルートに残った塊には適用しない。

1. リーダーの `releaseTicks` を毎 tick +1 (FractureSystem の中、破断判定の後。分かれた tick は 0)
2. `Destructible.afterBreak` ごとに:
   - 0 残す: 何もしない
   - 1: `releaseTicks ≥ afterBreakTicks` で塊ごと `DestroyEntity` (子孫ごと消える)
   - 2: `afterBreakTicks` 到達で塊の全 Collider の `mask = 0`・`phase = 1`、さらに `fadeTicks` 後に Destroy。重力倍率 0 の塊は沈まないが、時間で消える (仕様どおり)
   - 3: `afterBreakTicks` から `fadeTicks` かけてリーダーの `LocalTransform` の scale を線形に 0 へ (最終 tick の直前で最小値を下限にして 0 除算・零体積を避ける)、終わったら Destroy
   - 4: リーダーの `Rigidbody.isSleeping` になった tick に Rigidbody を外す (塊の Collider は静的形状として残る)。スリープが無効な環境 (`sleepDelayTicks <= 0` / 環境なし) では静的化しない (ツールチップに書く)
   - 5: この Destructible の、まだ残っている分かれた塊の数が `maxDebris` を超えたら、`releaseTicks` の大きい順 (同値は entity index 小) に超過分を Destroy
3. すべて tick 数と hashed な欄だけで決める。Destroy / RemoveComponent はコマンドバッファ
4. `--fracture-demo` の破壊物に挙動の違うものを並べてよい (例: 箱 = 1、壁 = 5)。replay 一致

## やらないこと (このサブでは)

- ワールド共通の上限、フェードの見た目 (透明化) — 後回し

## 触る場所 (planner の見立て)

- `src/Engine/Engine/FractureSystem.cpp`
- `DemoContent.cpp` の fracture builder (任意)
- `FractureSelfTest.cpp` にケース追加

## 受け入れ条件 (このサブ)

1. 6 挙動それぞれについて、決まった tick で期待どおりの状態になる (1: tick N で塊が消える / 2: tick N で mask 0、N+fade で消える、沈んで床より下へ / 3: scale が線形に減り N+fade で消える / 4: スリープした tick に Rigidbody が外れ、塊が静的形状として当たる / 5: 上限を超えた分だけ古い順に消える / 0: 何も起きない) — `--selftest`
2. 同じシーン 2 本の並走でハッシュ列一致 — `--selftest`
3. `--fracture-demo` の replay 一致 — `replay_verify.bat` の `fracture` job
4. 既存 job 全 PASS、`check_rules.ps1` PASS、WIP 不変

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
