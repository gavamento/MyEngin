# sub-04: M66d: ReloadHub Begin/EndBatch + ゲート + 4 窓の HasUnsavedChanges + トランザクション + revert

- 依存: sub-03
- 状態: OK (commit: M66d — ハッシュは harness.md のサブ進捗表)
- 往復: 2

## やること

spec §4.1 (段階分類 / ゲート / 4 窓 / S4 / S5 / S8) / §5 の 4(e)(f)(h), 5, 9, 17。**このマイルストーンの核**。

1. **`ReloadHub::BeginBatch()` / `EndBatch(const std::vector<BatchChange>&)`** (`src\Engine\Engine\HotReload\ReloadHub.{h,cpp}`):
   `BatchChange{std::wstring path; enum Kind{Modified, Added, Deleted, Renamed}; std::wstring oldPath;}` (Engine 層の汎用型。Collab を知らない)。
   batch 中は `Update()` が watcher を drain するだけで `HandleChange` しない。`EndBatch` は渡された変更を純関数 `OrderBatch()` で
   **種別順 (texture → mat → model → actor → scene)、同種は `NormalizePathKey` 昇順**に並べて `HandleChange`。`Deleted` は `OrderBatch` の出力から**除外** (S4: watcher からも来ない。呼び手が B 段階にする)。
   `EndBatch` 後、溜まっていた watcher 分は**破棄**。`Retry::attempts` を増やし `kReloadRetryMax = 60` で諦めて WARN (S4 の衛生)。**batch 外の挙動は不変**。
2. **`Added` の登録 (S5)**: `EndBatch` の前に Editor 側が `assets\` 配下の Added を AssetDatabase に登録する。方式は spec §7 (増分登録 vs `ScanAndSync` 再実行) を
   coder が比較して選び、理由を実装メモに残す。`.meta` が同梱されていればその guid を採る。
3. **アクセサ追加**: `BuildSettingsWindow::IsRunning()` (`proc_ != nullptr`、`.h:29-30` の隣)。`StartGameLogicBuild` の全呼び出し元を洗い、同期ビルド経路があれば「実行中」に含める。
   **4 窓 `HasUnsavedChanges()`** (S6 = a): Animation / AnimatorController / AudioMixer / ProjectSettings。判定 = Save が書く JSON をメモリから直列化 → ディスクの現物と比較 (無ければ「メモリに内容があれば dirty」)。
   500 ms キャッシュ (`GitTransaction` 側で持つ)。ProjectSettings の render-path ラジオは対象外。
4. **`GitTransaction`** (`src\Editor\SourceControl\GitTransaction.h/.cpp`):
   - `CanRunGitWriteOp(std::vector<GateBlocker>&)`: spec §4.1 の 13 種を**全件**列挙。純関数 `ComputeBlockers(const GateInputs&)` に分離してセルフテスト可能に。
   - `Begin(op, args)`: `AudioSystem::StopMusic(0)` → `TextureLibrary::WaitForAsyncLoads()` → `ReloadHub::BeginBatch()` → モーダル (他窓の入力遮断、`ProcessPendingFileDrops` 保留、Play / Save / Build のショートカット無効) → `CollabClient::Request`。
   - 応答: 変更集合 (HEAD が動く op は `diff_names(before, after)`、revert は対象パス) → `Classify()` (spec §4.1 の表、純関数) → A: Added 登録 → `EndBatch(A集合)` / B: `EndBatch(A集合)` → `LoadSceneFromPath` 経路 (削除されていれば `NewScene` + トースト、`lastScenePath` 消去) → `.cs` は `CompileScripts` → `.cpp` は「Rebuild Scripts」トースト / C: `EndBatch({})` → 「再起動します」確認 → `RelaunchSelfWithProject` → 終了。
   - エラー: `EndBatch({})` → モーダルに `error.code` の訳文 + `paths[]` → 閉じる。
   - `.controller.json` / `.terrain.json` の B: ライブラリのキャッシュ無効化 API の有無を確認 (無ければ C に格上げし、実装メモに記録)。
5. **revert**: Rust `revert{paths}` = 追跡済みは `git checkout -- <paths>`、未追跡 (`?`) は削除 (`git clean -f -- <path>`)。C++ は対 (PairRule) で送り、確認モーダルに「未追跡ファイルは削除されます」を出す。
   Changes タブに「選択を破棄」「すべて破棄」。トランザクション経由 (ゲート適用)。
6. **セルフテスト**: (e) `ComputeBlockers` に各条件を 1 つずつ立てて期待リスト (13 ケース + 複合 1) / (f) `OrderBatch` の順序 (種別混在 + 同種昇順) と `Deleted` 除外 / (f2) `Classify` の表 (A / B / C / 混在で最重 / `D` → B / actor+scene → B / 201 件 → C) / (h) `EditorSettings` は sub-06 で。

## やらないこと (このサブでは)

- checkout / pull / merge の op (sub-05〜07)。トランザクションは revert でだけ通す。

## 触る場所 (planner の見立て)

- 変更: `src\Engine\Engine\HotReload\ReloadHub.{h,cpp}`、`src\Editor\Windows\BuildSettingsWindow.{h,cpp}`、`AnimationWindow.*` / `AnimatorControllerWindow.*` / `AudioMixerWindow.*` / `ProjectSettingsWindow.*` (アクセサ追加のみ)、
  `src\Editor\EditorApp.{h,cpp}` (モーダル中の抑止、`ExecuteAction` の再利用 `:1248-1277`、`LoadSceneFromPath :1218-1234`)、`SourceControlWindow.cpp`、`ops.rs` (revert / diff_names)、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`。
