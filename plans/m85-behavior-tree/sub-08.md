# sub-08: BT 窓 (1) — キャンバス・配置・接続・パラメータ・保存・Asset Browser から開く

- 依存: sub-07
- 状態: 未着手
- 往復: 0

## やること
- 新しい窓 `src\Editor\Windows\AI\BehaviorTreeWindow.{h,cpp}` (spec 4.3)。Window メニュー、レイアウトの一覧 (`EditorApp.cpp:98-118`)、`.bt.json` のダブルクリック (`AssetBrowserWindow.cpp:692-783` に分岐、ミキサーの `openMixerRequest_` / `TakeOpenMixerRequest` の流儀、`EditorApp.cpp:973-975`)。
- **モデル層と描画層を分ける**: 編集中の木 (ノード・位置・接続・パラメータ) と操作 (追加・削除・接続・切断・移動・パラメータ変更・保存) を ImGui に依存しないクラスにし、`BehaviorTreeEditorSelfTest` (Editor 層) で機械検査する。sub-09 の Undo はこのモデル層の操作単位に乗る。
- キャンバス: ImGui DrawList、パン (中ボタン / Space+左)、ズーム (ホイール、0.25〜2.0)。上から下の木、Decorator はノードの箱の上に帯で積む。子の順序は x の左から (表示に番号)。
- パレット: sub-01 のノード記述子の表から自動で作る (種類ごとの分類)。パラメータ欄も記述子から自動生成 (Bool / Int / Float / Vector / BB キーの選択 (型で絞る) / AssetRef / 文字列 / enum)。
- 保存: `BehaviorTreeLibrary` 経由で `.bt.json` (位置を含む)、`scmhint::Changed`、未保存の印、`EditorApp` の未保存確認 (`EditorApp.cpp:340/1834` の Animator の流儀)。保存すると ReloadHub の再読込でなく直接ライブラリへ反映 (spec 4.1.9 のやり直しが走る)。
- 位置を持たないノード (手書きの .bt.json) は読み込み時に自動整列。
- 文字列は en / ja。

## やらないこと (このサブでは)
- Undo / Redo、Blackboard パネル、検査エラー (sub-09)、ライブ表示 (sub-10)

## 触る場所 (planner の見立て)
- 新規: `src\Editor\Windows\AI\BehaviorTreeWindow.*`、`src\Editor\Windows\AI\BehaviorTreeEditModel.*` (名前は任意)、`src\Editor\SelfTest\BehaviorTreeEditorSelfTest.*`
- 変更: `EditorApp.{h,cpp}`、`AssetBrowserWindow.{h,cpp}`、`EditorMain.cpp` (selftest)、`LocalizationTable.inl`、`build\Editor.vcxproj` (+filters)
- 参考: `AnimatorControllerWindow.cpp` (DrawList の描き方だけ。位置を保存しない・Undo 無しの作りは真似しない、spec 2. #16)

## 受け入れ条件 (このサブ)
1. (spec 11 の一部) モデル層: 追加・接続・切断・並べ替え (x の順)・削除 (子ごと / 子を残す) ・パラメータ変更・保存 → 読み直しで位置と順序が戻る — `BehaviorTreeEditorSelfTest`。
2. 画面: デモ用の木を開いた BT 窓の画像 (一時プローブの `--screenshot`。ノードの箱・Decorator の帯・接続線・パラメータ欄が見える) を SELF_EVAL に添える。
3. Asset Browser のダブルクリックと Window メニューの両方で開く (手順の記録)。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0 (ローカライズ)。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、`tools\check_rules.ps1`、一時プローブの `--screenshot` (`screenshot-probe-recipes` のメモ: シーン撮りは Runtime.exe、エディタ窓は Editor.exe)

## 実装メモ (coder が追記)

## フィードバック履歴
