# sub-19: プレハブ伝播の表示、大きいシーンの計測、仕上げ (M104t)

- 依存: sub-18
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-16、sub-18 の VERDICT で planner が書き直した)。Phase 6 の最後のサブ (この後は配布の sub-20 だけ)。最後の横断の検証 (A20) もここで回す。
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

arch §6.4、§11。要求 §7.4、§14.1、§16.3 の 5 番目 (Prefab 伝播変更を直接編集と区別できる)。

## やること

### A. Debug のスタックオーバーフローの原因の確かめ (最初にやる。spec §2 D38、D43)

sub-18 の coder が見つけたこと: `EditorApp` に `RenderSystem` を 2 つ直に持たせた版で、Debug の `behaviortree` スイートがスタックオーバーフローした (`unique_ptr` にして解消)。`C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp` 583 行目で `mye::EditorApp app;` が `wWinMain` の**ローカル変数**なので、`--selftest` が `EditorApp` を作る前に戻る経路でも、`wWinMain` のスタックの枠に `EditorApp` の大きさぶんの領域が確保されている見込み (推測)。すると、M104 で `EditorApp` のメンバが増えるほど、Debug の自己テストが使えるスタックが減る。**D38 (Debug の `physics` が開始直後にスタックオーバーフロー) が同じ原因である可能性がある。** planner が 2026-11-16 に、`MYE_SELFTEST_ONLY=behaviortree,physics` の Debug で、physics が今も開始直後に終了コード -1073741571 で落ちることを確かめた。

1. 確かめる (推測で直さない):
   - `sizeof(EditorApp)` を、基点 `ca6a3991` の版と今の版で比べる (`static_assert` か一時のログで。一時の変更はコミットしない)。基点の版は別の作業ツリーでビルドしなくても、ヘッダの差分から見積もれるならそれでよい。見積もりなら見積もりと書く。
   - 今の Debug の exe のコピー (リポジトリの外) を `editbin /STACK:` で大きくして、`MYE_SELFTEST_ONLY=physics` が通るかを見る (sub-18 の coder がしたのと同じ確かめ方)。
2. 通るなら、**`EditorMain.cpp` の `EditorApp` をヒープに置く** (`std::make_unique<EditorApp>()`。作る場所は今と同じ、`--selftest` などが戻った後)。Editor 層の変更で、Engine 層に触れない。その後、Debug の `MYE_SELFTEST_ONLY=physics` と、physics を含む Debug の全スイートを回す。通れば D38 は解消として、spec と sub-14 / sub-16 の「physics を除く」の扱いを外す (文書だけ。過去のサブの結果は書き換えない)。
3. 通らない (スタックを大きくしても physics が落ちる、またはヒープに置いても落ちる) なら、D38 は M104 と別の原因として残し、分かったことを spec の D38 に書き足す。**physics 自体のコード (Engine 層) は直さない。**

### B. プレハブ伝播 (`PrefabPropagation`)

arch §6.4。プレハブのベースが変わったとき、そのプレハブを参照しているインスタンスのうち、override していないフィールドが、ロード時 (`RefreshNonOverridden`) とホットリロード時 (`PropagateBaseChange`) に**メモリ上で**変わる。ディスクのシーンファイルには現れない。

1. サービス (`tools\track`): プレハブのファイル (`*.prefab.json`、`*.actor.json`) の Revision で `PrefabAssetChanged` と中のフィールドの変更が記録されたとき、**そのプレハブを参照しているシーンの文書** (`PrefabInstanceComponent` の `prefabHash` で引く。今のディスクのシーンの JSON を読む。読めないシーンは飛ばして数を出す) の、各インスタンスについて、変わったフィールドのうち**そのインスタンスが override していないもの**を `PrefabPropagation` の行として `scene_changes` に記録する。`caused_by` に原因のプレハブの変更の行を入れる。`before_value` / `after_value` はプレハブの Before / After の値 (インスタンスのライブの値は分からない。Editor Snapshot が無いため)。
   - `prefabHash` と、プレハブのファイルの対応 (ハッシュの作り方) を、着手時にエンジンのコード (`PrefabInstanceComponent`、プレハブのライブラリ) で確かめる。対応が取れない場合は、伝播の行を作らずにその理由を文書に書き、以下の 2〜4 は「対応が取れた範囲」で行う。
   - 入れ子のプレハブ (プレハブの中のプレハブ) は、直属のベースだけを対象にする。入れ子の先までは追わない (限界として出す)。
   - 対象のシーンが多い・大きいときの上限 (読むシーンの数、エンティティの数)。超えたら打ち切りと件数。
   - events から作り直せること (spec §2 D26)。スキーマの版は 3 (足すなら v4)。
