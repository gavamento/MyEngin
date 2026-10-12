# sub-12: リポジトリ全体の改行コードを .gitattributes で統一する

- 依存: sub-11 (sub-11 のコミットの後に着手する。**着手時に作業ツリーが clean であること** — 司会が plans/ の更新を含めてコミットしておく。clean でなければ着手しない)
- 状態: 未着手
- 往復: 0
- 新設: 2026-10-12 (ユーザーの追加依頼「subを追加テストの改行コードを統一」)
- 拡張: 2026-10-12 (U14 のユーザー回答「リポジトリ全体」。planner 案「M91 の範囲だけ」の逆)

## 現状 (2026-10-12 に `git ls-files --eol` で確認)
- index (コミットされている中身) は、テキストが**すべて LF** (i/lf が 2154 件、i/crlf と i/mixed は 0 件)。バイナリ (i/-text) は 184 件。
- 作業ツリーは `core.autocrlf=true` のため、ばらばら:
  - w/crlf: 1478 件 (チェックアウトで CRLF になったもの)
  - w/lf: 663 件 (ツールや AI が書いたまま、一度もチェックアウトし直していないもの)
  - w/mixed: 13 件 (`build/GameLogic.vcxproj`、`build/Runtime.vcxproj` ほか)
- `.gitattributes` が改行を指定しているのは `*.bat text eol=crlf` だけ。あとはバイナリの指定 (`*.png` / `*.mvox` / `*.dmnet` / `*.msfm` / `tests/deepmodal/*.bin` / `external/...`)。
- 結論: 統一の中身は「**作業ツリーの改行を、マシンや clone の設定 (autocrlf) に依存させない**」こと。index は LF のままなので、`git add --renormalize .` の差分は 0 件の見込み。

## 種類ごとの eol 規則 (planner 裁定)
`.gitattributes` の先頭に置く既定と、例外を次のとおりにする。既存の行は消さない。順序は「既定 → 例外 → バイナリ」で、後の行が勝つ。

| 種類 | 規則 | 理由 |
|---|---|---|
| 既定 (テキスト全般: .h .cpp .c .hpp .inl .cs .hlsl .hlsli .py .rs .ps1 .md .json .ndjson .meta .txt .yml .toml .ini .gitignore など) | `* text=auto eol=lf` | テキストかどうかは git の自動判定に任せる。今も binary と判定されている .md 3 件などを無理にテキスト扱いしない。作業ツリーは常に LF にする |
| `.bat` `.cmd` | `text eol=crlf` (既存の `*.bat` に `*.cmd` を足す) | cmd.exe は LF だとラベルや goto が壊れることがある |
| VS が書くファイル: `.sln` `.vcxproj` `.vcxproj.filters` (`*.filters`) `.props` `.targets` `.csproj` `.rc` | `text eol=crlf` | VS と MSBuild は CRLF で書き戻す。LF にすると、VS で保存するたびに作業ツリーの全行が変わって見える。`.sln` は LF だと読めない版があったとされる (記憶による・未確認) |
| golden・replay・アセット・シーン (テキストのもの: シーンや設定の `.json`、`.meta`、`.ndjson`、`.hlsl`) | 既定 (LF) | エンジンはこれらを読むときに改行の違いを吸収する前提にしない。全員の作業ツリーを LF に揃えて、バイト比較と content hash を安定させる |
| golden・replay・アセット (バイナリのもの: `.png` `.fbx` `.glb` `.wav` `.dds` `.blend` `.lib` `.dll` `.exe` `.pdf` `.pyc` `.mvox` `.dmnet` `.bin`、`.rep`、hash dump など) | `binary` を明示する (今ある行に足す) | 自動判定に任せず、1 バイトも変換させない。今 i/-text の拡張子を全部明示する。`.rep` と dump がリポジトリに無くても、生成されてコミットされうる種類は先に書いておく |
| `external/` (第三者のソース) | 既定 (LF)。既存の `*.patch -text` などはそのまま | 内容は変わらない (index はもう LF) |

