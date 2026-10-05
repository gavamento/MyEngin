# sub-05: 汎用イベントキューと SendEvent

- 依存: sub-04 (同じ BehaviorTreeSystem を触るので直列)
- 状態: 未着手
- 往復: 0

## やること
- spec 4.1.6 のイベントキュー。持ち主は BehaviorTreeSystem (BT 節に配送待ちを入れる。別システムにするなら SimSnapshot の節と SimSources がもう 1 組要るので、分けるなら理由を SELF_EVAL に)。
  - 積む口 (Engine 内の関数。ABI は sub-11): `BtSendEvent(world, sender, target, nameHash, payload)`。tick N に積んだ分を tick N+1 の BT フェーズ冒頭で「配達済み」へ移し、tick N+1 の末で捨てる。
  - 配送順 = 送信元 entity キー → seq。上限 `kBtMaxEventsPerTick` (256)、溢れは捨てて 1 回警告。
  - BB への反映: `.bb.json` のキーの `eventName` (このサブで BB の形式に足す。sub-01 で既に足していれば流用)。宛先が自分 or 全体のとき反映、型ごとの書き方は spec 4.1.6、同じ tick の複数は配送順の最後が勝つ。反映は Abort の監視より前 (spec 4.1.1 の (1))。
  - 「tick N+1 に配られた分」を読む Engine 内の口 (`BtEventCount` / `BtGetEvent` 相当) を作っておく (sub-11 で ABI へ出す)。
- SendEvent ノード (spec 4.1.5)。
- 配送待ちが空ならハッシュは変わらない。

## やらないこと (このサブでは)
- ABI / C# の口 (sub-11 / sub-12)

## 触る場所 (planner の見立て)
- `BehaviorTreeSystem.{h,cpp}`、BB アセット (eventName)、`SimSnapshot` (版 +1)、`BehaviorTreeSelfTest.cpp`

## 受け入れ条件 (このサブ)
1. (spec 8) tick N 送信 → tick N+1 反映、送信元キー → seq の順、上限と警告、全体宛て、BB 反映で LowerPriority の Abort が起きる — `BehaviorTreeSelfTest`。
2. (spec 3 / 8) 配送待ちがある tick 末で保存 → 復元 → 連続実行一致。イベントが無いシーンのハッシュが不変。
3. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
