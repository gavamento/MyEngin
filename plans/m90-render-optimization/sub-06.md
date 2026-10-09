# sub-06: 描画側の並列化と URO (パレット・LOD 選択・カスケードの並列、ビュー間キャッシュ)

- 依存: sub-04, sub-05
- 状態: 未着手
- 往復: 0

## やること
spec §4.1.5、§2 #9 #10 #11。
1. `CollectDrawables` のステージ 2 (並列) に LOD 選択とスキンの視錐台判定を入れる (sub-04/05 で直列に置いたものを移す)。
2. パレット評価を並列化: 直列で「パレットが要る項目」を数えてスロット (事前確保した互いに素の出力) を割り当て → `ParallelRanges` で評価 → 直列で index 順にキューへ。`skinPalettes_` のポインタ安定性 (emplace_back で再確保されない) を構造で保証する。WARN の 1 回出し (`boneOverflowWarned_`) は並列段で書かない。
3. カスケードごとの影のカリングを並列 (カスケード × 範囲、出力はカスケード別)。
4. URO: screen-size が閾値未満のスキンは `updateInterval` tick に 1 回だけパレットを作り直す。作り直す tick = `(tickIndex + entity.index) % interval == 0`。間は前のパレット、補間なし。ラグドール作動中・クロスフェード中は間引かない。閾値と間隔の既定表をコメントの根拠付きで。`RenderSystem::enableAnimUro` (既定 true) とエディタのメニュー (`Tr()`)。
5. パレットのキャッシュをビュー間で共有。キー = エンティティ + ポーズ入力 (`SamePoseInputs` の等価) + 補間 alpha + URO の更新 tick。描画専用 (sim から見えない)、エンティティの破棄・シーン切り替えで捨てる。
6. 統計: パレット評価数 / 再利用数。
7. (sub-02 VERDICT から) オクルージョンのフェーズ 2 は、run / 単発ごとに状態設定と CB 更新をもう 1 回出す (どれを間引くかを CPU が知らないため)。ユニークなメッシュ・材質が多いシーンでは、CPU の提出コストが最大で約 2 倍になる。`render_bench` に「ユニークなメッシュ/材質が多い」変種を足し (CLI の引数で切り替え。既定のシーンは変えない)、GBuffer の提出にかかる CPU 時間をオクルージョン ON/OFF で計って報告する。OFF より明らかに重ければ planner が対処を決める (候補: 可視ビットの読み戻しで、フェーズ 2 に候補が無い run を飛ばす。数フレーム遅れの読み戻しは描画判断に使わない規約なので、使うなら spec の変更が要る)。

## やらないこと (このサブでは)
- sim 側のポーズ評価 (部位追従・IK・ラグドール) に触ること。
- deferred context。

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (`CollectDrawables` ステージ 2/3、`RenderCascadeShadows`)、`RenderSystem.h` (キャッシュ・設定)
- `src\Engine\Engine\Animation\SkinningSystem.h` (`SamePoseInputs`)
- `src\Engine\Core\Jobs\JobSystem` (使うだけ。API を足すなら理由を書く)
- tickIndex の取り方: 描画へ渡っている tick 番号 (実時間・描画フレーム数を使わない)

## 受け入れ条件 (このサブ)
1. `render_bench` と既存のスキン入りデモで、jobs あり / `--no-jobs` のスクショの画素差 0。 — img-diff
2. URO: dump で遠いキャラのパレット評価数が減り、近いキャラは毎 tick 評価。同じ tick を 2 回撮ると同じ絵。 — dump、`--screenshot` 2 回
3. 複数ビュー (Scene View + Game View) で同じキャラのパレットがフレームあたり 1 回だけ評価される。 — selftest または dump のビュー別欄
4. selftest: URO の更新 tick 関数、キャッシュのキーの等価 (ポーズ入力が変わったら必ず作り直す)。
5. 既存 golden 全 PASS、`tools\check_rules.ps1` PASS、`tools\replay_verify.bat` PASS (描画の変更が sim に漏れていない)。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- A/B スクショ (`--no-jobs`)、dump

## 実装メモ (coder が追記)

## フィードバック履歴
