# 改行コードの統一と作業ツリーの揃え直し

正本は `.gitattributes`。index (コミットされる中身) のテキストは常に LF。
作業ツリーでは、テキストは LF、`.bat` `.cmd` と Visual Studio が書くファイル (`.sln` `.vcxproj` `.filters` `.props` `.targets` `.csproj` `.rc`) は CRLF。
バイナリは変換しない。`core.autocrlf` の設定には依存しない。

## 他の clone・マシンで揃え直す

`.gitattributes` の変更を取り込んだあと、作業ツリーに古い改行が残っていたら次で揃える。

1. 未コミットの変更を退避する。`git status --porcelain` が空になるまで、コミットまたは `git stash` する。空でないまま進めると変更を失う。
2. 次を実行する。未追跡のファイル (bin/ cache/ など) には触らない。

```
git rm --cached -r -q .
git reset --hard
```

3. `git ls-files --eol` で確かめる。テキストは `w/lf`、eol=crlf の種類は `w/crlf`、`w/mixed` は 0 件。`tools\check_rules.ps1` の規則 14 も同じ見張りをする。

新規の clone は何もしなくてよい。
