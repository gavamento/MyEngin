# sub-05: 汎用イベントキューと SendEvent

- 依存: sub-04 (同じ BehaviorTreeSystem を触るので直列)
- 状態: OK (M85e としてコミット、ハッシュは台帳)
- 往復: 2

## やること
- spec 4.1.6 のイベントキュー。持ち主は BehaviorTreeSystem (BT 節に配送待ちを入れる。別システムにするなら SimSnapshot の節と SimSources がもう 1 組要るので、分けるなら理由を SELF_EVAL に)。
  - 積む口 (Engine 内の関数。ABI は sub-11): `BtSendEvent(world, sender, target, nameHash, payload)`。tick N に積んだ分を tick N+1 の BT フェーズ冒頭で「配達済み」へ移し、tick N+1 の末で捨てる。
  - 配送順 = 送信元 entity キー → seq。上限 `kBtMaxEventsPerTick` (256)、溢れは捨てて 1 回警告。
  - BB への反映: `.bb.json` のキーの `eventName` (このサブで BB の形式に足す。sub-01 で既に足していれば流用)。宛先が自分 or 全体のとき反映、型ごとの書き方は spec 4.1.6、同じ tick の複数は配送順の最後が勝つ。反映は Abort の監視より前 (spec 4.1.1 の (1))。
  - 「tick N+1 に配られた分」を読む Engine 内の口 (`BtEventCount` / `BtGetEvent` 相当) を作っておく (sub-11 で ABI へ出す)。
- SendEvent ノード (spec 4.1.5)。
- 配送待ちが空ならハッシュは変わらない。

- (round 1 VERDICT で追加) 配達 (`DeliverPending`) を TickRunner のフェーズ 3 (スクリプトの Update) より前、`stepSim` のゲートの中で呼ぶ。BT の Update からは配達を外し、反映 (ApplyEventsToBlackboard) だけ残す。HeadlessSim / 再シム / What-if の経路も同じ TickRunner を通ることを確認する。TickRunner を変えるので `tools
eplay_verify.bat` を回す (WriteBt が BT の無い構成でも節を書く変更も snapshot stress で通るか確かめる)。

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
round 1 (SELF_EVAL の要約):
- BehaviorTreeSystem に pending_ (sim 状態、BT 節の末尾に `Count + BtEvent 列`) と delivered_ (非 sim) を追加。`SendEvent(tick, sender, target, nameHash, vec3, value, int)` / `EventCount(self)` / `GetEvent(self, i, out)` / `PendingEventCount()` は BehaviorTreeSystem のメンバ (BtSendEvent(world, ...) の自由関数にはしていない)。
- 配送: Update 冒頭の DeliverPending(tick) が sentTick < tick の分だけを配る (スクリプト層が同じ tick の BT より前に積んだ分は次の tick)。配送順 = 送信元キー → seq。残りは seq を振り直す。上限 256 は pending_ の総数で、積む時点で捨てて 1 回だけ警告。
- BB 反映は StepOwner の MonitorNode の前 (ApplyEventsToBlackboard)。送信元 null の Entity キーは未設定。
- SendEvent ノード (Task): params eventName (新設 BtParamType::String、63 バイトまで) / target (Self / All / Entity) / floatValue / intValue、keys target / vector。名前が空・Entity 宛てで未設定・Vector キー指定で未設定は Failure、キュー溢れは Success。
- StateHash は配送待ちが空なら何も足さない。HasHashableState は pending_ も見る。snapshot v37。
- SimSnapshot.cpp の WriteBt (BT 無し構成) にも pending の 0 件を書く (これを忘れると Nav 系 selftest の復元が壊れた)。AcousticAudioSelfTest の版固定値を 37 へ。
- `[search] stuck` の診断ログは 1 実行 2 行なので残した。
- 検証: Debug / Release ビルド 0 警告、Editor --selftest Debug (exit 0、既知 flake 0 件、ダイアログなし) / Release (exit 0)、Debug Server --selftest exit 0、check_rules 0。replay_verify は TickRunner / シーン経路を触っていないので未実行。

round 2 (FIX_REQUEST #1〜#3):
- #1: DeliverPending を public にし、TickRunner のフェーズ 3 (スクリプト) の前、`stepSim && ts.behaviorTree` で呼ぶ。Update からは配達を外し BB 反映だけ。テストの Sim::Step / NavSim / 各 step lambda も同じ順 (配達 → Update)。新テスト: BT より前の層 (Sim::beforeBt) から EventCount / GetEvent が読める・BB は同 tick の BT フェーズで入る・一時停止中 (Sim::paused) は配達せず再開 tick に配る。
- #2: tools\replay_verify.bat → exit 0、16 ジョブ全 PASS (snapshot stress 含む)。golden は触っていない。
- #3: assert の行番号は再現できず未特定 (推定経路は Nav 復元テスト失敗後の添字アクセス)。
- 検証: Debug / Release ビルド 0 警告、Editor --selftest Debug (exit 0、FAIL 0、ダイアログなし) / Release (exit 0)、Debug Server --selftest exit 0、check_rules 0。

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must: 配達をスクリプトより前へ (spec 4.1.6 変更) と replay_verify の実行。他の逸脱・追加は承認
- round 2: VERDICT OK (planner)。must 1 (配達を tick の頭へ、一時停止中は配らない) と must 2 (replay_verify 16 ジョブ PASS) を確認。nit 3 は「推定・未確認」の注記つきで司会の別件へ
