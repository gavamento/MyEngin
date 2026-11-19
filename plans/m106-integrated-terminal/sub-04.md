# sub-04: Terminal 窓

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること
spec 4.1・4.2・4.3。
- `/home/user/MyEngin/src/Editor/Windows/` 配下 (Project か新カテゴリ、手本 `/home/user/MyEngin/src/Editor/Windows/Project/BrowserWindow.h`、`/home/user/MyEngin/src/Editor/Windows/Scene/ParticleSettingsWindow.cpp`) に TerminalWindow。セルを ImGui DrawList で描画 (背景矩形の連続ランをまとめる、全角2セル、カーソル、選択ハイライト)、ツールバー、エラー/終了表示と再起動。
- 等幅フォント: ImGuiTheme (`/home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.cpp:175` 付近) に Cascadia Mono → Consolas の実行時読込を追加し日本語グリフを既存方式で merge。無ければ既定フォント+固定セル幅。Renderer 層への追加が層規則・engine_spec と合うか確認。
- 入力: フォーカス時のキー/IME 確定文字のみ (2026-11-19 ユーザー確定)/貼付/選択コピー、ショートカット抑止 (`/home/user/MyEngin/src/Editor/App/EditorApp.cpp:1970-2011`)。
- 配線: EditorApp メンバ (:199-255)、layouts_.Init (:119-148)、OnImGui (:1007-1341)、Window メニュー (:1930-1962)、SetupDockLayout (:2315-2374)、OnShutdown (:626-)。
- 設定: EditorGlobalSettings に terminalShell / terminalFontSize / terminalScrollback / terminalLoadVsDevEnv (読込のみ。VS 環境の実処理は sub-05)。設定 UI は既存のエディタ設定画面の流儀に合わせる。
- ローカライズ: LocalizationTable.inl 両言語、###ID 一意。
- check_rules: 規則20 (`/home/user/MyEngin/tools/check_rules.ps1:1503-1510`) と同型の新規則で Engine/Runtime/GameLogic/Shared/Server から `Editor[\/]Tools[\/]Terminal[\/]` の include を禁止。
- 文書: engine_spec.md に §9.4 Terminal window を追加、`/home/user/MyEngin/docs/test_checklists.md` に手動確認項目 (Claude Code CLI・git・MSBuild 出力・リサイズ・貼付・終了。sub-03 由来: `terminalconpty` Full の成功、エディタ終了・再起動・子終了時に UI が目立って止まらないこと、終了後に conhost/cmd/pwsh が残らないこと、cmd /c が引用なしスイッチで動くこと)。

## やらないこと (このサブでは)
- VS 開発者環境、AI 追跡。

## 受け入れ条件 (このサブ)
1. spec 5-6, 5-7, 5-10, 5-11。
2. Linux: 追加文字列が LocalizationTable.inl の両言語に存在し ###ID 重複なし (grep/スクリプト)、新規則の正規表現が既存コードで誤検出しない (pwsh があれば check_rules を、無ければ正規表現を grep -P で同等確認)。
3. Linux: 窓の Win32/ImGui 非依存部 (セル→描画ラン変換、選択範囲→文字列、ピクセル→行列計算) を純関数化して g++ テスト。

## 検証コマンド
- Linux: g++、grep、XML 検査。
- Windows (要確認): ビルド、`tools\check_rules.ps1`、`Editor.exe --selftest-full`、手動でスクショ (Claude Code CLI 起動画面、git log、ビルド出力、日本語表示)。
