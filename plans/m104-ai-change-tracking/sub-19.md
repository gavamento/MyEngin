# sub-19: プレハブ伝播の表示、大きいシーンの計測 (M104t)

- 依存: sub-18 (sub-18 を外した場合は sub-17)
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。Phase 6 の最後のサブ (この後は配布の sub-20 だけ)。

## やること

arch §6.4、§11。要求 §7.4、§14.1。

1. `PrefabPropagation`: プレハブのベースが変わったとき、そのプレハブを参照しているインスタンスと、override していないフィールドから影響範囲を計算して `scene_changes` に記録する。原因の Change を `caused_by` で結ぶ。**ディスク上のシーンファイルには現れない変更**なので、サービスは「影響を受けるはず」の範囲を計算するだけで、実際の値は Editor Snapshot があるときだけ示す (arch §6.4)。入れ子のプレハブの扱いを着手時に決める (直属のベースだけにとどめるなら、その限界を表示する)。
2. 表示: 伝播による変更を、直接の編集と区別して出す (一覧、Hierarchy のバッジ、Inspector)。原因の変更へ移動できる。復元は原因の側へ案内する (restore §3.3)。
3. 計測: 大きいシーン (エンティティ数を段階的に増やした生成シーン) で、ベースライン、構造 diff、一覧の取得、オーバーレイの描画、pipe の往復の時間を測り、結果を文書に残す。**推測で最適化しない** (AGENTS.md §3.5)。計測の結果、問題のある箇所だけを軽くする (可視のエンティティを優先、オーバーレイの表の引き方、など)。
3b. sub-22 の残り: 数万ディレクトリのツリーでの、監視の分け方の計画づくりの時間と監視の本数 (上限 256 本を超えて再帰 1 本へ戻る条件) を測る。1 つのディレクトリへの一括作成で取りこぼしを再現できる条件が見つかれば、自前の `ReadDirectoryChangesW` (spec §2 D34 の第 2 の手) を別サブとして提案する。
3c. Editor 側の計測 (sub-24 / sub-25 の残り): 実サービスから 1 万行相当が届いたときの `Poll()` 1 回の実時間、数万件の変更があるシーンでの Scene View の箱の候補作り (毎フレーム `FindByFileId`) の時間、箱の可視の判定を原点から AABB に変えた場合の費用。
4. 性能の目標値を、計測値を根拠に arch §11 に置く。
5. 仕上げ: `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` の進捗表、ADR-039 の状態を「実装済み」に、`docs\test_checklists.md` の手動確認の項目、`engine_spec.md`。要求 §16 の全項目について、どのテスト / 手動確認で確かめたかの対応表を `docs\ai-change-tracking\` に残す (計画「検証」の最後の行)。

## やらないこと (このサブでは)

- 計測の根拠が無い最適化。インクリメンタルな構造 diff などの大きい作り直し (必要と分かったら別サブとして提案する)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\scene_diff.rs` (伝播)、`tests\`
- `C:\HAL\MyEngin\src\Editor\ChangeTracking\`、Hierarchy / Inspector のバッジ
- 文書: `architecture.md` §11、対応表、計画、ADR-039

## 受け入れ条件 (このサブ)

1. プレハブのベースのフィールドを変えると、そのプレハブのインスタンスのうち override していないものに `PrefabPropagation` が出て、override しているものには出ない。`caused_by` が原因を指す → `cargo test` (要求 §16.3 の 5 番目)。
2. 伝播による変更と直接の編集が、一覧とバッジで区別して表示される → `changetracking` スイート + `--screenshot`。
3. 伝播の変更に対して復元を選ぶと、原因の変更へ案内される (伝播そのものは戻せない) → スイート。
4. 計測の手順・環境・値が文書にあり、同じコマンドで取り直せる。軽くした箇所は前後の値がある → 文書。
5. 要求 §16 の全項目の対応表があり、「自動テスト / 手動確認済み / 未確認」が区別されている。未確認を成功扱いにしていない → 文書。
6. A20 (全体の横断): `tools\check_rules.ps1` が 0 error、`Editor.exe --selftest-full` (Debug / Release) が通る、`tools\replay_verify.bat` が追跡の有効 / 無効で一致、`tools\mcp_verify.bat`、`tools\track_verify.bat`、`cargo test` が通る、ABI・SimSnapshot・TypeId が変わっていない (spec §2 D20)。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`tools\mcp_verify.bat`
- ビルド (Debug / Release)、`Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`

## 実装メモ (coder が追記)

## フィードバック履歴
