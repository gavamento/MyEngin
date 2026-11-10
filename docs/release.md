# エンジンの公開手順 (GitHub Releases)

エンジンの配布物を GitHub Releases に公開し、利用者が `MyEngineLauncher.exe` から入れられるようにする手順。
設計の理由は [ADR-037](adr/ADR-037-release-launcher.md)、ワークフローは [release.yml](../.github/workflows/release.yml)。

## 利用者に案内するリンク

```
https://github.com/gavamento/MyEngin/releases/latest/download/MyEngineLauncher.exe
```

これを開いて最新版をインストールする。2 回目からはスタートメニューの「MyEngine Launcher」から開ける。
エディタはヘルプ → 更新を確認 から新しい版へ更新できる (Launcher で入れた版のとき)。

## 公開の手順

1. `src/Engine/Engine/App/Project.h` の `kEngineVersion` を新しい版にする (例 `0.87`)。
   表示 (Hub・About・タイトル・Launcher) はすべてこの定数から出る。`tools/collab_fixture.ps1` の `engineVersion` も合わせる。
2. コミットして master に push する。
3. 試走 (任意): GitHub の Actions → Release → Run workflow。Release は作らず、asset を artifact として上げる。
   ```
   gh workflow run release.yml -R gavamento/MyEngin --ref master
   ```
4. タグを付けて push する。タグは `v` + `kEngineVersion` と一致させる (違うとワークフローが止まる)。
   ```
   git tag v0.87
   git push origin v0.87
   ```
   プレリリースにしたいときは `v0.87-rc1` のように接尾辞を付ける (Launcher とエディタは既定で見ない)。
5. Actions の Release が終わると、Releases に次の 4 個が並ぶ。
   - `MyEngine-<版>-win64.zip` / `.sha256`
   - `MyEngineLauncher.exe` / `.sha256`
6. 確かめる: Launcher を開き、最新版の表示・インストール・起動まで通す。

## ワークフローが行うこと

`tools\build_managed.bat Release` → `tools\build_collab.bat Release` → `MyEngine.sln` を Release でビルド →
`Editor.exe --selftest` (fast 層) と `MyEngineLauncher.exe --selftest` → `tools\make_dist.ps1 -SkipBuild -Version <版> -Release` →
SHA-256 を書く → `gh release create` (リリースノートは自動生成)。

`ci.yml` の成功は条件にしない (重い検査が赤い間も公開できるように)。公開前に手元で `Editor.exe --selftest-full`・`tools\check_rules.ps1`・`tools\replay_verify.bat` を回すこと (AGENTS.md 第 7 章)。

## 手元での確認 (ネットワークに出ない)

```powershell
# 配布物を作る (オンライン SDK を入れた機械では -Release を付けると止まる = 公開物に入れないため)
pwsh -NoProfile -File tools\make_dist.ps1 -SkipBuild -Version 0.87

# GitHub API と同じ形の release.json と asset を置いたフォルダを取得元にし、一時フォルダへ入れる
bin\x64\Release\MyEngineLauncher.exe --release-source <フォルダ> --install-root <一時フォルダ>
```

環境変数 `MYE_RELEASE_SOURCE` / `MYE_INSTALL_ROOT` でも同じ差し替えができ、エディタから起動した Launcher にも引き継がれる。

## 失敗したとき

- `tag ... does not match kEngineVersion`: タグと `Project.h` の版が違う。タグを消して付け直す (`git push origin :refs/tags/v0.87`)。
- `release packages must not contain the online SDK DLL`: オンライン SDK を入れてビルドした bin を詰めようとした。CI では起きない。
- Release を作り直す: `gh release delete v0.87 -R gavamento/MyEngin --cleanup-tag` の後、タグを付け直して push する。
