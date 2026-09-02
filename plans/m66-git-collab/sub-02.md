# sub-02: M66b: CollabClient 完成 + SourceControlState + Source Control 窓 (読み取り専用) + canonicalRoot

- 依存: sub-01
- 状態: OK (commit 165cfc6)
- 往復: 1

## やること

spec §4.1 (op 一覧のうち hello / repo_check / status / hint_changed) / §4.2 (canonicalRoot) / §4.3 / §5 の 4(a)(d)(i), 7。

1. **Rust `watch.rs`**: `notify` crate (Windows は ReadDirectoryChangesW) で toplevel を再帰監視。`.git\` 配下は
   `HEAD` / `index` / `refs\**` / `MERGE_HEAD` / `rebase-merge\` だけ拾う。300 ms デバウンス → worker に `status` を積み
   `status_changed{status}` を通知。`.git\HEAD` が変わったら `repo_changed{head}` も。`hint_changed{paths}` は同じ経路の即時版。
   `cache\` `.mye\` `target\` は無視 (テンプレ `.gitignore` と同じ集合 + `git check-ignore` は呼ばない)。
2. **`CollabClient`** 仕上げ: タイムアウト (hello 5 s / 読み取り系 30 s / 書き込み系なし)、`Unavailable` 列挙 (spec §4.3 の 8 種)、
   `repo_check` の結果と `ctx.projectRoot` の `NormalizePathKey` 比較 → `ToplevelMismatch` / `NotRepo`、`git_missing` / `git_too_old` → `NoGit` / `GitTooOld`、
   `service_error` → `ServiceDied`。`OpInFlight()` (未応答の書き込み系 id があるか)。
3. **`SourceControlState.h/.cpp`**: `status` 応答 → モデル。**対の束ね**: `X`, `X.meta`, `X.terrain.edit` を 1 行 (`PairedEntry{primary, sidecars[], state}`)、
   状態の合成は「最も重いもの」(競合 > D > R > A > M > ?)。**フォルダ集約**: 子の状態から親の状態 (子に M があれば M、`?` だけなら `?`、混在は M)。
   ahead / behind / branch / upstream / mergeInProgress。純関数 (`BuildModel(entries)`) にしてセルフテストから叩ける形に。
4. **`Windows\SourceControlWindow.h/.cpp`**: Changes タブ (対で束ねた一覧、状態バッジ、選択、フォルダ折り畳み)、上部にブランチ名 + ahead/behind、
   利用不可時は理由 (`Tr()`) + 案内 (`NotRepo` なら「`git init` はエディタ外で」)。Branches / History タブは空の枠だけ (sub-03 / sub-05)。
   歯車ポップアップの枠 (sub-06 で中身)。
5. **`EditorApp`**: メンバ、Window メニュー、dock 既定位置、`OnImGui` の先頭で `collab_.Poll()`、起動時 `Load → Create(projectRoot) → hello → repo_check → status`。裸起動は `NoProject` で Create しない。
   `.mye` の外で **`canonicalRoot`**: `ProjectManifest::canonicalRoot` 追加 (`Project.h/.cpp`、`SaveProjectManifest` で書く・`LoadProjectManifest` で読む)、`CreateProject` が作成時パスを書く、
   起動時に `NormalizePathKey(canonicalRoot) != NormalizePathKey(projectRoot)` なら `ToastCenter` に WARN 1 回。窓に「このパスを正にする」ボタン (manifest 書き換え)。
6. **`LocalizationTable.inl`**: en/ja、`###SourceControl` 系 ID の一意性、`error.code` → キーの表 (`CollabErrorText(code)`; 未知は生文字列)。
7. **`SourceControlSelfTest`** 追加: (c1) 偽 status トランスクリプト (M / A / D / R / ? / u / `.meta` 単独 / `.terrain.edit` 同居) → 対の束ねと合成状態、(d) フォルダ集約、(e1) `repo_check` の toplevel 不一致 → `ToplevelMismatch`、(i) `ProjectManifest` の `canonicalRoot` 往復 (一時ディレクトリで Save → Load)。

## やらないこと (このサブでは)

- stage / commit / revert / branch / fetch の UI と op。AssetBrowser のバッジ (sub-09)。

## 触る場所 (planner の見立て)

- 新規: `tools\collab\src\watch.rs`、`src\Editor\SourceControl\SourceControlState.h/.cpp`、`src\Editor\Windows\SourceControlWindow.h/.cpp`。
- 変更: `CollabClient.*`、`ops.rs` / `worker.rs` (通知)、`Cargo.toml` (notify)、`src\Editor\EditorApp.h/.cpp` (メンバ・メニュー・Poll・起動)、
  `src\Engine\Engine\Project.h/.cpp` (`canonicalRoot`)、`src\Editor\ProjectTemplates.cpp` (作成時に書く)、`src\Editor\ProjectManager.cpp` (リネームで失わない = 構造体経由なら無作業)、
  `src\Engine\Core\LocalizationTable.inl`、`SourceControlSelfTest.cpp`。
- 前例: `NetWindow.h` (Editor 層が POD を読む境界)、`ToastCenter::Notify`、`ImGuiTheme.h` の 5 箇条 (バッジ色は `themeColor::*`)、`EditorSettings.cpp` の `value(key, default)`。

## 受け入れ条件 (このサブ)

