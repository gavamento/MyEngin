# sub-01: 検出・疑似消失・安全停止 (①)

- 依存: なし
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.1 の検出と Lost/Fatal 状態、4.1.2 の疑似消失、4.1.5 の Fatal (①) を実装する。
このサブではまだ Recovering を作らない: **Lost になったら必ず Fatal へ進む**。
(sub-02 で Lost → Recovering に差し替える。状態機械の型・列挙はこのサブで Recovering を含めて定義してよいが、未使用の分岐は置かない)

1. `SwapChain::Present` が成否 (消失したか、HRESULT) を返すようにし、ログ 1 行のみの現挙動をやめる。
2. フレーム末で `GetDeviceRemovedReason()` を見る (GraphicsDevice に「消失しているか」を問う関数を足す。生の D3D 型を上へ出さない)。
3. 疑似消失: CLI `--simulate-device-lost <frame>` / `--simulate-device-lost-fatal` を EngineConfig 経由で受ける (CLI 解析は既存の Editor/Runtime の引数処理に合わせる)。指定フレームで Present 判定の直前に消失扱いにする。不正値はエラーログで無視。
4. Lost になったフレームの残りで D3D を呼ばない (ImGui 描画・Present を飛ばす)。tick は止めない。
5. Fatal: IEngineApp に致命通知の仮想関数を足す (既定は何もしない)。EngineLoop は通知後、ループを抜けて終了コード `kExitCodeDeviceLost` を返す。Shutdown 列は通すが、消失したデバイスへの解放で落ちないこと。
6. エディタ (`EditorApp`): 致命通知で、Play 中なら Play 前の編集状態、そうでなければ現在の編集状態を、元ファイルを上書きせずに退避ファイルへ保存 (保存先規約は既存のプロジェクト配下ディレクトリに合わせて決め、実装メモに記録)。理由と退避先の絶対パスを `MessageBoxW` で表示 (非対話実行ではログのみ)。文言は `Tr()` 日英。
7. Runtime: 致命通知で理由を表示 (非対話実行ではログのみ)、退避保存なし。
8. SelfTest: エディタの退避保存 → 再読込で編集状態が一致すること (Play 前状態の選択を含む)。

## やらないこと (このサブでは)
- 復旧 (デバイスの作り直し)。エディタのメニュー項目 (sub-05)。
- ABI・保存形式の変更。

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\src\Engine\Renderer\Device\SwapChain.cpp:185-192` / `SwapChain.h` — Present の戻り値
- `C:\HAL\MyEngin\src\Engine\Renderer\Device\GraphicsDevice.h/.cpp` — 消失判定 (removed reason を整数で返す)
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.h:39` (EngineConfig)、`:468` (IEngineApp)
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp` — L2918 付近の Present、L2923 付近のフレーム末、ループ脱出と終了コード、L2952-2967 の Shutdown 列
- `C:\HAL\MyEngin\src\Editor\App\EditorApp.h/.cpp` — 致命通知の実装、`SaveCurrentScene` (`EditorApp.h:97`) 周辺の保存経路の再利用、Play 前状態の取得
- `C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp` / `src\Runtime\` の引数解析
- `C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl` — 文言
- 非対話判定: 既存の `--selftest` / `maxFrames` の扱いを確認して合わせる

## 受け入れ条件 (このサブ)
1. (spec 1) 消失の検出とログ 1 回。
2. (spec 2) CLI の 2 フラグが Editor.exe / Runtime.exe で効く。不正値は無視 + エラーログ。
3. (spec 3) `Runtime.exe <代表シーン> --simulate-device-lost 30 --simulate-device-lost-fatal` が `kExitCodeDeviceLost` で終了。Debug ビルドで 30 フレーム以降の D3D デバッグレイヤのエラーが無い。`--simulate-device-lost-fatal` なしでも (このサブでは) 同じく Fatal で終わる。
4. (spec 4) SelfTest で退避保存 → 再読込の一致 (Play 中 → Play 前状態)。Editor.exe で `--simulate-device-lost 60` を付けて対話起動し、メッセージと退避ファイルを 1 回確認 (スクショまたはログで代替した場合はその旨を書く)。
5. (spec 14) `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`。

## 検証コマンド
- MSBuild で Debug|x64 / Release|x64
- `bin\x64\Debug\Runtime.exe ... --simulate-device-lost 30 --simulate-device-lost-fatal` (PowerShell ツールから実行し `$LASTEXITCODE` を記録)
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- `tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - SwapChain.h/.cpp: Present が HRESULT を返す。静的 IsDeviceLostResult を追加 (ログ 1 行のみの旧挙動を廃止)
  - GraphicsDevice.h/.cpp: DeviceRemovedReason() (long。生の D3D 型を出さない)
  - EngineLoop.h: EngineConfig に simulateDeviceLostFrame / simulateDeviceLostFatal、定数 kExitCodeDeviceLost = 6、構造体 DeviceFatalInfo、IEngineApp::OnDeviceFatal (既定は何もしない)
  - EngineLoop.cpp: Present の戻り値 + フレーム末 GetDeviceRemovedReason + 疑似消失 (frameIndex >= 指定) で検出。1 回ログ、検出フレームは PumpDebugMessages (実消失時のみ) を飛ばし OnDeviceFatal → exitCode = 6 → ループを抜けて既存 Shutdown 列
  - EngineCli.cpp: --simulate-device-lost <frame> (不正値は stderr にエラーを出して無視、起動は止めない) / --simulate-device-lost-fatal。Editor / Runtime 共通表なので両方で効く
  - Engine/Loop/DeviceFatal.h/.cpp (新規): 通知文の組み立て (Tr 日英、HRESULT は 16 進、書式文字列に Tr を使わない) と MessageBoxW (interactive のときだけ) + ログ
  - LocalizationTable.inl: DevLost_* の 7 文言 (日英)
  - RuntimeMain.cpp: OnDeviceFatal で ReportDeviceFatal (退避保存なし)
  - Editor/App/DeviceLostRescue.h/.cpp (新規): SaveRescueScene / RescueSceneName / RescueTimestamp
  - PlayModeController.h: PrePlaySnapshot() (Play 中のみ Play 開始前の JSON を返す)
  - EditorApp.h/.cpp: OnDeviceFatal。Play 中は PrePlaySnapshot、そうでなければ SaveToJson(*ctx.scene) を退避保存し、パスを含めて ReportDeviceFatal
  - SelfTest: Editor/SelfTest/DeviceLostRescueSelfTest.h/.cpp (新規、EditorMain の --selftest 末尾に登録) + EngineCliSelfTest に 4 件
  - 退避先の規約 (決定): <ctx.projectRoot、レガシー起動は exe ディレクトリ>\crash\device_lost_<YYYYMMDD-HHMMSS>\<シーン名>.scene.json。既存のクラッシュバンドル / desync バンドル (<root>\crash\...) と同じ親に置いた。spec 例の Saved/DeviceLost は既存規約に無いので使わない
  - 終了コード 6 = 既存 (1/2/4/5) と衝突しない値
