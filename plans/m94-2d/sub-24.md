# sub-24: 文書 (engine_spec / ADR-033 / test_checklists / README)

- 依存: sub-23
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (第 1 章の文書規約、第 8 章)。spec: 5. 40

## やること
1. `engine_spec.md`:
   - §6.15 (新設) 「2D rendering (M94)」: Sprite2D / 整数ソートキー / ソート層 (安定 id、上限 32) / SortingGroup / SpriteMask (ステンシル) / Sliced・Tiled / Light2D のライトバッファと層ごとの群 / ShadowCaster2D (押し出し) / 法線 / Parallax2D / Line2D / SpriteShape2D / PixelPerfectCamera2D (式、TAA・FXAA 無効、ポストは低解像度、UI はフル解像度) / サーフェスシェーダの枝。英語 (既存の節に合わせる)。
   - §10.13 (新設) 「2D assets (M94)」: `.meta` の sprites (サブアセット `#sprite<id>`)、`.spriteanim.json`、`.tilepalette.json` / `.tilemap.json` (セルのビット配置、チャンク、override 表 = sim 状態、presence gate)、`.shape2d.json`、`.spriteshape.json`。
   - §10.5 の末尾に「2D joints = plane2D + Hinge(Z) / Slider / Fixed / SpringJoint」の 1 段落、`Effector2D`。
   - 平面拘束 (`plane2D`) の節 (§10.4 か §10.5 の近く): 拘束点 A/B/D、不変量、z は初期値、異なる z は当たらない、`oneWay` の規則、CC の 2D モード (移動床は kinematic Rigidbody の velocity)。
   - §10.9 の近くに `GridAgent2D` (A* の辺とコスト、4096 展開上限、BT MoveTo の fallback)、Perception の `forwardAxis`。
   - §11.3 の replay ジョブ表に `2d`、§11 の golden 一覧に `2d` / `2d_lights`。
   - §12.2 の表に M94 の行、Scripting 節に ABI の版と 10 本。
2. `docs\adr\ADR-033-2d-lane.md`: 文脈 / 決定 (平面拘束の 3D ソルバ流用、整数ソートキー、アセット保存 + 差分 sim 状態、ライトバッファ乗算 + 押し出し影、ピクセルパーフェクトの RT、スプライト列を Animator のステートに、ソート層の安定 id、ABI 1 回の位置) / 却下案と理由 (spec 2. の表から) / 結果 (sub-01 の不変量、sub-11 の計測値、sub-23 の性能) / 参照 (Unity の URL)。`engine_spec.md` §13 の ADR 一覧に追加。
3. `docs\test_checklists.md` の M94 節 (sub-23 の下書きを仕上げる): 手動確認 (Scene View 2D モードの操作感、スプライトエディタ、タイルパレット、ピクセルパーフェクトの見え方、ライトの見え方)。
4. `README.md` の機能表に 2D レーンの 1 行。`plans\m75-ugui.md` の M75h の注記に「M94n が v31 を使った場合は v32」(実際の版で)。
5. `plans\m94-2d\harness.md` の申し送り欄に残り minor と未確認事項 (司会が仕上げるが、coder が下書き)。
6. 文書のみの変更: ビルドは原則不要 (AGENTS.md 7)。参照先の存在と行番号 / 名前の整合を確認する。

## やらないこと (このサブでは)
- コードの変更 (文書で見つかった不整合は「仕様との差分」に出す)

## 触る場所 (planner の見立て)
- `engine_spec.md` (§6, §10, §11, §12, §13)、新規 `docs\adr\ADR-033-2d-lane.md`、`docs\test_checklists.md`、`README.md`、`plans\m75-ugui.md`、`plans\m94-2d\harness.md`

## 受け入れ条件 (このサブ)
1. (spec 40) 上の全箇所が書かれ、engine_spec の節番号・ADR 番号・ABI 版・TypeId・snapshot 版が**実際のコード**と一致 (grep で確認したログ)。
2. `pwsh tools\check_rules.ps1` 0 (規則 14 の改行など)。
3. 文書内のリンク先が存在する。

## 検証コマンド
- `pwsh tools\check_rules.ps1`
- `git diff --stat` で文書だけが変わっていること

## 実装メモ (coder が追記)

## フィードバック履歴
