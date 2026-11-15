# sub-20: エンジン配布物へのサービス同梱 (M104u)

- 依存: sub-19
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-16、sub-19 の VERDICT で planner が書き直した)。M104 の最後のサブ。
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

ユーザー判断 (2026-11-14): エンジン配布物 (Releases の `MyEngine-<版>-win64.zip`) に `mye_track.exe` を入れる作業を M104 に含める (spec §2 D5)。前提の置き場所は sub-05 で確定済み: `tools\build_track.bat` が `bin\x64\Debug\` と `bin\x64\Release\` の両方へ置き、Editor は自分の exe の隣を探す (ipc §9.5。`MYE_TRACK_EXE` が設定されていればそれだけ)。

## 確認済みの事実 (2026-11-16 に planner が調べた。行番号はずれうる)

- `C:\HAL\MyEngin\tools\make_dist.ps1`: ビルドの経路 (`-SkipBuild` なし) は 65 行目の `foreach ($bat in 'build_managed.bat', 'build_collab.bat')`。必須の一覧 `$required` は 72〜73 行目 (`MyeCollab.dll`、`MyeCollabCli.exe` などがある)。欠けていれば 78 行目で失敗する。`bin\x64\Release` から配布物へのコピーは 99〜103 行目付近。
- `C:\HAL\MyEngin\.github\workflows\release.yml`: 「collab service (Release)」の段 (68〜70 行目、`tools\build_collab.bat Release`) の後に native build、selftest、launcher selftest、package (89〜93 行目、`make_dist.ps1 -SkipBuild`)。Rust のツールチェーンの段 (65 行目) は既にある。
- `C:\HAL\MyEngin\.github\workflows\ci.yml`: ゲームの配布物 (`Editor.exe --package cache\dist_ci …`、194 行目) に開発用の道具が混ざらないことを、204 行目の `$forbidden = @('MyeCollab.dll', 'MyeCollabCli.exe', '.git')` で確かめている。`track service` の段 (147 行目) は sub-05 で入った。
- Launcher (`C:\HAL\MyEngin\src\Launcher\InstallService.cpp`): 新しい版は別のディレクトリへ展開し (`staging` → `rename(staging, target)`、184 行目)、古い版のディレクトリは置き換えない。**動いている古い版の `mye_track.exe` が、新しい版の導入を邪魔することは無い見込み。** 古い版の**アンインストール** (`UninstallEngine`、203 行目〜、`remove_all`) は、その版の `mye_track.exe` が動いていると失敗しうる。
- `C:\HAL\MyEngin\docs\ai-change-tracking\hooks-setup.md` 9 行目: 配布物での `mye_track.exe` の場所は「Editor の隣 (配布物への同梱は M104u)」と書いてある。

## やること

1. `tools\make_dist.ps1`:
   - ビルドの経路の一覧 (65 行目) に `build_track.bat` を足す。
   - `$required` に `mye_track.exe` を足す (欠けたまま配ると「追跡だけ動かない」原因の分かりにくい縮退になる。既存のコメントと同じ理由を 1 行)。
   - `bin\x64\Release` から配布物へのコピーが `mye_track.exe` を拾うことを確かめる。拾わなければ足す。
2. `.github\workflows\release.yml`: 「collab service (Release)」の段の隣に `tools\build_track.bat Release` の段を足す (`package` の段より前)。CI (`ci.yml`) の `track service` の段で `rusqlite` bundled のビルドが確かめられていない (push していない) ので、**リリースのワークフローの実走は未検証**として報告する。
3. **ゲームの配布物には入れない** (要求 §14.2「履歴データをゲーム配布物へ含めない」): `ci.yml` 204 行目の `$forbidden` に `mye_track.exe` を足す。`--package` のコピーの規則が `.mye\` を拾わないことをコードで確かめ、`--package` の出力に `.mye` が無いことも同じ段で確かめる (`$forbidden` に `.mye` を足すか、別の 1 行)。
4. 配布物での探し方の確認: 展開先には `tools\track\target\` も `.mcp.json` も無い。Editor が「自分の exe の隣」だけでサービスを見つけ、一時プロジェクトの `.mye\ai_changes\` に記録できること (下の受け入れ条件 3)。`mye_mcp.exe` は配布物に入っていない (`$required` に無い) ので、配布物での MCP 経由の起動はこのサブの範囲外。
5. `docs\ai-change-tracking\hooks-setup.md`: 配布物の利用者がフックを設定するときの `mye_track.exe` のパス (配布物のどこにあるか、`--project` の付け方) の節を足す。9 行目の「(配布物への同梱は M104u …)」を実装に合わせて直す。
6. **Launcher との関係** (上の事実): 新しい版の導入は邪魔されない。古い版のアンインストールは、その版の `mye_track.exe` が動いていると失敗しうる。**Launcher のコードは変えない。** 次を確かめて文書に書く:
   - `UninstallEngine` が、使用中のファイルで `remove_all` に失敗したとき、利用者に見える形で失敗を出すか (コードを読む。Launcher の自己テストに近い試験があれば、その形を見る)。出さないなら「不安・質問」で返す (Launcher の変更が要る話なので、このサブでは直さない)。
   - サービスは接続が 0 になってから 10 分で自分で終わる (sub-05) ので、Editor を閉じて待てばアンインストールできる。この手順を `docs\ai-change-tracking\` の配布の説明か `hooks-setup.md` に書く。
   - 古い版のサービスが動いている間に新しい版の Editor を同じプロジェクトで開いた場合: 1 プロジェクトに 1 サービスなので、古いサービスにつながる。プロトコル版が同じなら動き、違えば Editor は「版が違う」と出す (sub-09)。この挙動を文書に 1 行書く (直さない)。
7. Defender の振る舞い検知 (M98 の v0.86.1 で対処した件): 配布物に「親から切り離して常駐する exe」が増える。開発機で配布物を展開して起動し、検知されないかを 1 回確かめる。確かめられなければ「未確認」と書く。
8. 文書: `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` の進捗表に M104u、ADR-039 の「影響」に配布物の 1 行 (状態は sub-19 で「実装済み (配布は M104u)」になっている。M104u の後は「実装済み」)、`C:\HAL\MyEngin\docs\ai-change-tracking\acceptance.md` の A21 の行、`C:\HAL\MyEngin\docs\test_checklists.md` (配布物での確認の手順)。

## やらないこと (このサブでは)

- `mye_mcp.exe` を配布物に入れること (ユーザーが決めたのは `mye_track.exe` だけ)。
- 追跡の既定値の変更 (既定は無効のまま)。
- 版の採番、リリースの公開、**push** (ユーザーの操作)。
- Launcher の実装の変更 (上の 6)。
- `src\` の変更 (要らない見込み。要るなら差分に出す)。

## 作業の注意

- 配布物の作成と展開は、リポジトリの外 (`%TEMP%` の自分のディレクトリ) で行う。`make_dist.ps1` の出力先がリポジトリの中 (`dist_engine\` など) なら、それが `.gitignore` 済みであることを確かめ、`git status` に出ないこと。
- 展開先から Editor を窓つきで起動するときは、作業ディレクトリを展開先 (リポジトリの外) にする (利用者の `imgui.ini` を書き換えない)。`--selftest` はリポジトリで。
- `bin\x64\Release\mye_track.exe` を一時的に別名にする確かめ (受け入れ条件 2) は、`mye_track.exe` が動いていないことを確かめてから行い、確かめた後で必ず元の名前に戻す。**削除はしない。** 別名にする代わりに、`bin\x64\Release` の内容をリポジトリの外へ複写して、複写の側で確かめる方法があれば、そちらを優先する (`make_dist.ps1` が入力のディレクトリを変えられるかを見る)。
- `tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 受け入れ条件 (このサブ)

