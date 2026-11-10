# ADR-037: 配布と更新 — GitHub Releases に版ごとの zip を置き、入れ替えは Launcher だけが行う

- 状態: **確定** (2026-11-10、M98a〜M98e)
- 出所: M98。計画と進捗は `plans/m98-release-launcher.md`。
- 実体:
  - 共通部品 (Engine 層): `src/Engine/Platform/HttpClient.{h,cpp}` (WinHTTP、https のみ)、`Sha256.{h,cpp}` (BCrypt)、`ZipExtract.{h,cpp}` (miniz 3.0.2、`external/miniz/`)、`src/Engine/Engine/Update/EngineRelease.{h,cpp}` (版の解析と比較・Release JSON・asset 名の契約・取得元)、`InstallLayout.{h,cpp}` (置き場と `install.json`)。
  - Launcher: `src/Launcher/` → `MyEngineLauncher.exe` (`build/Launcher.vcxproj`)。`InstallService` (インストール・削除・ワーカー)、`LauncherUi`、`LauncherSelf` (自己配置・自己更新・Setup.ps1)、`LauncherSelfTest`。
  - エディタ: `src/Editor/Update/UpdateWindow.{h,cpp}` (自動確認・更新窓・Help メニュー・About)、`EditorApp` の `PendingAction::UpdateAndRestart`、`editor_global.json` の `autoUpdateCheck` / `lastUpdateCheckUnix`。
  - 公開: `.github/workflows/release.yml`、`tools/make_dist.ps1` (`-Version` / `-Release`)。手順は `docs/release.md`。
  - アイコン: `tools/gen_app_icon.ps1` → `build/res/MyEngine.ico` (起動スプラッシュのメビウスの輪)、`build/res/AppIcon.rc`。
  - 検証: selftest `update` (Engine 層)・`editorupdate` (エディタ)、`MyEngineLauncher.exe --selftest`。
- 番号: ABI・SimSnapshot・TypeId は**変更なし**。製品の版 `kEngineVersion` を 0.6.8.22 → **0.86** に変更 (最初の公開版)。

## 背景

エンジンの配布は `tools/make_dist.ps1` の zip (約 350 MB) を手で渡し、受け取った側が `Setup.bat` を実行する形だけだった (M92)。
新しい版を出しても受け取った側は知る手段が無く、入れ替えも手作業 (上書きすると古い版に戻れない)。
「専用の Launcher を 1 個ダウンロードして開けば最新版が入る」「エディタから更新できる」を、追加のサーバを持たずに実現する。

## 決定

### D1. 配布元は GitHub Releases、公開の起点はタグ

タグ `v<kEngineVersion>` を push すると `release.yml` が Release ビルド → 短い selftest → `make_dist.ps1 -Release` → `gh release create` まで行う。
タグと `kEngineVersion` が食い違えば止める (版の正本は 1 つ。About・Hub・Launcher の表示と Release の版がずれると更新確認が終わらない)。
`-rc1` などの接尾辞付きタグはプレリリースで、Launcher とエディタは既定で見ない (`--include-prerelease` で見る)。
`ci.yml` の成功を条件にしない: `ci.yml` は replay 照合やスクショまで回す重い検査で、そちらが赤い間も公開を止めないため。release.yml 側で Release ビルドと fast 層の selftest・Launcher の selftest を独自に回す。
却下: 独自の更新サーバ (運用が増える)、`ci.yml` の成果物をそのまま公開 (成功時に成果物を上げておらず、警告をエラーにするビルドが既存の警告で落ちている)。

### D2. asset 名は契約

`MyEngine-<版>-win64.zip`、その `.sha256` (sha256sum 形式)、`MyEngineLauncher.exe`、その `.sha256`。名前は `EngineRelease.h` (`EnginePackageAssetName` / `kLauncherAssetName`) と `release.yml` / `make_dist.ps1` で揃える。
`MyEngineLauncher.exe` の名前を固定するので `https://github.com/gavamento/MyEngin/releases/latest/download/MyEngineLauncher.exe` が常に最新を指す。

### D3. 版ごとに並べて保持し、入れ替えは rename 1 回

