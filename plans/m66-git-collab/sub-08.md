# sub-08: M66h: 衛生 — .gitignore テンプレ 4 行 + project_settings.json の個人設定分離

- 依存: sub-03 (sub-05〜07 とは独立)
- 状態: OK (commit: M66h — ハッシュは harness.md のサブ進捗表)
- 往復: 1

## やること

決定 8 / S10 / §4.2 (EditorSettings の粒子 3 キー) / §5 の 4(g), 6, 13。

1. **テンプレ `.gitignore`** (`ProjectTemplates.cpp:118`): `/.mye/` `/cache/` `/dist/` に **`/save/` `/crash/` `/assets/scripts/Generated/` `*.log`** を足す (計 7 行)。
   Source Control 窓に「推奨 .gitignore を適用」: 純関数 `MissingGitignoreLines(existingText) → lines` で**不足行だけ末尾に追記**、既存行は触らない。無ければ新規作成。
2. **決定 8 (粒子設定の分離)**:
   - `ParticleSystem.cpp:130-154` の `SaveSettings` から `particleCompareMode` / `particleCompareOffsetX` / `particleCpuSimd` の**書き出しと読み込みを撤去** (旧 JSON に残っていても読み飛ばす)。`particleBackend` の読みは残す。
   - `EditorSettings` に 3 キー (既定は現行の既定値と同じ)。Editor が起動時と変更時に `ParticleSystem` の setter へ流す (Engine 層は `EditorSettings` を知らない = 呼ぶのは Editor 側)。
   - `particleBackend` の**書き戻しは `ProjectSettingsWindow` の Apply だけ**に限定。Particle Settings 窓のトグルはセッション上書き (`--particle-backend` CLI と同じ「書き戻さない」)。`ParticleSystem.cpp:26-31` の「★呼んではいけない」注記を現状に合わせて書き直す。
   - Runtime.exe は compare / simd を CLI (`--particle-compare` 等) でしか受けなくなる = 仕様どおり (ADR-008: SIMD on/off はビット一致)。
3. **セルフテスト**: `ParticleSelfTest` に「compare / simd を変えても `project_settings.json` のバイト列が変わらない」、`SourceControlSelfTest` に `MissingGitignoreLines` (空 / 一部あり / 全部あり / 末尾改行なし)。
   テンプレ生成 (`CreateProject` を一時ディレクトリで) の `.gitignore` が 7 行。

## やらないこと (このサブでは)

- `partTags` / `physicsLayers` (プロジェクト設定として正しい置き場) は動かさない。

## 触る場所 (planner の見立て)

