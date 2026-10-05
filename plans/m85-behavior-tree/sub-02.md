# sub-02: Decorator 5 種と Abort、手数の上限

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
- spec 4.1.2 の Decorator 5 種 (BlackboardCondition / Invert / Cooldown / Repeat / Timeout) をノード記述子の表に登録し、実行器に組み込む。
- spec 4.1.1 の Abort (None / Self / LowerPriority / Both、OnResultChange のみ、毎 tick の BT フェーズ冒頭に監視中の Decorator を優先順に評価)。親が Selector 以外の LowerPriority / Both は Self として扱い、読み込み時に警告 (エディタの検査エラーは sub-09)。
- Abort の後始末: 深い方から OnAbort。Cooldown は Abort でも計時を始める。Timeout は子を Abort して Failure。
- `kBtMaxStepsPerTick` (256) で止めて次の tick に続ける。1 回だけ警告。
- Cooldown / Repeat / Timeout の計時・回数は BT 節に入る (snapshot の版はノードのインスタンス状態の形が変わるなら +1)。
- UE の公式ドキュメント (Decorators / Observer Aborts) を参照できるなら照合し、spec 4.1.2 の「UE との対応」欄と違えば不安・質問に書く (spec 2. #3)。

- (sub-01 VERDICT より) Decorator の評価は各 Visit* の入口と毎 tick の監視 (根の Visit の前) に置く。Cooldown の計時は「ノードが終わった後も残る」ので Decorator 側の状態欄に置く (ノード側の欄に置かない)。1 Decorator = 1 状態欄。

## やらないこと (このサブでは)
- BB を書く Task (SelfTest は BB を直接書く口を使う)、エディタの検査エラー表示

## 触る場所 (planner の見立て)
- `src\Engine\Engine\AI\BehaviorTreeSystem.cpp` / ノード記述子の表 / `BehaviorTreeSelfTest.cpp`、必要なら `SimSnapshot.h` の版

## 受け入れ条件 (このサブ)
1. (spec 4) 5 種 × 代表ケース (Cooldown の Abort 後計時、Repeat 0 = 無限と Failure で抜ける、Timeout で子が OnAbort を受ける)、Abort 4 種 (Self で部分木が止まる、LowerPriority で右の兄弟が止まって左が走る、Both、Sequence の下の LowerPriority が Self 扱い)、後始末の順序 (深い方から) — `BehaviorTreeSelfTest`。
2. (spec 5) Repeat 無限 + 即 Success の木が 1 tick 256 手で止まり、次 tick に続き、警告が 1 回 — `BehaviorTreeSelfTest`。
3. (spec 3) 計時の途中 (Cooldown 中・Timeout 中) で保存 → 復元 → 連続実行一致。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド (`/p:MyeWarnAsError=true`)、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