置き場は `%LOCALAPPDATA%\MyEngine\Engine\<版>\` (Program Files は書けないので使わない。M52 計画で未解決だった点)。中身は zip の最上位フォルダを外した、リポジトリと同じ形 (`FindAssetsRoot` / `FindEngineRepoRoot` がそのまま効く)。
インストールは「`.sha256` の取得 → `.part` へダウンロード → SHA-256 照合 → `<版>.staging-<pid>` へ展開 → `Editor.exe` の存在確認 → `install.json` → `<版>` へ rename」。どの段で失敗・取り消しても既存の版と `<版>\` には触れず、書きかけを消す。
削除は先に `<版>.deleting-<pid>` へ rename してから消す。使用中 (エディタが動いている) なら rename が失敗し、1 ファイルも消さずに止まる。
一覧は `Engine\*\install.json` の走査で作り、別の台帳を持たない (ファイルと台帳がずれないように)。`install.json` の無いフォルダは Launcher の管理外として触らない。
却下: 1 フォルダの上書き更新 (失敗や不具合の版から戻れない。ユーザーの選択)。

### D4. エディタは確認だけ、入れ替えは Launcher

動いている exe は自分を上書きできず、版ごとの置き場を扱う処理を 2 か所に持ちたくない。エディタは最新版を確認して知らせ、「更新して再起動」は未保存の確認 (既存の `PendingAction` の流れ) の後に Launcher を `--update --launch-project <プロジェクト>` で起動して終了する。Launcher は最新版を入れ、同じプロジェクトでエディタを開き直す。
Launcher が入れた版でない (`install.json` が無い = リポジトリのビルド) エディタでは、確認はするがボタンを無効にして git での更新を案内する。

### D5. 自動確認は 1 日 1 回・対話起動だけ・失敗は黙る

起動時に、前回の確認から 24 時間以上たっていれば別スレッドで確認する (GitHub API の未認証の上限 60 回/時/IP に余裕を持たせる)。CI・検証の起動 (`automation`) では通信しない。
自動確認の失敗は通知しない (オフラインでも編集を止めない。障害の局所化)。手動の確認では理由を出す。
更新確認はエディタの機能で、シミュレーションの状態・リプレイ・ハッシュには入らない (実時間を使うのはこの判断だけ)。

### D6. 取得元はローカルフォルダに差し替えられる

`--release-source <dir>` か環境変数 `MYE_RELEASE_SOURCE` で、GitHub API と同じ形の `release.json` と asset を置いたフォルダを取得元にできる。置き場は `MYE_INSTALL_ROOT` / `--install-root` で差し替える。selftest と手元の検証はネットワークに出ずに全経路を通す。

### D7. Launcher は単体の exe

CRT 静的、import は Windows 標準 DLL だけ (Engine.lib をリンクし、使わない DLL は遅延ロード。Server.exe と同じ手)。初回起動で自分を `%LOCALAPPDATA%\MyEngine\Launcher\` へ写してスタートメニューに登録し、Release に新しい Launcher があれば動いている exe を `.old.exe` へ rename して入れ替える。
画面は Hub と同じ ImGui の小さなループ。生の D3D 型を持たせないため `SwapChain::BindAndClearBackbuffer` を足し、`src\Launcher` を規則 16 の検査対象に入れた。

### D8. 安全

https 以外は受け付けない。zip のエントリ名は書き込む前に全件検査し、`..`・絶対パス・ドライブ指定・`:` (代替データストリーム)・制御文字・Windows の予約名 (CON / NUL など)・末尾のドットや空白を 1 件でも含めば何も書かない (zip slip)。CRC は miniz が展開しながら照合する。
公開物にオンライン SDK の DLL を入れない (`make_dist.ps1 -Release` が止める。再配布の可否が未確認、ADR-031)。CI は SDK 無しでビルドするので、公開版ではオンライン (EOS) 機能は無効。

## 制限

- 署名が無いので、Launcher を初めて開くと SmartScreen の確認が出る。Launcher が落とした zip には Zone.Identifier が付かないので、中の exe には出ない。
- 1 版あたり約 400 MB。古い版は自動では消さない (Launcher の一覧から削除する)。
- ダウンロードの再開 (Range) は無い。途中で切れたら最初からやり直す。
- 公開のたびに `kEngineVersion` を上げる必要があり、上げると Hub が既存プロジェクトに版違いの注意を出す (警告だけ、起動は止めない)。
- プロジェクトごとに使う版を固定する機能は無い (Launcher の一覧から版を選んで起動はできる)。
- Runtime.exe (ゲーム側) と Server.exe には更新機能もアイコンも付けない。ゲームの配布はゲームの責務。
