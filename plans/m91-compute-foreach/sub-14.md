# sub-14: 外部プロジェクトのリポジトリにも同じ改行規則を入れる

- 依存: sub-13 (sub-12 で規則が確定し、sub-13 でエンジンが外部プロジェクトの管理対象を LF で書くようになってから行う。先に行うと、検証でエンジンが CRLF で書き戻して差分が出る)
- 状態: OK (三校 commit f918c7b、未 push)
- 往復: 0
- 新設: 2026-10-12 (U15 のユーザー回答「揃える」。planner 案「範囲外」の逆)

## 対象 (2026-10-12 に planner が確認)
| プロジェクト | パス | git | 作業ツリー | 改行の現状 (`git ls-files --eol`) | 慣例 |
|---|---|---|---|---|---|
| 三校 | `C:\HAL\三校` | あり (origin = `https://github.com/gavamento/Shadow_Sound.git`) | clean (確認した時点) | i/crlf w/crlf が 6 件 (`.gitattributes` と `assets/physmats/*.physmat.json` 5 件)、i/lf w/crlf が 31 件、i/lf w/lf が 21 件、-text が 2 件。今の `.gitattributes` は `*.png binary` / `*.wav binary` / `*.bat text eol=crlf` | 日本語の件名。ときどき `P2:` のような接頭辞が付く (`git log` で確かめて合わせる) |
| HAL Collector | `C:\HAL\GameEngin_Demo` | **無い** (`.gitignore` と `.mye` はあるが `.git` が無い) | — | — | — |

- sub-10 の実走で使ったのは scratchpad の一時プロジェクトで、この 2 つではない。
- HAL Collector には git が無いので `.gitattributes` が効かない。このサブでは**触らない**。扱いは U16 で決める。

## やること (三校)
1. 着手時に `git -C C:\HAL\三校 status --porcelain` が空であることを確かめる。空でなければ**何もせずに止まり**、SELF_EVAL に状態を書く (未コミットの変更を壊さないため)。
2. `C:\HAL\三校\.gitattributes` を、sub-12 で確定したエンジンの規則と同じ内容にする。既定は `* text=auto eol=lf`、例外は `.bat` / `.cmd` と VS のプロジェクトが CRLF、バイナリは明示する。
   - 三校にしか無いバイナリの拡張子 (今ある -text の 2 件など) を足す
   - 三校にある既存の行の意味は保つ
   - エンジン側の external/ などの固有の行は持ち込まない
3. `git add --renormalize .` を実行する。index で CRLF の 6 件が LF に変わる見込み。差分の一覧を実装メモに書く。
   - `.gitattributes` の変更と renormalize の結果を**同じ 1 コミット**に入れる
   - 件名は三校の慣例に合わせた日本語 (例: `改行コードを .gitattributes で統一する (テキストは LF、.bat は CRLF)`。接頭辞の要否は `git log` を見て決める)
   - コミットの末尾には、このセッションの帰属行を付ける (司会の指示に従う)
4. 作業ツリーを書き直す (`git rm --cached -r -q .` → `git reset --hard`)。1 の clean の確認を通ったときだけ行う。
5. **push はしない**。三校の origin は共同制作のリポジトリなので、push するか、いつするかはユーザーが決める。完了報告に「未 push」と書く。

## やらないこと (このサブでは)
- HAL Collector (`C:\HAL\GameEngin_Demo`) への変更 (U16)
- push
- 三校のゲームの中身の変更。ASSET_HANDOVER.md と 三校企画.md の制作上の制限を、このサブにも当てはめる。改行の正規化と `.gitattributes` 以外は触らない

## 受け入れ条件 (このサブ)
1. 三校で 1 コミットが作られていること。
   - 中身は `.gitattributes` と renormalize の結果だけ
   - `git show --stat` を実装メモに書く
   - push していないこと (`git status` が "ahead of origin by 1 commit" になる)
2. 書き直した後の `git ls-files --eol` の集計を書く。期待は次の 3 点:
   - i/crlf と i/mixed が 0 件
   - 作業ツリーのテキストは LF。ただし .bat などは CRLF
   - バイナリは -text のまま
3. **三校で回せる検証** (結果を実装メモに書く):
   - a. エンジンで開けること: `Editor.exe --project C:\HAL\三校` で起動する (headless か `--autoplay --frames N` で可)。シーンの読込と、アセット (physmat の .json を含む) の読込で、ERROR が新たに出ないこと
   - b. C++ スクリプトのビルド: エディタの Rebuild Scripts と同じ経路 (`PrepareProjectScriptsBat` の bat) が通ること。ABI v29 で再ビルドが要るはず。失敗した場合、改行と無関係な原因 (ABI など) なら、その旨と出たエラーを書いて止める。改行が原因なら直す
   - c. physmat の .json 5 件の値が、正規化の前後で同じに読まれること。ログ、または読込結果の比較で確かめる
   - d. 三校の `tests\` に回せる試験があれば回す (無ければ「無い」と書く)
   - e. (sub-13 の受け入れ 3 の代わりも兼ねる) シーンを 1 つ保存し、`.meta` を 1 つ作り直し、Rebuild Scripts を 1 回行う。そのうえで、エンジンで開いて保存しても、改行だけの差分が `git status` に出ないこと (sub-13 の効果の確認)。保存した変更は `git checkout -- .` で戻し、最後は clean で終える
4. 失敗した場合: 3 のどれかで改行が原因の問題が出て直せないときは、コミットを `git reset --hard HEAD~1` で戻す前に、まず司会へ報告する。これは不可逆に近い操作なので、勝手に戻さない。

## 検証コマンド
- `git -C C:\HAL\三校 status --porcelain` / `ls-files --eol` / `add --renormalize .` / `show --stat`
- `bin\x64\Release\Editor.exe --project C:\HAL\三校 ...`、生成された `cache\build_scripts.bat`

## コミット
- 三校のリポジトリに 1 コミット (件名は三校の慣例に合わせる)。エンジンのリポジトリへのコミットは無い。ただし plans/ の実装メモを更新したら、司会がエンジン側でまとめてコミットする

## 実装メモ (coder が追記)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。三校のコミット f918c7b (未 push)。
  - ahead の数は origin/master が無いので取れなかった。`rev-list --count HEAD ^ab07dc5` = 1 を代わりの証拠として受け入れる。
  - verify.bat の FAIL 2 件は、改行が原因ではないことが対照実験で示された。M91 の退行かどうかは sub-15 で切り分ける。
  - 3-e の未実施分 (シーンの保存、GUI からの Rebuild Scripts) は、sub-09 の手動の項目で補う。
