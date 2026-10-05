# sub-02: Decorator 5 種と Abort、手数の上限

- 依存: sub-01
- 状態: OK (M85b としてコミット、ハッシュは台帳)
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
SELF_EVAL: sub-02 (round 1)
実装:
  - BehaviorTreeLibrary.h/.cpp: BtDecoratorKind 5 種 / BtDecoratorTypeInfo / BtDecoratorDef (kind, key, params, slot) と BtNodeDef::decorators、表 kDecoratorTypes。"decorators" の読み書き (key は BlackboardCondition だけ)。BtLinkAsset が欄を割り当てる (ノード数 + Decorator を並び順に 1 欄ずつ = stateSlotCount)。上限 kBtMaxDecoratorsPerNode = 8 / kBtMaxStateSlots / kBtMaxRepeatCount。読み込み時に、親が Selector でない LowerPriority / Both を警告。params の読み書きを ReadParamList / WriteParamList へ共通化。
  - BehaviorTreeSystem.cpp: Visit を VisitBody (従来の switch) と VisitDecorated (入るとき全条件を評価 → ArmDecorator → RunLevel → 終わったら FinishDecorators) に分割。RunLevel が Invert / Timeout / Repeat を外側から順に包む。MonitorNode が StepOwner の Visit(root) の前に前順で Self / LowerPriority / Both を評価。AbortNode は NodeActive (本体 or Decorator 欄が active) で判定し、子孫 → 自分の記録 → FinishDecorators (Cooldown の計時開始) → Finish の順。AbortBody は Timeout の打ち切り用。
  - 欄の使い方: BlackboardCondition = phase に最後の評価結果 (Finish しても残す。LowerPriority の「変化」検出用)、Cooldown = counter に入れるようになる tick (終了 / Abort の tick + ticks)、Repeat = counter に終えた回数、Timeout = counter に打ち切る tick。
  - BehaviorTreeSystem.h: SetAbortTrace (検査用。Abort を受けたノード id の記録先。sim 状態ではない)、BtNodeState のコメント更新。
  - BehaviorTreeSelfTest.cpp: 未知の Decorator の既存テストを差し替え、Decorator 読み書き / BlackboardCondition の比較 / Invert / Cooldown (Success・Failure・Abort 後) / Repeat (有限・無限・Failure で抜ける・Timeout との周回) / Timeout / Abort 4 種 + Sequence 下の LowerPriority + 変化のときだけ働く確認 / 手数の上限と警告 1 回 / Cooldown・Timeout・Repeat の途中での保存 → 復元 → 120 tick の毎 tick ハッシュ一致、を追加。
仕様との差分:
  - [追加] Decorator の JSON 形式: {"type", "key" (BlackboardCondition のみ), "params"{...}}。BlackboardCondition の params = query / intValue / floatValue / abort。spec は欄の形を定めていなかったので、ノードの params と同じ表引きにそろえた (sub-09 のエディタも同じ表を引ける)。
  - [追加] 意味の細部を埋めた: (a) 条件 (BlackboardCondition / Cooldown) はノードに入るときに 1 回だけ全部評価 (Repeat の周回では再評価しない)。(b) Timeout は「その tick に子が終わるなら終わりを優先」、切れるのは入った tick + ticks の tick。(c) Timeout は Repeat の内側にあれば周回ごとに掛け直す。(d) Repeat の既定 count = 2、Timeout の ticks は 1 以上。(e) Cooldown は終了 / Abort の tick + ticks が「入れるようになる tick」(ticks = 0 は計時なし)。(f) 未設定・キー無し・BB 無しでの大小比較、Vector / Entity の大小比較は偽。Bool の IsSet は true のとき、Entity の IsSet は World::IsAlive。(g) LowerPriority は「偽 → 真に変わった tick」だけ働き、同じノードの他の条件が通らない間は変化を見送る (チャーン防止)。(h) 条件が偽の Decorator 付きノードは入らず Failure (Abort は記録しない)。
  - [逸脱] 監視 (MonitorNode) の評価は手数 (kBtMaxStepsPerTick) に数えない。ノード数 (<= 1024) で有界なため。
  - [逸脱] snapshot の版は上げていない (v34 のまま)。BT 節のバイト形式と BtNodeState の形は不変で、Decorator は既存の欄に載る。変えたのは読み込み上限 (kBtMaxNodes → kBtMaxStateSlots) だけ。形が変わる扱いなら +1 するので判断してほしい。
  - [追加] 根が終わった次の tick のやり直しは、ノードの欄だけ初期化し Decorator の欄 (Cooldown の計時など) は残す。
