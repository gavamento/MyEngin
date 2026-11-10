# M98: GitHub Releases 配布 + 専用 Launcher + エディタの更新機能

## 再開手順 (セッション開始時に読む)

1. `git log --oneline --grep "^M98"` で終わったサブを確かめる (進捗の一次情報)
2. 下の進捗表で次のサブを選び、その節と「設計」を読む
3. サブごとにビルド (Debug / Release) → `MYE_SELFTEST_ONLY=update,...` で `Editor.exe --selftest` → `tools\check_rules.ps1` → コミット (`M98x: ...`)
4. 終わったら進捗表を更新し、計画外だった事実と後続への申し送りだけを追記する

## 進捗

| サブ | 状態 | 内容 |
|---|---|---|
| M98a | 完了 | miniz、HttpClient / Sha256 / ZipExtract / EngineRelease、`update` スイート |
| M98b | 完了 | Launcher プロジェクト、インストール・一覧・起動・削除、`--release-source` |
| M98c | 完了 | Launcher の自己配置 (LOCALAPPDATA + スタートメニュー)、自己更新、Setup.ps1 の実行 |
| M98d | 完了 | エディタの UpdateService、Help メニュー、About、トースト、設定、Launcher への引き渡し |
| M98e | 完了 (v0.86 公開済み) |
| M98f | 完了 | Defender 対策 (ダウンロードの印とスキャン、自己複製をやめる、zip から Launcher を外す)、0.86.1 | make_dist.ps1 の拡張、release.yml、README / engine_spec / ADR-037 / docs\release.md、版 0.86 への統一と `v0.86` の公開 (ユーザー指示済み) |

## ユーザーの追加指示 (2026-11-10、M98a の後)

- 全部終わったら **版 0.86 で公開する** (タグ `v0.86`)。`kEngineVersion` (`src\Engine\Engine\App\Project.h`)・Hub の表記・`tools\collab_fixture.ps1` など版の表記を 0.86 に統一する (M98e で実施)
- 公開の許可はこの指示で出ている。M98e では `workflow_dispatch` の試走で artifact を確かめてから `v0.86` を push する (rc タグの試走は不要になった)
- 0.86 は `ParseEngineVersion` で [0, 86, 0, 0] になり、0.6.8.22 より新しいと判定される (順序は正しい)。asset 名は `MyEngine-0.86-win64.zip`

## 申し送り

- M98f (ユーザー判断: 方針 1 = IAttachmentExecute、版は 0.86.1、自己配置はボタン): ダウンロードを Attachment Manager で保存 (MOTW + AV スキャン)、展開物へ印を引き継ぐ、Setup.ps1 -NoUnblock、起動時の自己コピーとスタートメニュー登録をやめて場所の記録 (launcher_location.json) だけにする、zip から Launcher を外す。2 回目の実走 (新しい Launcher + v0.86 の zip) では新しい Launcher 自体は止められず、v0.86 の zip に同梱の旧 Launcher が展開時に検知された
- M98f: 0.86 の e2e 試験 (環境変数で置き場を差し替え) でユーザーの実際のスタートメニューに一時フォルダを指すショートカットが作られていた。削除はユーザーの承認待ち