1. `--selftest` 緑 (c1 / d / e1 / i を含む)。
2. `check_rules.ps1` 緑 (規則 10: 新規文字列が en/ja 両方、`###` 一意)。
3. 実機 (fixture を `--project` で開く): 窓に status が対で束なって出る (`.meta` が本体と同じ行)。ファイルを外部で書き換えると 1 s 以内に一覧が変わる (notify 経路)。
4. 実機: `MyeCollab.dll` をリネームして起動 → `NoService` + 他機能無傷 (Play / 保存 / AssetBrowser)。裸起動 → `NoProject`。fixture の外 (git でないディレクトリ) を `--project` → `NotRepo`。
5. 実機: `canonicalRoot` を手で書き換えて起動 → WARN トースト 1 回。
6. `cargo test` / `collab_verify.bat` / `replay_verify.bat` 緑 (無変更)。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
pwsh -File tools\collab_fixture.ps1 cache\fixture_proj   (目視用)
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

```
SELF_EVAL: sub-02 (round 1)
実装:
  - tools/collab/src/watch.rs:is_interesting / relative_key / spawn — notify 8 (既定機能オフ) で
    toplevel を再帰監視。.git\ は HEAD / index / MERGE_HEAD / refs/** / rebase-merge/ /
    rebase-apply/ だけ拾い、.mye / cache / dist / target は捨てる。300 ms デバウンス +
    2 s の上限 (書き込みが続く環境で永久に更新されないのを防ぐ)。判定は純関数で単体検査
  - tools/collab/src/worker.rs:Service::with_watch / Msg::Refresh / handle_refresh —
    監視スレッドは worker に Refresh を送るだけ (git は 1 本のスレッドで直列のまま)。
    Drop は監視 -> worker の順。CLI (Service::new) は監視を立てない
  - tools/collab/src/ops.rs:refresh_status / build_status / status / hint_changed —
    status の結果に "head" (porcelain の branch.oid) を追加し、HEAD 移動の検知に
    rev-parse を足さずに済ませた。前回と同じ結果なら status_changed を出さない
  - src/Editor/SourceControl/SourceControlState.*:BuildModel / PrimaryPathFor /
    CombineState / EvaluateRepoCheck / SourceControlSession — 対の束ね、フォルダ集約、
    repo_check 判定はすべて純関数。セッションは hello -> repo_check -> status の順で起動
  - src/Editor/SourceControl/CollabClient.*:ExpireTimedOut / OpInFlight / SetUnavailable —
    hello 5 s / 読み取り 30 s / 書き込み無期限。期限切れは timeout 応答を合成して配る
  - src/Editor/Windows/SourceControlWindow.* — Changes タブ (バッジ + 折り畳み + 選択 +
    ツールチップ)、Branches / History は枠、歯車ポップアップの枠、利用不可の理由 8 種
  - src/Engine/Engine/Project.*:ProjectManifest::canonicalRoot — 空ならキーごと書かない
  - src/Editor/EditorApp.* — メンバ / Window メニュー / dock 既定 / Poll / Start / Shutdown
  - tools/collab_verify.ps1:New-Fixture — fixture をシナリオ 1 本ごとに作り直す
検証: 下記「検証コマンド」を全部実行、すべて緑 (詳細は SELF_EVAL 本文)
```

## フィードバック履歴

## planner 追記 (sub-01 round 1 の判定から。coder は着手前に読む)

- **CLI (script モード) は event 行を出さない** (spec §4.4 に追記済み)。watch.rs を足すとき、Service の生成に「watcher / 定期 fetch を起動するか」のフラグを持たせ、main.rs 側は off、lib.rs (create) 側は on にする。そうしないと collab_verify の出力が非決定になる (coder の CLI は EOF 後 200 ms drain で終了する設計)。
- [should] porcelain の fixture に **非 ASCII パス** (例: `assets/textures/日本語.png`、core.quotepath=false で生 UTF-8) を 1 本足し、UTF-8 のまま entries.path に載ることを cargo test で示す。collab_verify は cmd 経由で ASCII 限定なので、ここでしか検査できない。
- [nit] CollabClient::AddPendingForTest はテスト専用の公開 API。sub-02 で DispatchLine の検査経路が増えるなら、friend か「Request の DLL 無し分岐」に寄せられないか検討 (無理なら現状維持で可)。
- [nit] ci.yml の `dtolnay/rust-toolchain@stable` ステップに `name:` が無い (ログで探しにくい)。sub-10 でまとめて直してもよい。
- [nit] cargo build のたびに `warning: linker stdout: ライブラリ ... を作成中` が 1 件出る (MSVC リンカの情報行)。CI は -D warnings ではないので実害なし。触らない。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: `TerrainEdit.cpp:469 EditPathFor` は `.json` を `.edit` に差し替える = 逸脱 1 は coder が正しく sub-02 の字面が誤り (spec §4.1 と sub-03 を修正)。`CombineState` は最重 (`SourceControlState.cpp:71`)、`LocalizationTable.inl:973-976` の `tools\build_collab.bat` は正しくエスケープ済み (cat -A で確認)、`Project.cpp:40,57` で canonicalRoot は value() 読み・空なら書かない、`watch.rs:37-` の除外集合、`cache/replay_m66b2.log:3711` に [PASS]。`tests/actual/scm_final.png` で Assets と同束のタブ・5 changed・M/? の束ね表示を目視。質問 1〜5 は全て承認、6 は「プロジェクト起動で既定表示」に裁定 (sub-03 で実装)。