仕様との差分:
  - [追加] DeviceFatalInfo.interactive: 非対話判定を EngineLoop が「maxFrames>0 / screenshot / replay record・verify / timeTravel・whatIf プローブ」で決めて渡す (既存の windowModeLive / batchRun と同じ集合から netRole を除いたもの)。Editor の automation 変数は main 内ローカルで Engine から見えないため
  - [追加] 退避保存は SceneSerializer::SaveToFile ではなく SaveToJson + WriteFileReplacing を直接使う (SaveToFile は scene.SetSourcePath を書き換えるため)。形式は通常保存と同一
  - [未実装] --simulate-device-lost-fatal は解析して config に入るだけで、このサブでは効果なし (Lost は常に Fatal。sub-02 で Recovering と分岐する)
  - [逸脱?] 「Lost になったフレームの残りで D3D を呼ばない」: 検出点が Present 直後 / フレーム末の 1 か所のため、実消失がフレーム途中で起きても ImGui 描画や Present は呼んでしまう (呼び出しは失敗を返すだけ)。検出後に呼ぶのは OnDeviceFatal と Shutdown のみ。疑似消失は Present まで実行してから消失扱いにしている
  - [追加] ミニシーン編集中 (actorEdit_) は本シーンだけを退避し、編集中アセットは退避しない (警告ログのみ)。形式が別物のため
  - 「Play 前状態の選択を含む」: 選択はシーン文書に入らない (selection_ はエディタ内部状態) ので退避ファイルにも入らない。SelfTest は Play 前のシーン文書の一致を検証した
  - 不正値のエラーは MYE_LOG ではなく stderr (CLI 解析時はログ未初期化。--particle-backend 等の既存フラグと同じ流儀)