- 変更: `src\Editor\ProjectTemplates.cpp`、`src\Engine\Engine\Particles\ParticleSystem.{h,cpp}` (永続化のみ。**sim 経路には触れない**)、`src\Editor\EditorSettings.{h,cpp}`、`src\Editor\EditorApp.cpp` (起動時の受け渡し)、
  `src\Editor\Windows\ProjectSettingsWindow.cpp` (backend 行 + Apply)、Particle Settings 窓 (トグルの保存呼び出しを外す)、`SourceControlWindow.cpp` (適用ボタン)、`ParticleSelfTest.cpp`、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`。

## 受け入れ条件 (このサブ)

1. `--selftest` 緑 (Particle の不変検査 + gitignore 4 ケース + テンプレ 7 行)。
2. **`tools\shot_verify.bat` 無変更緑** (golden 16/17 = 粒子 2 枚。`--particle-backend` の CLI 固定と非書き戻しが不変の証明。先に Release ビルド)。
3. 実機 (fixture): Particle Settings 窓で compare を切り替えても `assets\project_settings.json` の mtime が変わらない。「推奨 .gitignore を適用」で 4 行が末尾に増え、既存行は無傷。
4. `replay_verify.bat` 無変更緑 (`ParticleSystem` を触るので必須)。`check_rules.ps1` 緑。

## 検証コマンド

```
cmd /c bin\x64\Debug\Editor.exe --selftest
tools\replay_verify.bat          (Release ビルドも兼ねる)
tools\shot_verify.bat
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視)
```

## 実装メモ (coder が追記)

### round 1

```
SELF_EVAL: sub-08 (round 1)
実装:
  - src/Editor/ProjectTemplates.{h,cpp}:kRecommendedGitignore / RecommendedGitignoreText /
    MissingGitignoreLines / GitignoreWithRecommended — 推奨 7 行を**ここだけに置き**、
    CreateProject も Source Control 窓も同じ配列から作る。不足行の算出と追記本文の
    組み立ては**純関数** (既存部分はバイト単位で不変)
  - src/Editor/Windows/SourceControlWindow.{h,cpp}:DrawHeader — 設定ポップアップに
    「推奨 .gitignore を適用」。不足が無ければ BeginDisabled、ツールチップに追記予定の行を列挙。
    host に `gitignoreMissing` (呼ばれたときだけ読む) / `applyGitignore` を追加
  - src/Editor/EditorApp.cpp:OnImGui — 上記 2 つの実体 (読む→純関数→書く + トースト + status 再取得)
  - src/Engine/Engine/Particles/ParticleSystem.{h,cpp} — 決定 8。LoadSettings(path) を public にし
    **particleBackend だけ**を読む/書く。SetActiveKind / SetCompareMode の SaveSettings 呼び出しを撤去し、
    SetCompareOffsetX / CompareOverriddenByCli を追加。旧の「★呼んではいけない」注記を現状に合わせて書き直し
  - src/Editor/EditorSettings.{h,cpp} — particleCompareMode / particleCompareOffsetX /
    particleCpuSimd (既定は旧値と同じ false / 4.0 / true)
  - src/Editor/EditorApp.cpp:OnStart — 起動時に個人設定を ParticleSystem へ流す。
    `--particle-compare` で起動していたら CLI を勝たせる
  - src/Editor/Windows/ParticleSettingsWindow.{h,cpp} — OnImGui(ctx, settings)。compare / simd は
    editor_settings.json へ、バックエンドはセッション上書き (注記行を 1 行追加)
  - src/Editor/Windows/ProjectSettingsWindow.{h,cpp} — Rendering 節に「パーティクルバックエンド」+
    「プロジェクト既定にする」。**project_settings.json へ書く唯一の口**。
    HasUnsavedChanges の対象外である理由をヘッダに明記
  - src/Editor/AssetOps.{h,cpp}:ParseBuildErrorLines + EditorApp::ReportScriptBuildErrors —
    planner 追記 (should): スクリプトビルド失敗時の error 行を Console へ。
    logging::WriteSrc を直接呼び `path(line)` を LogEntry.file/line に乗せる (JumpToSource が使える形)
  - src/Editor/DocumentDirty.{h,cpp} → DiskCompare.{h,cpp} に `git mv` (planner 追記 a)。
    include 4 ファイル更新 + gen_project_files.ps1
  - src/Engine/Core/LocalizationTable.inl — en/ja 9 件追加、未使用の Scm_ComingSoon を削除 (planner 追記 b)
  - tools/collab/src/git.rs:classify_error — "you have unmerged files" → merge_in_progress (planner 追記 c)
  - tools/collab_fixture.ps1 — fixture の .gitignore を 7 行へ (テンプレと揃える)
  - テスト: ParticleSelfTest (永続化不変 6 件) / SourceControlSelfTest (g) (gitignore 13 件) /
    AssetOpsSelfTest (ビルドログ分解 6 件) / cargo test 1 件
仕様との差分:
  - [追加] 旧 project_settings.json の個人 3 キーは**読み飛ばすだけで移行しない**し、SaveSettings でも**消さない**。
    理由: 移行すると「共有ファイルの古い値があとから個人設定を上書きする」順序問題が残る。
    消すと Project Settings を一度保存しただけで共有ファイルに 3 行の削除差分が出る
  - [追加] ProjectSettingsWindow のバックエンドラジオは HasUnsavedChanges の**対象外**。
    入れると `--particle-backend gpu` 起動だけで書き込み系 git のゲートが永久に閉じる
  - [未実装] watch.rs::is_interesting の除外集合は**揃えない** (台帳の衛生項目 4)。
    理由: 監視は全プロジェクト共通だが、新しい 4 行を持たない既存プロジェクトでは
    crash/ や *.log が追跡対象でありうる。除外するとその人の一覧が永久に古くなる
    (watch.rs の設計原則「拾えないより拾いすぎに倒す」そのもの)
