# sub-03: オクルージョンを Forward へ広げ、デバッグ表示と統計を仕上げる

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
spec §4.1.4、§4.3。
1. sub-02 の 2 フェーズを Forward の不透明に通す (共通部は sub-02 のパスを使い回す。経路ごとに重複実装しない)。
2. `--hzb-debug` に max-Z ピラミッドの表示と「オクルージョンで落とした物の AABB」(EditorLinePass 等) を足す。既存の min-Z 表示は残す。
3. ProfilerWindow のオクルージョン欄を仕上げる (フェーズ別、ビュー別)。

## やらないこと (このサブでは)
- 影・半透明へのオクルージョン。

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Pipeline\ForwardPath.cpp`、sub-02 のパス
- `--hzb-debug` の実装箇所 (Deferred のデバッグ表示)
- ProfilerWindow

## 受け入れ条件 (このサブ)
1. Forward 経路の `render_bench` で occluded > 0、ON/OFF の画素差 0。 — `--screenshot` A/B + img-diff
2. `--hzb-debug` のスクショで max-Z と落とした物が見える。 — スクショのパスを報告 (目視はユーザー)
3. golden 全 PASS、`tools\check_rules.ps1` PASS。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、A/B スクショ

## 実装メモ (coder が追記)

## フィードバック履歴