検証:
  - MSBuild Debug|x64 / Release|x64 (MyEngine.sln) → 成功 (tools\gen_project_files.ps1 は pwsh で実行。Windows PowerShell 5.1 だと構文エラーで走らない)
  - Debug Runtime.exe --no-audio --frames 120 --simulate-device-lost 30 --simulate-device-lost-fatal → exit 6、ログに lost 1 回 + fatal、30 フレームで終了、[d3d] エラー 0 件 (デバッグレイヤ on)。Release でも exit 6
  - 同 --simulate-device-lost-fatal なし (--warp、30) → 同じく exit 6。abc / -3 → stderr に invalid、exit 0 で 60 フレーム完走
  - Debug Editor.exe --frames 150 --warp --simulate-device-lost 60 (通常 / --autoplay) → exit 6、crash\device_lost_*\main.scene.json に退避 (Play 中は sim が進んだ状態でも保存サイズ同一 = Play 前)
  - 対話起動 Editor.exe --scene ... --simulate-device-lost 60 (frames 指定なし) → プロセスは 25 秒後も生存し、UI Automation でダイアログ「Graphics device lost」の本文 (理由 + 退避パス) を確認。その後 Stop-Process で閉じた (スクショ未取得、Automation のテキストで代替)
  - Debug / Release Editor.exe --selftest → exit 0、FAIL 行なし (DeviceLostRescue 14 チェックと CLI 4 件含む)
  - tools\check_rules.ps1 → 0 error / 41 warning (すべて既存の rule 7。今回触ったファイルは無し)
  - tools\replay_verify.bat → exit 0、17 jobs passed
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜4・14 を満たす。差分は上記 (検出が 2 点に限られる点と選択の扱いが要確認)
  正しさ: 4 — 疑似消失は実走で確認。実際の DEVICE_REMOVED (本物の TDR) は未検証で、Present / GetDeviceRemovedReason の分岐は疑似では通らない (hr 経路は未実走)
  コード品質: 4 — 既存の層規約に沿う。Runtime / Editor の理由表示は DeviceFatal に一本化
  テスト: 4 — 退避の保存 → 再読込 / Play 前 / 元ファイル不変 / 書けない場所と CLI を自動化。EditorApp::OnDeviceFatal 自体と MessageBox は手動確認のみ
不安・質問:
  - 検出がフレーム末のみで、フレーム途中の実消失で ImGui/Present が失敗呼び出しされる点は spec 4.1.1 の「残りで D3D を呼ばない」と厳密には異なる。sub-02 で Lost を持つときに「Lost 後のフレームでは OnRenderViews 以降を飛ばす」を入れるなら十分か判断してほしい
  - 選択 (selection_) を退避ファイルへ含める要件が U2 の意図なら、シーン文書外の保存形式が要る (今回は含めていない)
触ったファイル:
  src/Engine/Renderer/Device/SwapChain.h, src/Engine/Renderer/Device/SwapChain.cpp, src/Engine/Renderer/Device/GraphicsDevice.h, src/Engine/Renderer/Device/GraphicsDevice.cpp, src/Engine/Engine/Loop/EngineLoop.h, src/Engine/Engine/Loop/EngineLoop.cpp, src/Engine/Engine/Loop/DeviceFatal.h, src/Engine/Engine/Loop/DeviceFatal.cpp, src/Engine/Engine/App/EngineCli.cpp, src/Engine/Engine/App/EngineCliSelfTest.cpp, src/Engine/Core/Localization/LocalizationTable.inl, src/Runtime/RuntimeMain.cpp, src/Editor/App/DeviceLostRescue.h, src/Editor/App/DeviceLostRescue.cpp, src/Editor/App/EditorApp.h, src/Editor/App/EditorApp.cpp, src/Editor/App/EditorMain.cpp, src/Editor/Scene/PlayModeController.h, src/Editor/SelfTest/DeviceLostRescueSelfTest.h, src/Editor/SelfTest/DeviceLostRescueSelfTest.cpp
  (生成物: build/Editor.vcxproj, build/Editor.vcxproj.filters, build/Engine.vcxproj, build/Engine.vcxproj.filters は gen_project_files.ps1 の出力。新規ファイルを載せるためコミットに要る)
申し送り:
  - sub-02 は EngineLoop.cpp の「デバイス消失の検出」ブロックを Lost → Recovering に差し替える (現在は OnDeviceFatal → exitCode 6 → running=false で終了)。simulateDeviceLostFatal / 1 回だけ発火させる one-shot フラグはそこで追加 (今は検出で必ず終了するので不要)
  - 検証で bin\x64\Debug\crash\device_lost_* が 3 つできている (bin は追跡外)
  - gen_project_files.ps1 は pwsh で回すこと

## フィードバック履歴
- round 1: VERDICT OK (planner)。逸脱 2 件 (消失フレーム内の D3D 呼び出し、選択の扱い) は仕様側の書き方の問題として spec 8. で読み替え。退避先 `crash\device_lost_*` と終了コード 6 を spec に確定。本物の DEVICE_REMOVED 経路は未実走 (spec 受け入れ 13 の手動 TDR で回収)。