検証:
  - Editor.exe --selftest (Debug / Release) → exit 0、FAIL 0 件 (新規 27 件全 PASS)
  - tools\replay_verify.bat → PASS (10 job)  / tools\shot_verify.bat → PASS (19 枚)
  - pwsh check_rules.ps1 → 0 error / 0 warning
  - cargo test → 35 + 24 + 6 PASS (merge_in_progress 1 件追加)  / collab_verify.bat → 9 シナリオ PASS
  - 実機 (cache\m66h_fx): .gitignore に 4 行追記 + 既存 5 行無傷 /
    project_settings.json の mtime と SHA256 が 4 セッションを通して同一 /
    スクショ 3 枚 (cache\m66h_ui_scm.png / _popup.png / _enabled.png)
  - ★shot_verify の 1 回目は acoustic_deferred で FAIL。**本サブと無関係の既存のフレーク**と断定:
    同じ Release バイナリで acoustic_forward / acoustic_deferred とも 1/4〜1/2 の確率で
    (237,443) 付近の ~40x40 画素だけが動く (maxDiff 120〜145)。ログは PID 以外完全一致。
    2 回目以降は 19 枚 PASS
自己採点 (1-5):
  仕様適合: 5 — 決定 8 / S10 / 台帳の衛生 4 件 + planner 追記 (a)(b)(c) を全部。
              見送った watch.rs は差分欄に理由付きで記載
  正しさ: 4 — 受け入れ 1/2/4 はコマンドで、3 は実機のバイト列で確認。
            壊れた C++ スクリプトを実際にビルドして Console へ出るところだけ未実走 (分解は単体検査済み)
  コード品質: 4 — 永続化と追記のロジックを純関数へ抜いた。UI のボタン押下自体は目視代替
  テスト: 4 — 新規 27 件 + cargo 1 件。UI クリックと実ビルド失敗の 2 経路がプローブ代替
不安・質問: (SELF_EVAL 本文を参照 — 司会の転記に (a) DiskCompare 改名が無かった件、
            golden のフレーク、推奨 .gitignore ボタンの置き場)
```

## フィードバック履歴

## planner 追記 (sub-05 round 1 の判定から。coder は着手前に読む)

- [should] **スクリプトビルド失敗時のエラー行を Console へ流す**。sub-05 で Asset Browser の [Rebuild Scripts] を `StartGameLogicBuild` 経由 (出力は `<project>/cache/build_scripts.log` + トースト) に一本化した結果、旧経路の可視 cmd 窓 (bat が失敗時に `pause` するのでその場でコンパイルエラーが読めた) が消えた。同等の UX として、`EditorApp::PollScriptBuild` が失敗を検知したら log の `error` を含む行 (MSVC の `path(line): error Cxxxx:` 形式) を `MYE_LOG_ERROR` で Console へ流す。Console の double-click ソースジャンプ (`ConsoleWindow.cpp:33-49 JumpToSource`) が `file:line` を拾える形に整えること。成功時は流さない (トーストのみ)。
- [should、sub-05〜07 から集約した衛生のコード 3 件] (a) `src/Editor/DocumentDirty.h/.cpp` を `DiskCompare.h/.cpp` 等へ改名 (`DocumentDirty` 構造体は GitTransaction.h 側にあり紛らわしい。`gen_project_files.ps1` を回す) / (b) 未使用の `Scm_ComingSoon` を LocalizationTable.inl から削除 / (c) `classify_error` に "Pulling is not possible because you have unmerged files" → `merge_in_progress` を追加 (cargo test 1 本)。sub-10 はドキュメント専用なので、コードの衛生はここで閉じる。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: ParticleSystem.cpp:120-153 で読み書きとも `particleBackend` のみ (旧 3 キーは読まず消さず) / `git status` に `RM DocumentDirty.* -> DiskCompare.*` / ProjectTemplates.cpp に `/save/` `/crash/` `*.log` `/assets/scripts/Generated/` / `git diff --stat -- src/Engine` = Particle 2 本 + ParticleSelfTest + LocalizationTable のみ (音響に触れていない) / エンジンリポの `assets/project_settings.json` の 3 キーは既定値と同じなので読み飛ばしても挙動不変 / golden*.rep 17:07 再生成、tests/actual の acoustic diff 17:12 (フレークの現物)。acoustic フレークは `git log -- tests/golden/acoustic_deferred.png` = M65g が最後、その後 04b0733 (M65h) が AcousticField / RenderSystem / TickRunner に触れている = M66 以前からの描画側非決定性と断定。扱いはユーザーへ質問 (sub-10 で反映)。質問 1 = sub が正本で改名は正しい、3 = 設定ポップアップ内で可 (帯は v1.5)。