検証:
  - Debug Editor build (/p:MyeWarnAsError=true) → 0 警告 0 エラー。Release Editor / Debug Server / Release Runtime も 0 警告。
  - bin\x64\Debug\Editor.exe --selftest → exit 0、BehaviorTree ALL PASS (新規 FAIL 0)。1 回目から既知 flake 5 件 (Fracture weight cache / net V1 LoadPersist・LoadGame) は出なかった (0 回)。
  - bin\x64\Release\Editor.exe --selftest → exit 0、BehaviorTree ALL PASS。
  - bin\x64\Debug\Server.exe --selftest → exit 0。
  - tools\check_rules.ps1 → 0 error 0 warning。
  - UE 規則の照合 (Decorators / Observer Aborts): Web を引いていない。記憶から、未検証。
  - 未実行: replay_verify (このサブは BT のシーン / 実機経路を持たない。sub-13 で実施)、実機 (Runtime) での BT 動作。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜4 の検証済み。細部の解釈を差分欄に出した (特に snapshot 版を上げていない点)
  正しさ: 4 — 5 種 x 代表ケース / Abort 4 種 / 順序 / 手数 / 保存復元を SelfTest で確認。複数 Decorator の組み合わせ (Cooldown + Repeat の入れ子など) は代表のみ
  コード品質: 4 — 既存の Visit* の形を保ち分割。MonitorNode の LowerPriority 部分はやや長い
  テスト: 4 — 新規 30 件超。ミューテーションでの反証 (変化検出を外すなど) は未実施
不安・質問:
  - snapshot v35 にするか (上記)。
  - UE の Loop (Repeat) は子 Failure でも抜けるか、UE の Cooldown が Abort でも計時を始めるかは記憶ベースで未照合。違えば planner が直す。
  - 親が Selector でない LowerPriority の警告は FromJson の読み込み時だけ (エディタの検査エラーは sub-09)。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeLibrary.h
  - C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeLibrary.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeSystem.h
  - C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeSelfTest.cpp
  - C:\HAL\MyEngin\plans\m85-behavior-tree\sub-02.md (実装メモ)
申し送り:
  - 次にコンポーネントか BT 節の形を変えるサブが kSimSnapshotVersion 35 にする (本サブでは上げていない)。
  - 実行状態の欄の並び: [ノード 0..N-1][Decorator (ノード順、上から)]。ライブ表示 (sub-10) は BtDecoratorDef::slot を引く。
  - 本体の Abort 後始末 (MoveTo の停止など) は AbortNode / AbortBody の「ノード固有の後始末」コメントの位置へ足す。AbortTrace が順序検査に使える。
  - 毎 tick の監視は BlackboardCondition の abort != None だけ。sub-03 以降で BB を書く Task が入ると、書いた tick の Visit(root) の前ではなく次の tick の監視で反映される。

## フィードバック履歴
- round 1: VERDICT OK (planner)。解釈で埋めた細部 (a)〜(i) は spec 4.1.2 の補足として承認 (変更履歴)。監視の評価を手数に数えない逸脱は承認 (ノード数で有界、手数の上限は「即終了の木の無限周回」を止めるためのもので監視は周回しない)。snapshot は v34 据え置きを承認 (BT 節のバイト形式不変、v34 の節は Decorator を含み得ない = sub-01 は Decorator 付きを読み込み失敗にしていた)