- 生成物 (Kernels.gen.*、Schema の生成物、`cache\Generated\`): 既定 (LF)。書き込み側の改行は sub-13 で揃える。

## やること
1. 上の規則で `.gitattributes` を書き直す。既存の行の意味は保つ。各ブロックに理由のコメントを付ける。sub-11 が行を足していれば、その行もここで既定へまとめる。
2. `git add --renormalize .` を実行し、index の差分を確かめる。
   - 0 件のはず。0 件でなければ、出たファイルの一覧と理由を実装メモに書く
   - 出た差分は、`.gitattributes` の変更と**同じ 1 コミット**に入れる (renormalize の結果だけを別コミットにしない。規則と結果を分けると、途中のコミットでチェックアウトの結果がずれるため)
3. 作業ツリーを規則どおりに書き直す: `git rm --cached -r -q .` → `git reset --hard` (または同じ効果のある手順)。
   - **作業ツリーが clean のときだけ**行う。手順の前に `git status --porcelain` が空であることを確かめ、空でなければ止めて SELF_EVAL に書く
   - この手順は未追跡のファイル (bin/、cache/ など) には触らない
4. 見張り: `tools\check_rules.ps1` に規則を足す (番号は着手時点の末尾から取る)。
   - index に i/crlf と i/mixed のテキストが 1 件も無いこと
   - `.gitattributes` の eol=crlf の種類 (`.bat` など) が、作業ツリーで CRLF になっていること
   - LF のはずのテキストを作業ツリーで見張るのは sub-13 の後 (エンジンが CRLF で書くファイルがあると、ここで誤検出するため)。sub-13 で有効にする
5. 他の clone やマシンで作業ツリーを揃え直す手順を、docs に短く書く。`docs/` の Git 運用の文書があればそこへ、無ければ ADR-030 の付録として sub-09 へ申し送る。手順は 3 と同じで、未コミットの変更がある場合の注意も書く。
6. 洗い出し (sub-13 の入力): エンジン・エディタ・ツールのうち、**リポジトリで管理されるテキストファイルを書く箇所**を grep で列挙する。改行の書き方を「LF / CRLF (text モードの ofstream、`Set-Content` など) / 不明」に分けて表にし、実装メモに書く。
   - 例: シーンの保存、`.meta` の生成、project_settings.json、Schema / Kernels の生成器、gen_project_files.ps1 の vcxproj (こちらは CRLF が正しい)

7. (sub-11 からの申し送り) `src/Engine/Engine/Net/ServerNetSelfTest.cpp` の `TestWaitUntilReadable` が見ている待ち時間の上限 (1500 ms) をなくして下限だけを見るか、上限を 5000 ms 以上に広げる。遅い CI で揺れないようにするため。

## やらないこと (このサブでは)
- エンジン・ツールの書き込みの修正 (sub-13)
- 外部プロジェクト (HAL Collector / 三校) のリポジトリ (U15)

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\.gitattributes`、`tools\check_rules.ps1`、docs (再チェックアウトの手順)

## 受け入れ条件 (このサブ)
1. `git add --renormalize .` の結果を実装メモに書く (0 件か、差分の一覧と理由)。差分は `.gitattributes` と同じコミットに入れる
2. 再チェックアウトの後の `git ls-files --eol` の集計を書く。期待は次の 3 点:
   - テキストは w/lf。ただし eol=crlf の種類は w/crlf
   - w/mixed が 0 件
   - バイナリは -text のまま
3. `core.autocrlf=true` と `core.autocrlf=false` のそれぞれで新規の worktree を作り、作業ツリーの改行の集計が同じになること (= 設定に依存しない)。作った worktree は片付ける
4. **再確認** (どれも、再チェックアウトの後の作業ツリーで行う):
   - Debug / Release のビルド
   - `bin\x64\Debug\Editor.exe --selftest` と `bin\x64\Release\Editor.exe --selftest`
   - `bin\x64\Debug\Server.exe --selftest`
   - `tools\replay_verify.bat` (Debug / Release の照合、jobs A/B、snapshot の往復を含む全ジョブ)
   - **golden (ピクセル回帰) の照合**: CI の pixel regression と同じ手順 (ADR-014) をローカルで回し、すべて一致すること。一致しなければ、memory `golden-diff-triage.md` に従って原因を断定する。`--update` で上書きしない
   - `tools\check_rules.ps1`
   - `tools\build_managed.bat` (`MYE_DOTNET_ARGS=/p:TreatWarningsAsErrors=true` 付き)
5. check_rules の新しい規則が、一時的に CRLF にした `.cpp` (index に入れる手前で `git add` したもの) と、LF にした `.bat` を検出すること。確かめたら元に戻す
6. 書き込み側の洗い出しの表 (6) が実装メモにあること

## 検証コマンド
- 上の 4 の一式と、`git ls-files --eol`、`git worktree add` / `git worktree remove`

## コミット
- 件名候補: `M91 追補: リポジトリ全体の改行を .gitattributes で統一する (テキストは LF、.bat と VS のプロジェクトは CRLF)`
- 後続のサブの文字 (g〜j) は変えない

## 実装メモ (coder が追記)

## フィードバック履歴
