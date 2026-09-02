# sub-09: M66i: Content Browser に Git バッジ + 保存直後のヒント

- 依存: sub-02 (sub-03〜08 とは独立)
- 状態: 未着手
- 往復: 0

## やること

決定 4 / §4.3 / §5 の 14。

1. **`SourceControlState`** に `StateFor(normKey) → std::optional<EntryState>` と `FolderStateFor(normDirKey)` (集約はキャッシュ、`status_changed` で無効化)。
2. **`AssetBrowserWindow`**: グリッド / ツリーの各項目に M / A / D / R / ? / 競合の 1 文字バッジ (フォルダは集約結果)。色は `themeColor::*` の既存トークンを意味で割り当て
   (例: M = Warning、A = Success、D / 競合 = Error、? = AccentSoft、R = Warning)。新トークンを足す場合は `ImGuiTheme.h` の 5 箇条 (彩度 0.35–0.60 / 明度 0.65–0.85、Accent は選択専用) に従う。
   `.meta` の行は本体に束ねられて表示されない (現行の AssetBrowser が `.meta` を隠しているならそのまま)。
3. **保存ヒント**: `EditorApp::SaveCurrentScene`、`AssetOps` の書き出し (import / 作成 / リネーム / 削除)、Animation / Controller / Mixer / ProjectSettings の Save の直後に `CollabClient::HintChanged(paths)`。
   Collab が Unavailable のときは no-op (呼び出し側に分岐を書かせない)。
4. **セルフテスト**: フォルダ集約 (子 M → 親 M / 子 ? のみ → 親 ? / 子 M + ? → 親 M / 子 競合 → 親 競合 / 空フォルダ → なし) — sub-02 の (d) に無いケースを足す。

## やらないこと (このサブでは)

- バッジからの右クリック操作 (stage 等)。v1 は表示のみ。

## 触る場所 (planner の見立て)

- 変更: `src\Editor\SourceControl\SourceControlState.*`、`src\Editor\Windows\AssetBrowserWindow.cpp`、`src\Engine\Renderer\ImGuiTheme.{h,cpp}` (必要なときだけ)、`src\Editor\EditorApp.cpp`、`src\Editor\AssetOps.cpp`、各 Save 窓 (1 行ずつ)、`SourceControlSelfTest.cpp`。

## 受け入れ条件 (このサブ)

1. `--selftest` 緑 (集約 5 ケース)。
2. 実機 (fixture): 変更したテクスチャに M、新規に ?、親フォルダに集約バッジ。シーンを保存した瞬間 (< 300 ms) に Changes タブとバッジが更新される。
3. `tools\shot_verify.bat` 無変更緑 (AssetBrowser は撮影対象外だが `ImGuiTheme` を触ったときの念のため)。
4. `check_rules.ps1` / `replay_verify.bat` 緑。

## 検証コマンド

```
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視)
tools\shot_verify.bat
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

## フィードバック履歴
