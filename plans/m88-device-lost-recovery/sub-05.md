# sub-05: エディタ側の復旧・メニュー・ADR-026・TDR 手順

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## やること
1. IEngineApp に「GPU を手放せ」「作り直せ」の 2 通知 (sub-02 で仮想関数だけ置いていればその実装) を EditorApp で実装:
   - `SceneViewWindow` の `rt_` / `previewRt_`、`GameViewWindow` の `rt_`
   - `AssetPreviewCache` (`rt_` とキャッシュ済みサムネイル。サムネイルは再生成待ちに戻す)
   - ImGui に渡している ImTextureID (アイコン・プレビュー・ビュー画像) を保持している箇所は、復旧後に取り直す
   - エディタ専用パス (`EditorLinePass` / `PickingPass` / `GhostMeshPass` / `NavFillPass` 等) が EngineLoop 側の復旧で未対応ならここで
1b. `PickingPass::Shutdown` で解放されていない `blendOff_` を補完する。`ImGuiRenderer::ReleaseDevice / RecreateDevice` (sub-02 で実装したが未実走。ImGui 1.92 のフォントアトラス再作成が前提) をエディタで実走させ、フォントとアイコンが描かれることを確認する。sub-02 時点では、エディタの旧デバイス外部参照は 899。
2. エディタのメニュー (既存のデバッグ系メニュー) に「デバイス消失を偽装」(Tr 日英、`###` 識別子を揃える)。
3. ADR-026 `C:\HAL\MyEngin\docs\adr\ADR-026-device-lost-recovery.md`: プロセス内復旧を選んだ理由 (と自動再起動案を採らなかった理由)、Shutdown→Init 再利用、参照数ゲート、ABI を変えない理由、GPU 上の中身を復元しない理由、WARP へ落とさない理由。
4. `C:\HAL\MyEngin\docs\test_checklists.md` に手動確認: (a) メニューの偽装で復旧、(b) `dxcap -forcetdr` (管理者 PowerShell、実機 GPU) での本物の TDR 復旧、(c) 失敗時の退避ファイルの確認。
5. `C:\HAL\MyEngin\engine_spec.md` の該当章 (描画デバイス / 障害時の挙動) があれば 1 段落追記。

## やらないこと (このサブでは)
- ProjectManager のミニループ (spec 対象外)。

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\src\Editor\App\EditorApp.h/.cpp`
- `C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewWindow.h:113,175`、`GameViewWindow.h:22`
- `C:\HAL\MyEngin\src\Editor\Asset\AssetPreviewCache.h:79` / `.cpp`
- ImTextureID の cast 箇所 (grep `ImTextureID`)
- `C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 5 Editor) `Editor.exe <project> --simulate-device-lost 60 --frames N --screenshot` で復旧、ゲート合格、0 終了。HW と `--warp`。
2. (spec 8) 復旧後のエディタ画面のスクショ (Scene ビュー・Game ビュー・アセットブラウザのプレビュー・アイコンが描かれている) を実装メモに添付。ユーザーの目視 1 回 (メニューの偽装 → 選択・ギズモ・Play/Stop) は「未検証」として残してよい。
3. (spec 13) TDR 手順が test_checklists.md にある。
4. (spec 15) ADR-026。
5. (spec 14) selftest (Debug/Release)、check_rules、replay_verify。
6. (sub-04 から移管、spec 10 の残り) `MYE_EXTRA_ARGS="--simulate-device-lost 1"` 付きの `tools\replay_verify.bat` が 17 job すべて一致すること (Editor.exe を使う chain / time-travel / what-if を含む)。
7. EditorApp の `probeSet_` (ReflectionProbeArray) と `reflectionProbes` を OnDeviceLost で手放し、復旧後はシーン読み込み時と同じ扱いで戻す (焼いたものがディスクにあれば読み直す。ディスクへは書かない)。焼いたプローブを持つシーンで、消失ありと無しのスクショが一致すること (履歴無効の条件で)。
8. エディタ側で描画に `meshes.Get` を使っている箇所 (SceneView 等) を `MeshLibrary::GetDrawable` に置き換える (sub-04 で追加した、vb/ib が null のメッシュを読み飛ばす入口)。
9. ADR-026 には、spec 8. の sub-04 の知見 (容量カウンタは Shutdown で 0 に戻す / CPU 側の履歴は消さない) と、既知の差 K1 を含める。

