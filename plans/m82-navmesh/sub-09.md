# sub-09: 文書と全体検証

- 依存: sub-08 (sub-04 も OK であること)
- 状態: 未着手
- 往復: 0

## やること
spec 5. の 11, 12, 15, 16, 17, 18 (c)。

1. `docs\adr\ADR-023-navmesh.md` を仕上げる (sub-01 の下書き + 各サブの決定: 復元方式、**将来の実行時再ベイクの足し方 (どの関数を・どの tick のどのステップで呼ぶか・何が未実装か)**、塗りの描画レーン、TypeId、`.mnav`、エリアコストを Surface に置いた理由、CC 必須、Link の渡り、Obstacle の同期確定、除外した案と理由、AcousticNav との役割分担)。
2. `engine_spec.md` に NavMesh 節 (コンポーネント、Tick 位置、スナップショット、ABI 表の版)。`docs\engine-feature-guide.md` 9.3 の「NavMesh ベイクとは区別します」を書き換え。`AcousticNav.h:22-24` のコメントを「音響ナビは NavMesh を使わない (汎用移動は ADR-023 の NavMesh)」の意味に整える。`docs\test_checklists.md` に NavMesh の手動確認項目。`CLAUDE.md` / `AGENTS.md` に末尾 TypeId や検証表の記述があれば更新 (無ければ触らない)。`plans\m75-ugui.md` の TypeId 注記。
3. 全体検証: 両構成 `/p:MyeWarnAsError=true`、`--selftest` 両構成、`check_rules`、`replay_verify` (全ジョブ)、`shot_verify`、Runtime.exe `--nav-demo` 実走 + スクショ、エディタで Create → 4 項目 → Bake → 再生で Agent が歩くのを `--screenshot` で確認。

## やらないこと (このサブでは)
- 機能追加

## 触る場所 (planner の見立て)
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`、`src\Engine\Engine\Acoustic\AcousticNav.h` (コメントのみ)、`plans\m75-ugui.md`

## 受け入れ条件 (このサブ)
1. (spec 16) 文書の差分が spec 4. と実装に一致 (参照先のパス・番号が実在)。
2. (spec 11, 12, 15, 17) 全検証 PASS (shot_verify の golden `nav` を含む)。未実施があれば理由とリスクを書く。

## 検証コマンド
- 上記 3. の全部

## 実装メモ (coder が追記)

## フィードバック履歴
