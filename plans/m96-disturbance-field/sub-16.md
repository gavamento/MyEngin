# sub-16: 仕上げ (ADR-036・文書・カタログ・全検証)

- 依存: sub-15
- 状態: 未着手
- 往復: 0

## やること

spec §5 の 26〜28。

1. `docs\adr\ADR-036-disturbance-field.md` (ADR-035 の体裁): 背景 / 決定 (spec §2 の結論と却下した案。地面の霧の専用描画 (板のレイマーチ、R12) と局所ライトの ON/OFF (R11)・太陽光の bool・フロクセルの自動有効 (場だけのモード)・3D 4 個の描画・起伏に沿う層を含む) / 計測値 (sub-04 / sub-07 の ms とスナップショットのバイト数、sub-03 の地面の霧の GPU 時間、sub-15 の局所ライト 0 / 1 / 8 本の GPU 時間、sub-07 のフロクセル注入の GPU 時間、sub-10 の焼き込みの ms、タイムトラベルで遡れる秒数の目安。spec §7 R3) / 制限 (R5 / R7 / R8、物理を押さない、太陽光は影なし) / 公式資料 (spec §2 #3 の UE Niagara Fluids の URL)
2. `engine_spec.md`: 新しい節 (sim の契約 = Tick 位置・内容ゲート・DFLD 節・キューの消費規則、描画の受け渡し、ABI v35)。§13 の ADR 一覧に 036。§11 (Debug / Release 一致) に規則 18 の一行
3. `docs\engine-feature-guide.md`: 作り手向けの使い方 (霧の広場の作り方、源の付け方、斬撃のアニメイベント、`FieldSample` で足元の霧を読む例、フロクセルが必要なこと)
4. `docs\test_checklists.md`: 手動確認の項目 (Play / Stop で霧が戻る (sub-02 では Editor の操作で確認していない)、Asset Browser のサムネイル (別の RenderSystem) が地面の霧に染まらない (sub-03 では CLI で撮れなかった)、Scene View で見える、追従の層が付いてくる、ヒットストップで霧も止まる)
5. エディタ: Add Component のカタログの分類 (既存の分類の仕組みがあれば)、Profiler に `disturbance` のスコープが出ることの確認
6. `AGENTS.md` / `CLAUDE.md` は変えない (規則 18 は check_rules に入っており、AGENTS §4.2 の決定論の規則の範囲内)
7. (sub-01 からの申し送り) 規則 18 の自己検査 `tools\check_rules_selftest.ps1` を呼ぶ所が無い。`tools\check_rules.ps1` から呼ぶか、CI (と replay_verify の rules ジョブ) に載せる
8. 全検証 (spec §5 の 28)

## やらないこと (このサブでは)

- 機能の追加・挙動の変更 (見つけた不具合は「不安・質問」へ。直すなら planner が判定)

## 触る場所 (planner の見立て)

- `docs\adr\ADR-036-disturbance-field.md` (新規)、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`
- エディタのコンポーネントカタログ (`grep -rn "SpringFollow" src\Editor`)

## 受け入れ条件 (このサブ)

spec §5 の 26, 27, 28, 30。

- 26: `git diff 2961ba3 -- <SampleWind と空力の 3 か所のファイル>` に該当関数の変更が無い (ファイルと関数名を SELF_EVAL に列挙)。replay `physics` / `joints` / `fracture` PASS
- 27: 文書 4 点の差分。文書中のパスがリポジトリ相対か絶対パスの規約 (規則 17) に従う
- 28: `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full` FAIL 0、`tools\check_rules.ps1` の error が着手前から増えない、`tools\replay_verify.bat` 全ジョブ PASS、`cmd /c C:\HAL\三校\tools\verify.bat` が既存の FAIL 2 件以外増えない (増えたら件名を SELF_EVAL に)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブが M96 の最後のまとめ実行。受け入れ 28 の `--selftest-full` (Debug / Release) と `tools\replay_verify.bat` を実行する。

- 上の 28 の全部

## 実装メモ (coder が追記)

## フィードバック履歴