10. (review-1 #1) エディタで「復旧を試みた後に Fatal へ進む」と、終了処理で ImGui の Win32 バックエンドを二重に Shutdown して落ちる。これを直す。ImGuiRenderer が DX11 / Win32 それぞれのバックエンドの生死を別に持ち、Shutdown は生きている側だけを畳む。RecreateDevice が途中で失敗した分岐 (ImGuiRenderer.cpp:117-120) も同じ。他のサブシステムにも同じ二重解放の型があれば直す (RecoverDevice の各段で失敗したときの終了処理を一通り読む)。
11. (review-1 #1、spec 受け入れ 12 の追加分) 検証専用の入口 (CLI でもテスト専用フックでもよい) を足し、RecoverDevice の途中で旧デバイスの子を握らせる。これで実ループのゲート不合格を起こし、Editor.exe `--frames N --simulate-device-lost 30` が Debug / Release とも exit 6、退避ファイルあり、クラッシュも IM_ASSERT ダイアログも出ないことを確かめる。可能なら再作成失敗の分岐も同じ入口で通す。test_checklists.md の (c) に、この経路の自動確認があることを書く。
12. (review-1 #2) ADR-026 の「実体」欄のファイル名を直す。RecycleDevice / DeviceLostLimiter は `src\Engine\Engine\Loop\DeviceRecovery.{h,cpp}`、DeviceFatalInfo は `EngineLoop.h` にある。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Editor.exe ... --simulate-device-lost 60 --screenshot ...` (PowerShell ツールから)
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要点)
- 実装: `EditorApp::OnDeviceLost / OnDeviceRestored` (プローブ束・`probePreview_`・`probeBaker_`・`sceneView_` / `gameView_` / `preview_` の `ReleaseGpu`、束を持っていたときだけ復旧後に BakeAll)。`SceneViewWindow::ReleaseGpu` (rt_・previewRt_・picking_・lines_・ghostMesh_)、`GameViewWindow::ReleaseGpu`、`AssetPreviewCache::ReleaseGpu` (サムネイル全消去 = 要求され次第再生成、rt_、previewRender_)。ImTextureID を持ち越している箇所は無かった (毎フレーム SRV を引き直している)。アイコンはフォントのグリフで、テクスチャを持たない。
- 実走で見つかった不具合 3 件: (1) `ImGui_ImplDX11_Shutdown` が `DestroyPlatformWindows` でメインビューポートの `PlatformUserData` を捨て、復旧直後の `NewFrame` で `ImGui_ImplWin32_GetWindowDpiScale` が null 読み (Editor.exe が復旧後に AV)。`ImGuiRenderer::ReleaseDevice / RecreateDevice` で Win32 バックエンドも Shutdown / Init し直すようにした。(2) `PickingPass::Shutdown` の `blendOff_` 未解放 (1b)。(3) `EditorLinePass::Shutdown` が `vbCapacity_` を 0 に戻さない (ADR-026 の規則どおり)。
- メニュー「デバイス消失を偽装」: View > Rendering の末尾 (Probe 系の下)。`EngineContext::requestSimulatedDeviceLost` を新設し、EngineLoop が検出点で読んで false に戻す。文言は `Menu_SimulateDeviceLost` ("...###Menu_SimulateDeviceLost" を日英で一致)。
- `meshes.Get` → `GetDrawable` (受け入れ 8): 描画に使う `SceneViewGhost` と `AssetPreviewCache` の 2 箇所。残りの Editor の `meshes.Get` (SceneView の AABB フィット / ピック、Inspector、CreateMenu、ModalTools、SelfTest) は CPU の AABB / 頂点を読むだけで vb/ib を触らないので置き換えていない。
- 文書: ADR-026、`docs\test_checklists.md` の M88 節 (偽装 / `dxcap -forcetdr` / 退避ファイル)、`engine_spec.md` §6.13 + ADR 一覧。
- 検証 (すべて今回の最終ツリーで):
  - Debug / Release の MSBuild: エラー 0。`tools\check_rules.ps1` 0 error (警告 50 はいずれも既存の rule 7)。
  - `Editor.exe --selftest` Release exit 0、Debug exit 0 (Debug は 8 分前後かかる)。
  - 受け入れ 1: `Editor.exe --simulate-device-lost 60 --frames 90 --shot-frame 88` (Release、HW と `--warp`、既定デモと `--render-demo`) → すべて `old device external references: 0 (allowed 0)` → `device recovered` (約 4.5 s)、world hash unchanged=1、exit 0。Debug も `--simulate-device-lost 20` で 0 / exit 0 (約 14 s)。
  - 受け入れ 2: `--simulate-device-lost 30` の Editor.exe (Debug、1600x900、`--no-fxaa`)、消失ありと無しの img-diff。画面全体の差は 5561 px (Scene 表示) だけで、diff の熱地図で差があるのは FPS の数字・Console の最終ログ行・ステータスバーの文言だけ。Scene ビューの絵 (グリッド・ギズモ・補助線を含む)・Game ビュー・フォント・Hierarchy / Asset Browser のフォルダアイコン・Asset のモデルサムネイル 2 枚 (BoxTextured / CesiumMan、復旧後に再生成されたことを `glTF loaded` ログで確認) は 0 差。スクショ: `plans\m88-device-lost-recovery\shots\sub05-editor-scene-after-recovery.png` / `sub05-editor-game-after-recovery.png` / `sub05-editor-scene-diff.png`。サムネイルとタブの Game / Scene 切り替えの確認には一時プローブ (環境変数で SetWindowFocus と glb サムネイルの ImGui::Image を出す) を入れ、**撮影後に外した** (最終ツリーに残っていない)。
  - 受け入れ 7: `--render-demo` に反射プローブが 1 個あるので、一時プローブでエディタの BakeAll を frame 5 で要求 → 消失 (frame 30) あり / 無しを撮って img-diff。ゲートは 0 で合格し、復旧後に `[probe] baked 1 reflection probe(s)` が再度出た。diff の熱地図で差があるのは FPS・Console・「Baked ms」・トースト等のテキストだけで、Scene の絵は 0 差 (forward。TAA 等は既定で無効)。一時プローブは撮影後に外した。`sub05-editor-probe-after-recovery.png` / `sub05-editor-probe-diff.png`。
  - メニューの経路: 一時プローブで `ctx.requestSimulatedDeviceLost = true` を frame 7 に立てた Release 実行 → frame 8 に recovery started、ゲート 0、復旧、exit 0 (メニューのクリックそのものは自動化できていない)。
  - 受け入れ 6: `MYE_EXTRA_ARGS="--simulate-device-lost 1"` 付きの `tools\replay_verify.bat` → 17 job すべて PASS (Editor.exe の time-travel / what-if 4 job を含む。1361 s)。素の `replay_verify.bat` は、この変更で (フラグ無しの) 経路は何も変わらないので再走していない (sub-04 のコミット時点で 17 job PASS)。
- メモ: sub-05.md の検証コマンドにある `--maxFrames` は存在しない CLI で、正しくは `--frames N` (未知の引数は黙って無視され、`--screenshot` と組むと Editor.exe が終了しない)。仕様側の誤記。

### round 2 (FIX_REQUEST: REVIEW round 1)
- #1: `ImGuiRenderer` に `win32Bound_` を追加。Init / RecreateDevice で立て、ReleaseDevice と RecreateDevice の DX11 失敗分岐で下ろし、Shutdown は `deviceBound_` / `win32Bound_` が立っている側だけ畳む。検証入口 `--simulate-device-lost-stale` (`EngineConfig::simulateDeviceLostStale`、`GraphicsDevice::HoldChildForTest`) を足し、RecoverDevice が imgui.ReleaseDevice した後・RecycleDevice の前に旧デバイスの子 (定数バッファ 1 個) を握らせる。CLI 解析は EngineCliSelfTest に 1 件追加。他の Shutdown (Vfx / Ui / Particle / Compute / Forward / Deferred / SwapChain / Device) は ComPtr の Reset と容量・ready の初期化だけで二重実行しても害が無いことを読んで確認し、上の実走 (RecoverDevice 後の致命停止で終了処理を全部通す) でも落ちないことを確認した。
- #2: ADR-026 の実体欄を実際のファイルに直した。test_checklists の M88 (c) に `--simulate-device-lost-stale` の項目を追加。
- 検証: Debug / Release MSBuild エラー 0。`Editor.exe --frames 120 --simulate-device-lost 30 --simulate-device-lost-stale` が Debug / Release とも exit 6、退避あり (bin\x64\<構成>\crash\ 配下)、クラッシュ・assert なし。`Editor.exe --selftest` Debug / Release exit 0。check_rules 0 error。`tools\replay_verify.bat` (フラグ無し) 17 job PASS。
- 未実施: 修正前のコードで同じコマンドが落ちることの再現確認 (レビュー報告の根拠コードを信頼した)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。追加 3 件を承認した (requestSimulatedDeviceLost、ImGui の Win32 再 Init、EditorLinePass の容量リセット)。`--maxFrames` の誤記は spec / sub の両方で `--frames N` に直した。素の replay_verify を再走していない件はレビューで回収する。エディタの自動回帰は後回し。ユーザーの目視 3 項目 (メニュー偽装からの操作、本物の TDR、退避ファイルの再読込) と ImGui の ini・ドッキング状態の確認は未検証として残す。
- review-1: 差し戻し (#1 major: 復旧を試みた後に Fatal へ進むと、ImGui の Win32 バックエンドを二重に Shutdown する / #2 minor: ADR-026 のファイル名)。受け入れ 10〜12 を追加した。spec 受け入れ 12 と 4.1.5 も更新した (planner)。
- round 2 (review-1 の修正): VERDICT OK (planner)。win32Bound_ による二重 Shutdown の解消、`--simulate-device-lost-stale` による実ループの Fatal 経路 (Debug / Release で exit 6、退避あり、クラッシュ無し)、ADR のファイル名修正を確認した。修正前に落ちることの再現は未実施で、再作成失敗の分岐 (受け入れ 11 の「可能なら」) は通していない。どちらも nit。
