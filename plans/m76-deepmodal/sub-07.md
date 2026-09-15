# sub-07 (M76g): Editor — 面打ちプレビュー / WAV 書き出し / PhysMat 欄 / カタログ / 文字列

- 依存: sub-06
- 状態: 未着手
- 往復: 0

## やること
spec §4.3。

- `InspectorWindow.cpp` の `ModalSound` 節末尾: 状態 (Missing / Baking / Ready / Failed / NoModel + `backend->Name()`) と有効 cell 数、
  **6 面ボタン** (`+X −X +Y −Y +Z −Z`: 接触点 = ローカル AABB 面中心をワールドへ、力 = 内向き法線 × スライダ [N·s] 0.1..20) → `MakeModalShotPlay` (**sub-06 と同じ関数、2 本目を書かない**) → `RegisterClip(HashStr("modal://preview"))` → `PlayDesc { bus = kBusUi, priority = 255 }` (SoundGenWindow.cpp:181-202 と同型)。Baking 中はボタン disabled。
  **Export WAV**: 直近のプレビュー clip を `WriteWavToFile` (SynthCore.h:46) で `assets\audio\modal_<entity>_<face>.wav` へ (SoundGenWindow::Save 204-234 の重複名 `" (N)"` 規則を踏襲)。
- PhysMat インスペクタ (InspectorWindow.cpp:2058-2077 付近) に E / ν / α / β の 4 行 (DragFloat、Sanitize 範囲)。ν の行にはツールチップ「現行ランタイムは未使用 (FEM / 参照材質メタデータ用)」を en/ja で付ける (ユーザー判断 spec §2 #4)。
- `EditorComponentCatalog.cpp` に `ModalSound` (Audio 分類)。
- `LocalizationTable.inl` に en/ja (節見出し / 状態 5 種 / 6 面 / 力積 / Export WAV / PhysMat 4 行 + ν のツールチップ)。`Tr()` を printf の唯一の引数にしない。

## やらないこと (このサブでは)
新しい配色ルール (`themeColor::*` の状態 → 色) は足さない (状態はテキスト表示)。ランタイムのロジック変更。

## 触る場所 (planner の見立て)
- `src\Editor\Windows\InspectorWindow.cpp` (ModalSound 節 / PhysMat 2058-2077)
- `src\Editor\EditorComponentCatalog.cpp`
- `src\Engine\Core\LocalizationTable.inl`
- 参考: `src\Editor\Windows\SoundGenWindow.cpp:19` (プレビュー id の置き方) / 181-234

## 受け入れ条件 (このサブ)
spec §5 の 18。
1. `--selftest` 全緑 (LocalizationSelfTest が en/ja と `###` の一意性を固定)、`check_rules.ps1` 規則 10 緑
2. 手動 (coder が実機で。手順を SELF_EVAL に): `--modal-demo` を Editor で開き箱を選択 → 6 面で音が変わる / スライダで音量が変わる / Baking 中は disabled / Export WAV が出て `.wav` を Asset Browser から再生できる
3. PhysMat の 4 行を編集 → 保存 → JSON に値が入る → 再読込で戻る

## 検証コマンド
- Debug ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → `pwsh -File tools\check_rules.ps1`
- 手動: `bin\x64\Debug\Editor.exe --modal-demo`

## 実装メモ (coder が追記)

## フィードバック履歴