- 新規: `src\Editor\SourceControl\GitTransaction.h/.cpp`、`StageClassifier.h/.cpp` (純関数)。
- 参照: `AudioSystem.h:192` (`StopMusic`)、`GpuResources.h:134`、`ProjectManager.cpp:456-469` (`RelaunchSelfWithProject`)、`PlayModeController.h:10` (`PlayState`)、`NetRuntime.h:22` (`active`)、`ReloadHub.cpp:82-95` (現行の drain とリトライ)。

## 受け入れ条件 (このサブ)

1. `--selftest` 緑 (e / f / f2)。
2. 実機 (fixture): Play 中 → 「選択を破棄」が無効 + ツールチップに `Playing`。シーン dirty → `SceneDirty`。Animation 窓で値を変えて未保存 → `AnimationDirty`、保存すると消える (500 ms 以内)。
3. 実機: アクティブシーンを変更して保存 → 破棄 → シーンが開き直り dirty でない。テクスチャだけ変えて破棄 → 再起動なしで絵が戻る (A)。
4. 実機 (エンジンリポ裸起動、§5-17): hlsl / png / シーン外部編集のホットリロードが従来どおり (batch 外の不変)。
5. `tools\replay_verify.bat` **無変更緑** (ReloadHub は Engine 層 — sim には触れていないことの証明。ここが最重要)。
6. `check_rules.ps1` / `cargo test` / `collab_verify.bat` 緑。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視 2, 3)
cmd /c bin\x64\Debug\Editor.exe                                (目視 4: 裸起動 → 既定デモ)
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

```
SELF_EVAL: sub-04 (round 1)
実装:
  - tools/collab/src/ops.rs:revert — status を 1 回読んで追跡済み/未追跡に振り分け、
    追跡済みは `git checkout -- <paths>`、未追跡は `git clean -q -f -- <paths>`。
    応答は `{reverted, deleted, status}` (status_after_write 経由で last_head も更新)
  - tools/collab/src/ops.rs:diff_names — `git diff --name-status -z --no-renames <from>..<to>`。
    リビジョンは `-` 始まり / 空白を弾く。C++ 側の呼び手は sub-05
  - tools/collab/src/porcelain.rs:parse_name_status_z — R/C だけ 3 レコードになる形を扱う
  - ReloadHub.h/.cpp:BatchChange / OrderBatch / BeginBatch / EndBatch / kReloadRetryMax=60
  - StageClassifier.h/.cpp:ApplyStage / StageChange / ClassifyChange / Classify (全部純関数)
  - GitTransaction.h/.cpp:GateBlocker(13) / ComputeBlockers / GitTransaction (確認 →
    実行前処理 → revert → 段階 A/B/C の後処理 → モーダル)
  - DocumentDirty.h/.cpp:TextDiffersFromDisk (改行と末尾空白を無視して比較)
  - AnimationWindow / AnimatorControllerWindow / AudioMixerWindow / ProjectSettingsWindow:
    HasUnsavedChanges()、PhysicsLayerNames / PartTagNames: DiffersFromDisk()
  - BuildSettingsWindow.h:IsRunning / IsPipelineRunning / IsScriptBuildRunning
  - SourceControlState:Revert / SourceControlWindow:破棄 2 ボタン + 阻害要因ツールチップ +
    identity 未設定でコミット無効化 (planner の should)
  - EditorApp:BuildGateInputs / フック配線 / モーダル中のショートカットと D&D 抑止
  - tests/collab/05_revert.ndjson + expected、cargo test 8 本、selftest (e)(f)(f2) + 実 DLL プローブ
仕様との差分:
  - [追加] `src/GameLogic/Scripts/` の `.h/.hpp/.inl` も段階 B (spec は `*.cpp` のみ)
  - [追加] 段階 B は「A 段階の変更だけ」を EndBatch へ渡す (B の引き金を渡すと
    開き直す直前に捨てられる ApplyDiff が 1 回走る)
  - [追加] `.meta` の guid 変化は「実行の前後でディスクの .meta を読み比べる」で判定
  - [追加] 未追跡は `git clean -q -f` (`-d` は付けない)
  - [未実装] `diff_names` の C++ 呼び手は無い (sub-05)
検証: (SELF_EVAL 本体を参照。cargo test / selftest 両構成 / check_rules / collab_verify /
  replay_verify すべて緑、実機は fixture の一時プローブで A・B の 2 経路を実走)
```

