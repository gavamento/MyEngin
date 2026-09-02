# sub-06: M66f: fetch / pull / push + 定期 fetch + 通知 + EditorSettings

- 依存: sub-04 (sub-05 とは独立。同時に進めるなら `SourceControlWindow.cpp` と `ops.rs` のマージ順を司会が決める)
- 状態: 未着手
- 往復: 0

## やること

spec §4.1 (op: fetch / pull / push / remote_state、背景 fetch と認証) / §4.2 (EditorSettings) / 決定 5, 6 / §5 の 2 (2 clone), 4(h), 11。

1. **Rust ops**: `fetch` (`git fetch --prune`) / `pull{allowMerge}` (既定 `--ff-only`、非 ff は `non_fast_forward`; `allowMerge=true` は `--no-rebase`、競合は `conflict` = sub-07) /
   `push{setUpstream}` (upstream 無しなら `-u origin <branch>`、拒否は `non_fast_forward`、認証失敗は `auth_failed`、到達不能は `network`) /
   `remote_state` (`{upstream, ahead, behind, commits[{sha, author, date, subject}]}`、`commits` は `HEAD..@{u}` の最大 20 件)。
   **定期 fetch**: worker が `hello` の `autoFetch` / `fetchIntervalMin` に従い、起動直後 + 間隔ごとに `fetch` → `remote_state` → 変化があれば `remote_changed` を通知。
   背景 fetch は env `GIT_TERMINAL_PROMPT=0` + `GCM_INTERACTIVE=never`、失敗は**同じ `error.code` が続く間 1 回だけ** `service_error` ではなく `remote_changed{error}` で通知。
   ユーザー操作の fetch / pull / push は `GIT_TERMINAL_PROMPT=0` のみ (GCM の GUI を許す)。設定変更は `hello` の再送で反映。
2. **C++**: Changes タブ上部に帯「upstream に N 件の新しいコミット」(展開でコミット一覧: author / subject) + `ToastCenter` (起動時 fetch の結果)。
   ボタン: Fetch (ゲート不要、`OpInFlight` のみ) / Pull (トランザクション経由 = ゲート適用、`non_fast_forward` → 「マージして pull」を提示 → `allowMerge=true` で再実行) / Push (ゲート不要、`non_fast_forward` → 「先に pull」、`auth_failed` → Credential Manager の案内)。
   **自動 pull はしない**。
3. **`EditorSettings`**: `scmAutoFetch` (true) / `scmFetchIntervalMin` (5)。窓の歯車ポップアップで編集 → `Save()` → `hello` 再送。
4. **冒頭確認**: GUI 無しの Editor.exe (実際は GUI アプリ) の孫プロセス git から GCM のダイアログが出るか、認証未設定の https リモートで 1 回実験して実装メモに結果を書く。出なければ `auth_failed` の案内文を「ターミナルで一度 `git push` して認証を通してください」にする。
5. **collab_verify**: `tests\collab\04_remote.ndjson` = bare リポを origin に 2 クローン (A / B) → A で commit → push → B で fetch → `remote_state` behind=1 → pull → 一致 / B の非 ff push → `non_fast_forward`。
6. **セルフテスト (h)**: `EditorSettings` の新キーを一時ディレクトリで Save → Load 往復、旧 JSON (キー無し) の既定値。

## やらないこと (このサブでは)

- 競合の解決 UI (sub-07)。認証 UI。

## 触る場所 (planner の見立て)

- 変更: `ops.rs` / `worker.rs` (タイマー)、`SourceControlWindow.cpp`、`GitTransaction.cpp` (pull の再実行分岐)、`src\Editor\EditorSettings.{h,cpp}`、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`、`tests\collab\04_*.ndjson`、`tools\collab_verify.ps1` (bare origin の作成)。

## 受け入れ条件 (このサブ)

1. `collab_verify.bat` 緑 (04 を含む)。
2. `--selftest` 緑 (h)。
3. 実機: 2 つ目のクローンから push → fixture 側のエディタで 5 分以内 (間隔を 1 分にして試す) にトースト + 帯。Pull → シーンが開き直る (B) か絵が変わる (A)。push → `git log origin/main` に載る。
4. 実機: 非 ff push → 「先に pull」。`scmAutoFetch=false` で fetch が止まる (Rust のログで確認)。
5. `cargo test` / `check_rules.ps1` / `replay_verify.bat` 緑。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視。origin は collab_verify が作る bare を流用してよい)
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

## フィードバック履歴
