# sub-20: エンジン配布物へのサービス同梱 (M104u)

- 依存: sub-19
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-19 の VERDICT で planner が見直してから着手する。

ユーザー判断 (2026-11-14): エンジン配布物に `mye_track.exe` を入れる作業を M104 に含める (spec §2 D5)。全体の最後のサブ。

## やること

配布物 (Releases の `MyEngine-<版>-win64.zip`) を展開した利用者の Editor でも追跡が動くようにする。前提は sub-05 で確定した置き場所 (`tools\build_track.bat` が `bin\x64\Release\` へ `mye_track.exe` を置き、Editor は自分の exe の隣を探す)。**sub-05 で別の置き場所が選ばれていたら、着手前の見直しでこのサブを書き直す。**

1. `tools\make_dist.ps1`:
   - ビルドする経路 (`-SkipBuild` なし、65 行目の `foreach ($bat in 'build_managed.bat', 'build_collab.bat')`) に `build_track.bat` を足す。
   - `$required` (72〜73 行目) に `mye_track.exe` を足す (欠けたまま配ると「追跡だけ動かない」原因の分かりにくい縮退になる。既存のコメントと同じ理由)。
   - `bin\x64\Release` から配布物へコピーする規則 (99〜103 行目付近) が `mye_track.exe` を拾うことを確かめる。拾わなければ足す。
2. `.github\workflows\release.yml`: 「collab service (Release)」の段 (68〜70 行目) の隣に `tools\build_track.bat Release` の段を足す (`make_dist.ps1 -SkipBuild` が詰める前に exe ができていること)。`rusqlite` bundled がリリースのランナーでビルドできるかは、sub-05 の CI の結果を根拠にする。sub-05 で CI に入れなかった場合は、ここが最初の確認になるので「未検証」と明記する。
3. ゲームの配布物には入れない (要求 §14.2「履歴データをゲーム配布物へ含めない」): `.github\workflows\ci.yml` 196 行目の `$forbidden` に `mye_track.exe` を足す。`--package` のコピー規則が `.mye\` を拾わないことをコードで確かめる。
4. 配布物での探し方の確認: 展開先には `tools\track\target\` も `.mcp.json` も無い。Editor が「自分の exe の隣」だけでサービスを見つけ、プロジェクトの `.mye\ai_changes\` に記録できること。`mye_mcp.exe` は配布物に入っていない (`$required` に無い) ので、配布物での MCP 経由の起動はこのサブの範囲外。フック CLI (`mye_track.exe hook`) を配布物の利用者が設定するときのパスを、sub-08 の手順の文書に 1 節足す。
5. Launcher / 更新 (M98) との関係: 更新時に、動いている `mye_track.exe` がファイルを掴んでいて置き換えに失敗しないかを確かめる。Launcher の更新の手順 (どのプロセスの終了を待つか) をコードで読み、サービスが残っているときの扱いを決める (推奨: Editor の終了時にサービスへ終了を頼む必要は無いが、更新の前に Launcher が pipe か mutex で検出して終了を頼む、または置き換えの失敗を利用者に見える形で出す)。**Launcher の変更が要ると分かったら、実装せずに「不安・質問」で planner へ返す** (範囲が広がるのでユーザーに確認する)。
6. Defender の振る舞い検知 (M98 の v0.86.1 で対処した件): 配布物に「子プロセスを切り離して常駐する exe」が増える。開発機で配布物を展開して起動し、検知されないかを 1 回確かめる。確かめられなければ「未確認」と書く。
7. 文書: `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` の進捗表に M104u、ADR-039 の「影響」、配布の文書 (場所は着手時に確かめる)。sub-19 が ADR-039 の状態を「実装済み」にしているので、このサブでは影響の節だけを足す。

## やらないこと (このサブでは)

- `mye_mcp.exe` を配布物に入れること (ユーザーが決めたのは `mye_track.exe` だけ)。
- 追跡の既定値の変更 (sub-09 と、Phase 3 完了時の確認で決まった値のまま)。
- 版の採番、リリースの公開、push (ユーザーの操作)。
- Launcher の実装変更 (上の 5)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\make_dist.ps1`
- `C:\HAL\MyEngin\.github\workflows\release.yml`、`C:\HAL\MyEngin\.github\workflows\ci.yml`
- 文書 (計画、ADR-039、sub-08 のフックの手順)
- 参考: `C:\HAL\MyEngin\tools\build_collab.bat`、`C:\HAL\MyEngin\tools\engine_dist\`、Launcher のソース (場所は着手時に確かめる)

## 受け入れ条件 (このサブ)

1. `tools\build_track.bat Release` の後、`pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild` が通り、できた配布物の中に `mye_track.exe` がある → 実行と中身の一覧。
2. `bin\x64\Release\mye_track.exe` を一時的に別名にして `make_dist.ps1 -SkipBuild` を回すと `missing in ...: mye_track.exe` で失敗する (確かめた後で名前を戻す。削除はしない) → 実行結果。
3. 配布物をリポジトリの外へ展開し、そこから Editor を対話起動して一時プロジェクトを開き、追跡を有効にすると、展開先の `mye_track.exe` が起動して接続され、ファイルの変更が AI Changes 窓に出る。`MYE_TRACK_EXE` は設定しない → 手動。確かめられなければ「未確認」と書く (成功扱いにしない)。
4. `release.yml` に `build_track.bat` の段があり、`package` の段より前にある → 差分の確認。ワークフローの実走は push の後なので「未検証」と報告する。
5. `ci.yml` の `$forbidden` に `mye_track.exe` があり、`--package` の出力に `mye_track.exe` と `.mye` が無い → `Editor.exe --package` 相当をローカルで回して中身を確認 (ci.yml の該当の段と同じコマンド)。
6. 更新時のファイルの掴みと Defender について、確かめた結果か「未確認」が文書にある → 文書。
7. `tools\check_rules.ps1` が 0 error。`src\` を触らなければ selftest と replay_verify は回さなくてよい (回していないことを書く)。

## 検証コマンド

- `tools\build_track.bat Release`
- `pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat -- src`

## 実装メモ (coder が追記)

## フィードバック履歴