2. `scene_overlay` (sub-12) は、伝播の行を「伝播」と分かる形で返す (直接の変更とは別の印)。`scene_diff` も。
3. Editor: Hierarchy のバッジと Inspector の印で、伝播による変更を直接の編集と区別する (別の印・別の色。色は sub-24 の設定に 1 つ足すか、既存の色を薄くするかを決める)。AI Changes 窓の一覧で、伝播の行から原因のプレハブの変更へ移動できる。
4. 復元: 伝播の行は戻す対象にしない (restore §3.3)。「復元…」を選ぶと、原因の変更へ案内する文面を出す (sub-16 の `ClassifySceneRow` が `Unsupported` に落としている分の文面)。

### C. 計測と軽量化 (AGENTS.md §3.5: 推測で最適化しない)

1. 計測する (結果を `C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` に、環境・コマンド・値で):
   - 大きいシーン (エンティティ 2 万個の生成。sub-11 の計測と同じ) で: 構造 diff (既存)、`scene_overlay` の応答の時間、Editor の `SceneChangeOverlay` の取り直しと `RebuildAncestors`、Hierarchy のバッジの描画、Scene View の箱とゴーストの候補作り (毎フレーム `FindByFileId`)、Inspector の印、1 フレームの合計の増分 (追跡なし / ありの比較)。
   - 変更の件数を 10 / 1000 / 2 万で変える。
   - 実サービスから 1 万行相当が届いたときの `TrackClient::Poll()` 1 回の時間 (sub-25 の残り)。
   - 数万ディレクトリのツリーでの、監視の分け方の計画づくりの時間と監視の本数 (sub-22 の残り。`mye_track bench plan` を生成したツリーで)。
2. 見直す (計測の結果で決める):
   - Scene View の箱とゴーストの可視の判定とピックが原点だけ (`SceneChangeWireBoxes` の `ProjectChangePoint` の 1 か所。sub-17 / sub-18 の残り)。AABB で判定する費用を測り、変えるかを決める。変えるなら、ピックと重ねの印の位置も同じ判定にする。
   - 遅い箇所があれば、計測で特定してから直す。直したら前後の値。
3. 性能の目標値を、計測値を根拠に arch §11 に置く (「目標値はまだ置かない」の段落を書き換える)。

### D. 仕上げ

1. **要求 §16 の全項目の対応表**を `C:\HAL\MyEngin\docs\ai-change-tracking\acceptance.md` (新規) に作る。各項目について「自動の試験 (スイート名 / `cargo test` の試験名 / シナリオ名)」「手動の確認 (`docs\test_checklists.md` の節)」「未確認」を区別する。**未確認を成功扱いにしない。** 要求 §18 の完成条件 (機能面 8、安全性 7、検証面 7) も同じ表に。
2. `C:\HAL\MyEngin\docs\adr\ADR-039-ai-change-tracking.md` の状態を「実装済み (配布は M104u)」にし、「影響」と「未決のまま残したもの」を実装に合わせる (サブを足した経緯: sub-21〜27。D38 の結論)。
3. `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md`: 進捗表 (全サブ)、「未決事項」(計画の未決事項 1〜6 の結論)、「申し送り」。
4. `C:\HAL\MyEngin\docs\test_checklists.md`: M104 の手動確認の節を 1 つにまとめた「通しの確認」(追跡を有効にする → AI が編集 → 窓で見る → レビュー → 復元 → シーンの比較) を足す。
5. `C:\HAL\MyEngin\engine_spec.md`: エディタの章とホットリロードの章が実装と合っているか。

