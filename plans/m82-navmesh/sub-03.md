# sub-03: NavMeshAgent と dtCrowd の移動 + SimSnapshot の Nav 節 + `nav` ジョブ

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
spec 4.1 (Agent、Tick の位置、AgentBrain 共存、エッジケース)、4.4 (N1〜N3)。

1. `NavMeshAgentComponent` を末尾 append (hash 対象)。Add Component で CC が無ければ CC も足す (1 Undo)。
2. `NavSystem` (sim 側、`src\Engine\Engine\Navigation\`): Surface ごとに dtNavMesh / dtTileCache / dtNavMeshQuery / dtCrowd (容量 既定 128) を持つ。TickRunner のフェーズ 3.4 の後・3.5 の前に独立の `if (stepSim)` ブロック。順序は spec 4.1 のとおり (Agent はエンティティキー順で同期、CC の実位置を crowd へ、`dtCrowd::update(1/60)`、望む速度 → CC.moveInput、状態 → Agent)。
3. **sub-01 で決めた復元方式**で (タイル差し替え済みの状態でも成り立つ形のまま。spec 4.4 F4) SimSnapshot に Nav 節を足す (`kSimSnapshotVersion` 24 → 25)。World hash にも Nav の外部状態を畳む (中身があるときだけ、XPBD / 音響と同じ content-gated)。Presence gate: NavMesh 系が無ければ RNG もハッシュも変わらない。
4. **計測 (sub-01 の申し送り)**: `--nav-demo` 実機で (i) Nav 節の capture バイト数と時間 (ロールバックの毎 tick capture 約 148 KB への上乗せ) (ii) 最悪 tick の `dtCrowd::update` 時間 (経路要求を同じ update で完走させるパッチの影響)。(i) は 128 体換算で +45% の見込み — 差分化するかの判断材料として SELF_EVAL に数字を書く。(ii) が重ければ 1 tick の経路要求を**件数で**絞る (時間で絞らない)。
4b. CC が越えられる段差の実測 → Surface の `maxClimb` 既定を確定 (spec 2. #5、R3)。結果を SELF_EVAL に書く。
5. `--nav-demo` (ShowcaseScenes の `kShowcases` に 1 行、`DemoContent.cpp`、editorOnly でない) と replay_verify の `nav` ジョブ (`:job_nav`、`-Jobs` 一覧、`:diagnose`、件数表示)。デモには Agent 数体と固定の目的地切り替え (tick で決まる) を入れ、`.mnav` はデモ用に用意 (assets にコミットするかデモ構築時にベイクするかは coder が決めて理由を書く。後者なら Debug / Release 一致は sub-02 で担保済み)。
6. デバッグ描画に Agent の経路 (コリドー / コーナー) を足す。
7. インスペクタ: 状態表示、寸法不整合・AgentBrain 併用・CC 無しの警告。

## やらないこと (このサブでは)
- Obstacle / Modifier / Link、ABI

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`src\Engine\Engine\Navigation\NavSystem.{h,cpp}`、`src\Engine\Engine\Loop\TickRunner.cpp` (340-387 付近)、`TickServices` (NavSystem の所有)、`src\Engine\Engine\Replay\SimSnapshot.{h,cpp}` / `WorldHasher.cpp`、`src\Engine\Engine\Demo\ShowcaseScenes.cpp` / `DemoContent.cpp`、`tools\replay_verify.bat`、`.github\workflows\ci.yml` のコメント (ジョブ数)、InspectorWindow、Localization
- 罠: AgentSystem は `ts.acoustic != nullptr` ゲートの中 (`TickRunner.cpp:340-386`) なので相乗りしない。位置は前 tick の WorldMatrix (`TickRunner.cpp:345-346`)。

## 受け入れ条件 (このサブ)
1. (spec 6) 段差・坂を越えて `Arrived`。届かない目的地は部分経路 + `pathPartial`、NavMesh 外は `NoPath`。CC 無しは `Inactive`。— SelfTest
2. (spec 7) すれ違いの最小距離: 回避ありで 2 体の半径和以上、回避なしで下回りうる。— SelfTest
3. (spec 3 実装) SelfTest: tick T で SimSnapshot を撮って新しい NavSystem へ restore → N tick 後のハッシュが連続実行と一致。
4. (spec 11, 12) replay_verify 全ジョブ PASS (新 `nav` を含む、snapshot stress と Server.exe 含む)。既存 golden 不変。
5. 0 警告、check_rules 0、selftest に新 FAIL なし。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`
- `Runtime.exe --nav-demo --screenshot` で経路描画を目視

## 実装メモ (coder が追記)

## フィードバック履歴
