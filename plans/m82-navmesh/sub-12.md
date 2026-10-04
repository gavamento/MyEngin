# sub-12: 同じ目的地の渋滞を到着として扱い、Stuck で止めない + レビューの小さな指摘

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## 背景
review-1 #1 (major、planner 宛 = 仕様の穴): 同じ目的地へ向かう複数の Agent が、既定の回避 (avoidanceQuality 2) のままだと Stuck で恒久停止する。
- 2 体では、両方が残り 0.34 / 0.42 m で止まる。
- 4 体では、1 体だけ Arrived で、3 体が Stuck。
- 判定箇所 `NavSystem.cpp:1495-1515` は spec どおりに実装されているので、直すのは仕様の側。

spec 4.1 を、次の 2 点に改めた。
- Stuck は止めない (表示と通知だけ)。
- 目的地の近くにいる、または到着済みの Agent に接していれば、到着として扱う。
あわせて、coder 宛の小さな指摘 #4〜#7 もここで直す。

## やること
1. **Stuck は止めない** (spec 4.1): Stuck にしても dtCrowd の移動目標と moveInput を保ったまま押し続ける。前進 (残り距離が基準から max(radius/4, 1 cm) 以上縮む) が戻ったら `Moving` へ自動で戻す。WARN は Stuck に入った tick に 1 回だけ出す。`EngineAPI.h:715` 付近のコメント (Stuck から戻らない) を新しい規則に合わせる。
2. **渋滞を到着として扱う** (spec 4.1): 前進が 60 tick 止まった Agent が次のどちらかを満たせば、`Arrived` にする。
   - (a) 残り距離が max(stoppingDistance, 2 × radius) 以内。
   - (b) 同じ目的地 (差 ≤ stoppingDistance) で既に `Arrived` の Agent に接している (中心の水平距離 ≤ 半径の和 + 余裕。余裕は名前付き定数)。
   (b) は連鎖する。順序はエンティティキー順で決定的にし、同じ tick の中で連鎖させるか次の tick に持ち越すかを決めて理由を書く。部分経路の到着 (既存) とは両立させる。
3. **SelfTest**: 回避あり (既定) で、2 / 4 / 8 体が同じ目的地へ向かうと、全員がいずれ `Arrived` になること。止まり続けたり、恒久的に Stuck のままの Agent が残らないこと。
   - 塞いでいた壁 (Obstacle) を消すと、Stuck の Agent が `Moving` へ戻って到着すること。
   - 渡りの途中で復元しても連続実行と一致すること (Nav 節にカウンタがあるので維持できているか)。
   - review-1 のプローブ (2 体・4 体) と同じ条件で再現して、直ったことを示す。
4. (#4、minor) `RestoreSimSnapshot` の「どこで失敗しても現世界は無傷」(`SimSnapshot.cpp:554`) を Nav 節が破っている。直し方は次の 2 案から選ぶ。
   - (i) World を差し替える前に、Nav 節の検証で失敗しうる点 (.mnav が読めない、書式) をすべて潰す。
   - (ii) 差し替え後の失敗は「World は変わったが Nav は空」として明記し、`EngineLoop.cpp:1033` 側の扱いを合わせる。
   まず (i) を検討し、無理なら (ii) にして理由を書く。
5. (#5、minor) ベイク結果は、今は Surface の Inspector が描かれているときにしか取り込まれない (`TakeResult` の呼び出し元は `InspectorWindow.cpp:1819-1821` だけ)。選択に関係なく確定するよう、取り込みを Editor の毎フレームの処理 (例: EditorApp のフレーム更新) へ移す。ベイク中に別のエンティティを選んでも .mnav と SceneView が更新されることを、SelfTest か手順で確かめる。
6. (#6、minor) 古いコメントを現状に合わせる。
   - `Components.h:1880` と `Components.cpp:1389`: M82f の UI の記述
   - `Components.h:1964` と `NavSystem.h:57`: y 回転だけの Box は回転箱で切る
   - `PATCHES.md:5`: パッチの件数 (4 件)
   - `PATCHES.md:61`: frand の現在の方式
7. (#7、minor) ADR-023:393 の「通常ビルドは警告 0」を「M82 のファイルからの警告は 0 (既存の ProjectComputeRunnerSelfTest.cpp の C4127 は通常ビルドでも 14 件出る)」と正確に書く。
8. ADR-023 の Stuck / 到着の節と、engine_spec.md 10.9、docs\engine-feature-guide.md 9.4 を新しい規則に合わせる。

## やらないこと (このサブでは)
- 高さの補正 (sub-11)
- ABI の版を上げること (署名は不変。コメントだけ直す)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Navigation\NavSystem.cpp` (`:1495-1515` 付近の到着と Stuck の判定)、`NavAgentSelfTest.cpp`
- `src\Engine\Engine\Replay\SimSnapshot.cpp`、`EngineLoop.cpp:1033`
- `src\Editor\Windows\Scene\InspectorWindow.cpp`、Editor のフレーム更新箇所、`NavBakeService`
- `Components.h/.cpp`、`NavSystem.h`、`external\recastnavigation\PATCHES.md`、`src\Shared\EngineAPI.h` (コメント)
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`docs\engine-feature-guide.md`

## 受け入れ条件 (このサブ)
1. (spec 20) 2 / 4 / 8 体、回避あり: 全員が `Arrived`。塞ぎを消すと Stuck → Moving → Arrived。— NavAgentSelfTest
2. #4〜#7 が直っている (差分と、#5 の確認手順)。
3. replay_verify 全 15 ジョブ PASS。両構成の selftest で Nav 系は ALL PASS。shot_verify は nav が PASS、既知の 5 枚は値が不変。焼き直した期待値は「旧値 → 新値、理由」を列挙する。
4. 警告 0、check_rules 0。

## 検証コマンド
- 両構成ビルド、`Editor.exe --selftest` と `Server.exe --selftest` を両構成で直列実行
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