### round 2 (VERDICT round 1 の must 1 への対応)

```
SELF_EVAL: sub-04 (round 2)
実装:
  - #1: GitTransaction.cpp の段階 C モーダルから「あとで」(Scm_RestartLater) を撤去。
    ボタンは『再起動』1 個だけ。LocalizationTable.inl の Scm_RestartLater も削除。
    ★失敗経路を塞いだ: Hooks::relaunch を bool 返しに変え、RelaunchSelfWithProject が
    失敗したら**モーダルを閉じない** (閉じると「あとで」と同じ状態になる)。
    代わりに赤字で Scm_RestartFailed を出す (新規 1 組。差し引き +-0)。
検証: --selftest Debug/Release 緑、check_rules 0/0、collab_verify 5 本緑、
  cache\scm_m66d_restart.png = ボタン 1 個の再起動モーダル (schemas の revert で
  実際に段階 C へ到達させて撮影)
```

## フィードバック履歴

## planner 追記 (sub-03 round 1 の判定から。coder は着手前に読む)

- [should] identity (`user.name` / `user.email`) 未設定のときコミットボタンを **無効化** する (spec §4.1「commit 周り」)。今は案内のみ。`scm.IdentityChecked() && !scm.IdentityOk()` の分岐に `BeginDisabled` を足すだけ (SourceControlWindow.cpp:456 付近)。ゲート (`GateBlocker`) には入れない — commit は working tree を書かないので別系統。
- 書き込み系 op (revert) の応答は sub-03 の `status_after_write` の型 (`{"status": ...}`) に揃え、C++ は `ApplyWriteResult` へ流す。**`last_head` の更新を忘れると自分の操作で「外部で HEAD が移動」トーストが出る** (coder 申し送り)。
- 一時プローブのレシピ (coder 申し送り): `MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` で Session を実 DLL 経由で駆動できる。ゲートとトランザクションの結線検証に使うこと (受け入れ条件 2・3 の目視の前に)。
- ServiceDied の実機経路は未検証のまま (sub-02 から持ち越し)。`GateBlocker::ServiceUnavailable` を検査するとき、cargo test の panic 注入 (service.rs) を DLL 経由で 1 回通し、窓が ServiceDied を出すことをスクショで 1 枚残す。
- round 1: VERDICT REWORK (planner、2026-09-03)。must 1 件 = 段階 C モーダルの「あとで」(GitTransaction.cpp:557) を撤去し『再起動』のみにする (spec §4.1 C 行を先に確定)。裏取り: ReloadHub.cpp の diff で `if (batching_)` 早期 return + `EndBatch` = DiscardPendingChanges → OrderBatch (Deleted 除外) → HandleChange + `retryAttempt_` の引き継ぎと `kReloadRetryMax` の give-up を確認 / StageClassifier.cpp:52,63-64 で schemas と `.cpp .h .hpp .inl` / SourceControlSelfTest.cpp:553 の static_assert / SourceControlWindow.cpp:536-538 の canCommit で identity 無効化 (sub-03 の should 消込) / ops.rs:477 `clean -q -f` (`-d` 無し) / GitTransaction.cpp:282-301 増分登録、:340-356 controller・terrain の無効化 / golden*.rep が 08:42 再生成 (replay 最終状態) / cache/scm_m66d_modal.png で確認モーダル (件数・赤字の未追跡削除警告・パス一覧・段階予測・暗転) を目視。質問 1 = a、2 = 仕様側を `.h` 込みに、3 = v1 は常に開き直し、4 = 現状可、5 = fixture 拡張は sub-05 へ。
- round 2: VERDICT OK (planner、2026-09-03)。must 1 の消込を現物で確認: `grep RestartLater src/ tools/` = 0 件、GitTransaction.cpp:555-571 = ボタンは Scm_RestartNow の 1 個、relaunch 失敗で restartFailed_ を立てて閉じない、成功時のみ CloseCurrentPopup、GitTransaction.h:92 / EditorApp.cpp:266 で relaunch は bool 契約、cache/scm_m66d_restart.png で「Restart required」+ ボタン 1 個 + 暗転を目視 (段階 C へ実到達させて撮影)。replay 再実行は不要と判断: このサブで Engine 層に触れたのは ReloadHub.{h,cpp} (round 1 の replay で緑) と LocalizationTable.inl (UI 文字列) だけ (`git diff --stat -- src/Engine` で確認)。nit: coder の「src/Engine に 1 バイトも触れていない」は不正確 (LocalizationTable.inl は src/Engine/Core にある) — 実害なし。質問 1 = spec §4.1 C 行に採用。