1. `tools\build_track.bat Release` の後、`pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild` が通り、できた配布物の中に `mye_track.exe` がある (配布物の中のパスを書く) → 実行と中身の一覧。
2. `mye_track.exe` が無い入力で `make_dist.ps1 -SkipBuild` を回すと、`missing in ...: mye_track.exe` で失敗する (上の「作業の注意」の方法で) → 実行結果。
3. **配布物で動く**: 配布物をリポジトリの外へ展開し、そこの Editor を、`changeTrackingEnabled: true` にした一時プロジェクトで起動すると、**展開先の `mye_track.exe`** が起動して接続され、一時プロジェクトの `.mye\ai_changes\` ができる。`MYE_TRACK_EXE` は設定しない。確かめ方は、対話起動が要るなら手動 (その場合は「未確認」でもよいが、成功扱いにしない)。非対話起動では追跡しない作り (sub-09) なので、自動で確かめられるのは「展開先の Editor の隣に `mye_track.exe` があり、`mye_track.exe --version` が動き、展開先の `mye_track.exe serve --project <一時プロジェクト>` が起動して `status` に答える」まで → 実行結果 (起動したサービスは `shutdown` で止める)。
4. `release.yml` に `build_track.bat` の段があり、`package` の段より前にある → 差分。ワークフローの実走は未検証と書く。
5. `ci.yml` の `$forbidden` に `mye_track.exe` (と `.mye`) があり、ローカルで `Editor.exe --package` を ci.yml と同じ引数で回した出力に、`mye_track.exe` と `.mye` が無い → 実行結果。
6. 6 の Launcher の確かめの結果 (アンインストールの失敗が見えるか、待てばできる手順、古いサービスにつながる挙動) が文書にある → 文書。
7. 7 の Defender の結果か「未確認」が文書にある → 文書。
8. `tools\check_rules.ps1` が 0 error。`git diff --stat HEAD -- src` が空 (空なら selftest / replay_verify は回さない。回していないことを書く)。`git status` に配布物の出力が出ない。リポジトリ直下の `imgui.ini` の更新時刻がこのサブの作業の前後で変わらない。

## 検証コマンド

- `tools\build_track.bat Release` (`mye_track.exe` が動いていないことを確かめてから)
- `pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild`
- `bin\x64\Release\Editor.exe --package <リポジトリの外> --package-boot ui_probe.scene.json` (ci.yml と同じ引数)
- 展開先の `mye_track.exe --version`、`serve --project <一時プロジェクト>`、`mye_track.exe client` で `hello` → `status` → `shutdown`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`git status --short`、`git diff --stat HEAD -- src`

## 実装メモ (coder が追記)

## フィードバック履歴