- **M98e 公開後に判明 (未解決)**: GitHub から落とした `MyEngineLauncher.exe` が、Release から 344 MB を落として入れ、Editor を起動した直後に Windows Defender の振る舞い検知 `Behavior:Win32/DefenseEvasion.A!ml` で実行を止められた (2026-11-10 22:33)。ローカル取得元からの同じ流れ (ネットワークなし) では検知されていない。推定: 「インターネットから落とした実行ファイルを Mark-of-the-Web 無しで書き、起動する」がドロッパー型の振る舞いに見える。対策案: (1) ダウンロードを IAttachmentExecute (Attachment Manager) 経由で保存して MOTW と AV スキャンを正規に通す、(2) コード署名、(3) Microsoft への誤検知の報告。ユーザーの判断待ち
- M98e: v0.86 公開 (`5a3552a`、タグ `v0.86`、https://github.com/gavamento/MyEngin/releases/tag/v0.86)。試走 run 42819599483 と公開 run 42822125348 はどちらも success (ランナーで fast selftest 107 スイート PASS、zip 344.3 MB)。GitHub の latest リンクから落とした Launcher で 44 秒でインストール・起動まで確認

- M98e: 実パッケージ (352.8 MB) をローカル取得元から Launcher (--update) で 11 秒で入れ、入った Editor がプロジェクト付きで起動することを確認
- M98e: ユーザー指摘で起動スプラッシュ下部の文字の重なり (長いパス・ログの折り返し) を直した。DirectWrite を NO_WRAP + 省略記号 (区切り `\` を 1 個残す) にした。PrintWindow で窓だけを撮って確認 (画面からの切り取りは上の窓が写るので使わない)
- M98e: `--selftest-full` Release で `disturbancelong` の焼き込みハッシュが 1 件 FAIL。基点 fbba064 (M98 着手前) を別 worktree でビルドしても同じ FAIL = **M98 と無関係の既存の失敗** (M89q / M97a の合流後から。別件で直す)。他の 119 スイートは Debug / Release とも PASS

- M98d: 自動確認の ON/OFF は計画の「ProjectSettings 窓」ではなく更新窓のチェックボックスに置いた (`editor_global.json` の `autoUpdateCheck` / `lastUpdateCheckUnix`)。自動確認は対話起動のときだけ (`EditorApp::allowAutoUpdateCheck = !automation`)
- M98d: 「更新して再起動」は `PendingAction::UpdateAndRestart` で既存の未保存確認を通し、Launcher を `--update --launch-project <projectRoot>` で起動して終了する。環境変数 (`MYE_RELEASE_SOURCE` / `MYE_INSTALL_ROOT`) は Launcher に引き継がれる
- M98d: 画面は一時プローブで撮影して確認済み (更新あり・開発ビルドでボタン無効・About)。Launcher が入れた版での「更新して再起動」の実走は M98e の本物のパッケージで行う

- M98c: アイコンは `tools\gen_app_icon.ps1` (System.Drawing) が `buildes\MyEngine.ico` を生成 (16〜256 の PNG 格納 ICO、RotY 2.6 / RotX 0.9)。`buildes\AppIcon.rc` を Editor / Launcher が共有し、Win32Window とスプラッシュの窓クラスが id 1 を読む。Runtime / Server には付けていない (ゲーム側の exe なので)
- M98c: Launcher の自己更新は Release の asset `MyEngineLauncher.exe` と `MyEngineLauncher.exe.sha256` を使う (M98e の release.yml で両方を出すこと)。実行中の exe を .old.exe へ rename できることは実機で確認済み
- M98c: 初回インストール後だけ Setup.ps1 を `powershell.exe -NoExit ... -NoLaunch` で別窓に出す。版ごとの行にも手動の Setup ボタン

- M98b: Launcher は exe 単体で動く (CRT 静的、import は Windows 標準 DLL だけ。d3dcompiler_47 は遅延)。Release で約 1.8 MB
- M98b: ImGui の描画先クリアは `SwapChain::BindAndClearBackbuffer` に足した (Launcher に生の D3D 型を持たせないため)。`src\Launcher` は規則 16 の検査対象に入れた
- M98b: 規則 13-d により Launcher.vcxproj にオンライン SDK の DLL 名は書けない。遅延ロード指定から外した (参照されないので import も無い)
- M98b: 検証用の起動 `MyEngineLauncher.exe --warp --frames N --install-root <tmp> --release-source <dir> [--update --launch-project P] --screenshot x.png`。実 GitHub API へは疎通済み (Release 0 件 = 404 を「まだ公開された版がありません」と表示)
- ユーザー追加指示 (M98b 中): **アイコンは起動スプラッシュのメビウスの輪。黒背景に白 (少し灰色寄り)**。Launcher と Editor の exe / 窓に付ける (M98c で実施)

- M98a: `MyeWarnAsError=true` の Debug ビルドは master の時点で `AnimatorControllerSelfTest.cpp` (C4456 / C4389) により失敗する (M98 と無関係の既存の赤)。M98 のファイルは警告 0
- M98a: 取得元の切り替えは `ResolveReleaseSource(cli)` (cli > `MYE_RELEASE_SOURCE` > GitHub)。ローカル取得元の `release.json` は GitHub API と同じ形で、asset の `browser_download_url` はフォルダからの相対パスでよい
- M98a: WinHTTP の実ネットワーク疎通は未確認 (selftest はネットワークに出ない)。M98b で Launcher から GitHub API を叩いて確かめる

## Context

今のエンジン配布は `tools\make_dist.ps1` で作った 358MB の zip を手で渡し、受け取った側が `Setup.bat` を実行する形 (M92)。
GitHub Releases は 0 件、Release を作るワークフローも無い。エディタに更新の仕組みも無い。

目標:
1. タグを push すると GitHub Actions が Release を作り、エンジンの zip と `MyEngineLauncher.exe` を公開する
2. ユーザーは `MyEngineLauncher.exe` を 1 個ダウンロードして開くだけで、最新版のエンジンを入れて起動できる
3. エディタが新しい版を知らせ、ボタン 1 つで更新できる

ユーザーの決定 (2026-11-10):
- Launcher は ImGui で Hub と同じ見た目
- 版ごとに並べて保持する (Unity Hub 型)。置き場は `%LOCALAPPDATA%\MyEngine\Engine\<version>\`
- エディタの更新確認は、起動時に自動 (1 日 1 回、設定で切れる) と Help メニューからの手動
- 公開はタグ `v<kEngineVersion>` の push で自動。`-rc` 付きはプレリリース

## 現状 (調査で確認済み)

| 既存のもの | 場所 | 使い道 |
|---|---|---|
| 配布 zip の作成 | `tools\make_dist.ps1` (必須ファイルの検査 :66-73、zip 名は日時 :14) | Release の zip 作成に流用。版名と Launcher の同梱を足す |
| 依存の導入 | `tools\engine_dist\Setup.ps1` (winget で VC++/.NET 8/Git、`-NoLaunch` あり) | Launcher が初回インストール後に実行する |
| Hub の小さな ImGui ループ | `src\Editor\Project\ProjectManager.cpp:371-454` | Launcher の画面の雛形 |
| Engine.lib の遅延ロード | `build\Server.vcxproj:57-65` | Launcher も同じ指定で、使わない DLL を読み込まない |
| ワーカー 1 本 + 進捗 + 取り消し | `src\Editor\Tools\NavBakeService.h:21-73`、UI は `InspectorWindowNav.cpp:306-318` | ダウンロード/展開のワーカーと、エディタの更新確認の型 |
| フォルダの原子的な差し替え | `src\Editor\Windows\Project\BuildSettingsWindow.cpp:361-438` | staging → rename の作り方を参考にする |
| 自分自身の再起動 | `ProjectManager.cpp:456-469` `RelaunchSelfWithProject` | Launcher → 新しい Editor の `--project` 起動 |
| マシン単位の設定 | `src\Editor\App\EditorGlobalSettings.{h,cpp}` (`%LOCALAPPDATA%\MyEngine\editor_global.json`) | 自動確認の ON/OFF、最後に確認した時刻 |
| JSON | `external\nlohmann\json.hpp` | GitHub API の応答を読む |
| トースト | `src\Editor\Widgets\ToastCenter.h` | 新しい版の通知 |
| 製品バージョン | `src\Engine\Engine\App\Project.h:8` `kEngineVersion = "0.6.8.22"` | 版の正本。タグと照合する |
| リモート | `gavamento/MyEngin` (PUBLIC) | 認証なしで `releases/latest` を読める (60 回/時/IP) |

無いもの: HTTP クライアント、zip 展開、SHA-256、Help メニュー、Launcher。

## 設計

### 配布物 (Release の asset)
- `MyEngine-<ver>-win64.zip`: 今の make_dist と同じ構成 (`MyEngine\bin\x64\Release\` + `assets\` + `src\Shared\` + `build\Common.props` + Setup 類)。`bin` に `MyEngineLauncher.exe` も入れる
- `MyEngine-<ver>-win64.zip.sha256`: Launcher が展開前に照合する
- `MyEngineLauncher.exe`: 名前を固定する。README に `https://github.com/gavamento/MyEngin/releases/latest/download/MyEngineLauncher.exe` を書けば常に最新版を指す

### インストールの形
```
%LOCALAPPDATA%\MyEngine\
  Launcher\MyEngineLauncher.exe     初回起動で自分をここへコピーし、スタートメニューにショートカットを作る
  Engine\0.6.8.23\                  zip の中身 (リポジトリと同じ形。FindAssetsRoot / FindEngineRepoRoot がそのまま効く)
           install.json             {version, tag, sha256, installedAt}。これがある = Launcher が入れた版
  Engine\0.6.8.23.staging-<pid>\    展開中。完了後に rename、失敗したら消す
  Downloads\*.part                  ダウンロード中
```
- インストール済みの一覧は `Engine\*\install.json` を走査して作る。別の台帳ファイルは持たない (ファイルと台帳がずれないように)
- Launcher が WinHTTP で落とした zip には Zone.Identifier が付かないので、`Unblock-File` は要らない。ただし Launcher 自体は署名が無いので、初回は SmartScreen の確認が出る (README に書く)

### 共通部品 (Engine 層、Launcher と Editor が使う)
- `src\Engine\Platform\HttpClient.{h,cpp}`: WinHTTP。`Get(url) → 本文`、`DownloadToFile(url, path, progress, cancel)`。自動プロキシ、タイムアウト、リダイレクト追従 (asset の URL は別ドメインへ飛ぶ)。User-Agent は `MyEngine/<ver>`
- `src\Engine\Platform\Sha256.{h,cpp}`: BCrypt
- `src\Engine\Platform\ZipExtract.{h,cpp}`: miniz (`external\miniz\` に追加し `VERSIONS.md` に記録)。`..` や絶対パスを含むエントリは拒否する (zip slip)。進捗と取り消し
- `src\Engine\Core\Update\EngineRelease.{h,cpp}`: 版の解析と比較 (`0.6.8.22` の 4 桁 + `-rc1` の接尾辞)、GitHub の release JSON の解析、asset の選択
- 取得元は 2 種類: GitHub (`api.github.com/repos/gavamento/MyEngin/releases/latest`) と、検証用のローカルフォルダ (`--release-source <dir>` / 環境変数 `MYE_RELEASE_SOURCE`。`release.json` は GitHub と同じ形)。ネットワーク無しで全経路を試せるようにする

### Launcher (`src\Launcher\`、`build\Launcher.vcxproj`、出力は `MyEngineLauncher.exe`)
- Engine.lib をリンクし、GPU 以外の DLL は Server と同じく遅延ロードにする。Hub のループを使う。生の D3D 型を使うので、`check_rules.ps1` 規則 15/16 の許可リストと `engine_spec.md` §6.3 に理由付きで追加する
- 画面: 最新版の表示 (リリースノート)、「最新版をインストール」、インストール済みの一覧 (起動 / 削除 (確認あり) / Setup を実行)、進捗バーと取り消し
- インストールの流れ: 取得 → `.part` へダウンロード → SHA-256 照合 → staging へ展開 → `install.json` を書く → rename。どの段で失敗しても既存の版には触れない
- 初回インストールの後は `Setup.ps1 -NoLaunch` を見えるコンソールで実行する (C++ Build Tools の y/N 質問があるため)
- 自己更新: Release の Launcher が新しければ `.new.exe` に落とし、実行中の exe を `.old.exe` へ rename (実行中でも rename はできる)、差し替えて再起動。`.old.exe` は次回の起動時に消す
- コマンドライン: `--update [--launch-project <path>]` (エディタから呼ばれる)、`--release-source <dir>`、`--include-prerelease`、`--selftest`、`--screenshot`
- 文字列は `Tr()` (`LocalizationTable.inl`) を使う

### エディタの更新機能 (`src\Editor\Update\`)
- `UpdateService`: NavBakeService と同じ型のワーカー。起動時、前回の確認から 24 時間以上たっていて自動確認が ON なら確認する。失敗しても何も出さない (ログだけ)。手動の確認では失敗も表示する
- メニューに **Help** を新設: 「更新を確認...」「MyEngine について」(版、git ハッシュ、ビルド構成)
- 新しい版があればトーストで知らせ、更新窓にリリースノートと「更新して再起動」を出す
- 「更新して再起動」: 未保存の確認 (既存の終了処理) → Launcher を `--update --launch-project <今のプロジェクト>` で起動 → エディタを終了。Launcher は `%LOCALAPPDATA%\MyEngine\Launcher\` を優先し、無ければ自分の `bin` 内のものを使う
- Launcher が入れた版ではないとき (リポジトリのビルドから起動、`install.json` が無い) は確認だけ行い、ボタンを無効にして「開発ビルドは git で更新してください」と出す
- 設定: `editor_global.json` に `autoUpdateCheck` と `lastUpdateCheckUnix`。ProjectSettings 窓から切り替える
- 実時間を使うのはエディタのこの機能だけで、シミュレーションの状態には入らない。ABI・SimSnapshot・`.rep` は変更しない

### Release ワークフロー (`.github\workflows\release.yml`)
- `on: push: tags: ['v*']` と、検証用の `workflow_dispatch` (Release は作らず、artifact だけ上げる)。`permissions: contents: write`
- 手順: checkout → タグと `kEngineVersion` の一致を検査 → `build_managed.bat Release` → Rust と `build_collab.bat Release` → sln を Release でビルド → `Editor.exe --selftest --warp --no-audio` と `MyEngineLauncher.exe --selftest` → `make_dist.ps1 -SkipBuild -Version <ver>` → sha256 → `gh release create` (`--generate-notes`。タグに `-` があれば `--prerelease`)
- **ci.yml の成功は条件にしない**。CI は直近ほぼ失敗している (replay determinism)。条件にすると、CI を直すまで公開できない。Release のビルドとスモークテストは独自に行う
- EOS の DLL が bin にあれば失敗させる (再配布できるか未確認)。CI は EOS 無しでビルドされるので、公開版ではオンライン (EOS) 機能は無効

### make_dist.ps1 の変更
- `-Version` を追加 → `MyEngine-<ver>-win64.zip`。省略時は今の日時名のまま
- 必須ファイルに `MyEngineLauncher.exe` を追加
- `-Release` 指定時に EOS の DLL が入っていたら止める

## サブ分割 (1 サブ = 1 コミット)

| サブ | 内容 | 主な検証 |
|---|---|---|
| M98a | miniz 追加、HttpClient / Sha256 / ZipExtract / EngineRelease、`UpdateSelfTest` (版の比較、JSON 解析、zip slip の拒否、SHA-256 の既知値、ローカル取得元) | ビルド、`--selftest` (該当スイート)、`check_rules.ps1` |
| M98b | Launcher プロジェクト (sln / vcxproj / `gen_project_files.ps1` / 規則 15・16 の許可リスト)、インストール・一覧・起動・削除の画面と処理、`--release-source` | ローカルの release フォルダからインストール → 起動、途中で取り消し / 壊れた zip / SHA 不一致で既存の版が無事なこと、`--screenshot` |
| M98c | Launcher の自己配置 (LOCALAPPDATA + スタートメニュー)、自己更新、Setup.ps1 の実行 | 古い Launcher → 新しい Launcher へ差し替わり再起動すること |
| M98d | エディタの UpdateService、Help メニュー、About、トースト、設定、Launcher への引き渡し | ローカルの取得元で「通知 → 更新して再起動 → 新しい版のエディタが同じプロジェクトで開く」、開発ビルドではボタンが無効、`--screenshot` |
| M98e | make_dist.ps1 の拡張、`release.yml`、README / `engine_spec.md` / ADR-037 / リリース手順書 (`docs\release.md`) | `workflow_dispatch` で artifact まで通る。版の表記を 0.86 に統一 → `workflow_dispatch` の試走 → `v0.86` を push → Release が作られ、Launcher で実際に入れられること |

## 決まっている副作用と注意
- 公開のたびに `kEngineVersion` を上げる必要がある。上げると Hub が既存プロジェクトに版違いの注意を出す (既存の挙動、警告だけ)
- `tools\engine_dist\*.bat` の改行 (LF) は check_rules の既存エラー。zip へは `Copy-Text` が CRLF に変換するので配布物には影響しない。今回は直さない
- 1 版はおよそ 390MB。古い版は自動では消さない (Launcher の一覧から削除する)
- タグの push と Release の作成は外部に公開される操作なので、M98e で実行する前にユーザーへ確認する

## 検証のまとめ
- 各サブ: Release と Debug のビルド、`Editor.exe --selftest` (`MYE_SELFTEST_ONLY` で関連スイート)、`tools\check_rules.ps1`
- 最後: `Editor.exe --selftest-full`。ABI もシミュレーションも変わらないので `replay_verify` は回帰確認として 1 回だけ実行する
- 画面: Launcher とエディタの更新窓を `--screenshot` で撮って確認し、目視はユーザーに依頼する
- 実ネットワーク: rc タグでの GitHub 経由のインストールと更新 (M98e)
