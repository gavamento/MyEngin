# sub-13: エンジン・エディタ・ツールが書くテキストの改行を、.gitattributes の規則に揃える

- 依存: sub-12
- 状態: 未着手
- 往復: 0
- 新設: 2026-10-12 (U14 のユーザー回答「リポジトリ全体」に伴う。sub-12 の洗い出しの表が入力)

## 入力 (sub-12 の洗い出し)
text モードの ofstream で CRLF を書く箇所が 6 つある:
- `AssetDatabase.cpp:275` (`.meta`)
- `AssetOps.cpp` の 912 / 957 / 1641
- `NetRollback.cpp:206`
- `InputActions.cpp:142`
- `FxStackAsset.cpp:207`

詳細は sub-12.md の実装メモにある。規則 14 には LF 側の作業ツリーの検査 (14-c) を足す。

## やること
1. sub-12 の洗い出しの表で「規則と違う改行で書いている」箇所を直す。リポジトリで管理されるテキストは LF、`.bat` と VS のプロジェクトは CRLF にする。
   - 書き込みの関数は binary モードにするか、改行を明示する
   - 既存の共通の書き込み関数 (`schema::WriteIfChanged` など) があれば、それに寄せる
2. 未追跡の生成物 (`cache\`、`bin\`、外部プロジェクトの `cache\Generated\`) は対象外。ただし、エンジンが外部プロジェクトの**管理対象**に書くもの (シーン、`.meta`、`project_settings.json`、`Kernels.gen.cs` など) は、同じ規則 (LF) で書く。外部プロジェクトの `.gitattributes` は変えない (U15)。
3. sub-12 で保留した check_rules の見張りを有効にする: 作業ツリーにある LF 規則のテキストに CR が含まれていたら error にする。対象は `git ls-files --eol` の w/crlf と w/mixed。
4. 修正した書き込みのそれぞれについて、selftest で LF (または CRLF) で書かれていることを確かめる。既存の selftest に足せるものはそこへ足す。

## やらないこと (このサブでは)
- 読み込み側を改行に寛容にする変更 (規則で揃えるので要らない。ただし利用者のファイル = 外部プロジェクトのカーネルなどは、今までどおり正規化する)

## 触る場所 (planner の見立て)
- sub-12 の表に挙がった書き込みの箇所 (シーンの保存、.meta の生成、設定の保存、生成器など)、`tools\check_rules.ps1`

## 受け入れ条件 (このサブ)
1. 表に挙がった箇所が、すべて直されているか、直さない理由が書かれていること
2. 書き込みの selftest が PASS すること
3. エディタでシーンを 1 つ保存し、`.meta` を 1 つ作り直し、Rebuild Scripts を 1 回行う。その後で `git status` を見て、改行だけの変更が出ないこと。手順と結果を記録する
4. check_rules の見張りが有効で、PASS すること。一時的に CRLF にしたファイルを検出できること
5. `Editor.exe --selftest` (Debug / Release) と `tools\replay_verify.bat` が PASS すること

## 検証コマンド
- Debug / Release のビルド、`Editor.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`、手動の保存操作と `git status`

## コミット
- 件名候補: `M91 追補: エンジンとツールが書くテキストの改行を .gitattributes の規則に揃える`
- 表に該当が 1 件も無ければ、3 の見張りの有効化だけをコミットする

## 実装メモ (coder が追記)

## フィードバック履歴