### E. 最後の横断の検証 (A20)

- `bin\x64\Release\Editor.exe --selftest-full` が全部通る。Debug は、A で D38 が解消すれば physics を含めて全部、解消しなければ physics を除いて全部 (作業ディレクトリはリポジトリ)。実行中と後に `mye_track.exe` が無い。
- `tools\replay_verify.bat` が通る (A で `EditorMain.cpp` を変えた場合は必ず)。
- `tools\check_rules.ps1` 0 error、`tools\check_rules_selftest.ps1` PASS。
- `cargo test` (track、mcp)、`tools\track_verify.bat`、`tools\mcp_verify.bat` (`tools\mcp\src` を変えていなければ省略可。省略したら書く)。
- ABI・SimSnapshot・TypeId が変わっていない: `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空。`git diff --stat ca6a3991 -- src\Engine` に出るのは `LocalizationTable.inl` と `HotReload\ReloadHub.*` だけ。
- `git status`: 復元による変更なし、`.mye` なし。リポジトリ直下の `imgui.ini` の更新時刻が作業の前後で同じ。

## やらないこと (このサブでは)

- 入れ子のプレハブの先までの伝播。伝播の行の復元。
- 計測の根拠が無い最適化。インクリメンタルな構造 diff などの大きい作り直し (必要と分かったら別サブとして提案する)。
- physics のコードの修正 (A の 3)。Engine 層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。
- 配布物 (sub-20)。

## 受け入れ条件 (このサブ)

1. A: `sizeof(EditorApp)` の比較 (または見積もり)、スタックを大きくした exe での `physics` の結果、ヒープに置いた後の Debug の `physics` の結果が、`sub-19.md` の実装メモと spec の D38 にある。解消した場合は、Debug の全スイート (physics を含む) が通る。
2. B: プレハブのベースのフィールドを変えると、そのプレハブのインスタンスのうち override していないものに `PrefabPropagation` が出て、override しているものには出ない。`caused_by` が原因を指す。DB を作り直しても同じ → `cargo test` (要求 §16.3 の 5 番目)。
3. B: 伝播による変更と直接の編集が、Hierarchy のバッジ・Inspector の印・AI Changes 窓の一覧で区別される。伝播の行から原因の変更へ移動できる。伝播の行で「復元…」を選ぶと原因の変更へ案内される (戻せない) → `changetracking` スイート + スクショ。
4. C: 計測の手順・環境・値が `performance.md` にあり、同じコマンドで取り直せる。軽くした箇所は前後の値がある。可視の判定を変えたかどうかと、その根拠がある。arch §11 に目標値がある → 文書。
5. D: `acceptance.md` に要求 §16 と §18 の全項目があり、「自動の試験 / 手動の確認 / 未確認」が区別されている。ADR-039・計画・`engine_spec.md`・`test_checklists.md` が実装と合っている → 文書。
6. E の全部。
7. スクショ (一時のプローブ。窓を出す起動の作業ディレクトリはリポジトリの外): 伝播の印 (Hierarchy、Inspector)、一覧の伝播の行と原因への移動、英語。**見た目の合否はユーザーの目視。**

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=physics` (Debug。A の前後)、`--selftest-full` (Release)、Debug の全スイート
- `tools\replay_verify.bat`
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release` (サービスを変えたら。`mye_track.exe` が動いていないことを確かめてから)、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`pwsh -File tools\check_rules_selftest.ps1`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`、`git diff --stat ca6a3991 -- src\Engine`、`git status --short`
- 作業の注意: 窓を出す起動 (`--screenshot`、`--frames`) は作業ディレクトリをリポジトリの外に、`--selftest` はリポジトリで。`editbin` などで exe を変えるときは、リポジトリの外のコピーで行う (`bin\x64\` の exe を書き換えない)。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

## フィードバック履歴
