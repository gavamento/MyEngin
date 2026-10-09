# sub-08: 文書 (ADR-029、engine_spec の移動、test_checklists) と全体の検証

- 依存: sub-03, sub-06, sub-07
- 状態: 未着手
- 往復: 0

## やること
spec 受け入れ条件 13、全体。
1. `docs\adr\ADR-029-*.md`: LOD (共有 VB + IB 範囲、オプトイン、選択式、クックの無効化) とオクルージョン (2 フェーズ、max-Z、viewKey 別、却下した 1 フェーズ案と理由)、URO を描画側に置いた理由。番号は着手時に `docs\adr\` の末尾を確認 (sub-07 が ADR-028 を使う)。
2. `engine_spec.md` §12.3 の「mesh LOD and GPU occlusion culling」を §12.2 の表へ移し、§12.2 時点の番号 (kCookVersion 6、ADR-028/029) を更新。描画の章に LOD・オクルージョン・URO・新しい CLI (`--no-occlusion`、`--render-stats-dump`) を足す。
3. `docs\test_checklists.md` に手動確認 (LOD の遠景、オクルージョンの欠けなし、画面外キャスターの影、URO の遠いキャラ) を足す。
4. 全体の検証を回し、結果を SELF_EVAL に書く。

## やらないこと (このサブでは)
- コードの変更 (検証で見つかった不具合は「不安・質問」に出し、planner が差し戻し先を決める)。

## 触る場所 (planner の見立て)
- `docs\adr\`、`engine_spec.md`、`docs\test_checklists.md`

## 受け入れ条件 (このサブ)
1. 文書の参照先が実在し、番号が実コードと一致。
2. `Editor.exe --selftest` Debug / Release、`tools\check_rules.ps1`、`tools\replay_verify.bat` (jobs A/B を含む) が PASS。
3. `render_bench` の最終 dump (LOD / オクルージョン / URO の ON・OFF の表) を SELF_EVAL に貼る。実機の ms は参考値として併記。

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
