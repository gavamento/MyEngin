# sub-08: M66h: 衛生 — .gitignore テンプレ 4 行 + project_settings.json の個人設定分離

- 依存: sub-03 (sub-05〜07 とは独立)
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴

## planner 追記 (sub-05 round 1 の判定から。coder は着手前に読む)

- [should] **スクリプトビルド失敗時のエラー行を Console へ流す**。sub-05 で Asset Browser の [Rebuild Scripts] を `StartGameLogicBuild` 経由 (出力は `<project>/cache/build_scripts.log` + トースト) に一本化した結果、旧経路の可視 cmd 窓 (bat が失敗時に `pause` するのでその場でコンパイルエラーが読めた) が消えた。同等の UX として、`EditorApp::PollScriptBuild` が失敗を検知したら log の `error` を含む行 (MSVC の `path(line): error Cxxxx:` 形式) を `MYE_LOG_ERROR` で Console へ流す。Console の double-click ソースジャンプ (`ConsoleWindow.cpp:33-49 JumpToSource`) が `file:line` を拾える形に整えること。成功時は流さない (トーストのみ)。
