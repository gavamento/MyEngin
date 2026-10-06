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
1. (spec 5 Editor) `Editor.exe <project> --simulate-device-lost 60 --maxFrames ... --screenshot` で復旧、ゲート合格、0 終了。HW と `--warp`。
2. (spec 8) 復旧後のエディタ画面のスクショ (Scene ビュー・Game ビュー・アセットブラウザのプレビュー・アイコンが描かれている) を実装メモに添付。ユーザーの目視 1 回 (メニューの偽装 → 選択・ギズモ・Play/Stop) は「未検証」として残してよい。
3. (spec 13) TDR 手順が test_checklists.md にある。
4. (spec 15) ADR-026。
5. (spec 14) selftest (Debug/Release)、check_rules、replay_verify。
6. (sub-04 から移管、spec 10 の残り) `MYE_EXTRA_ARGS="--simulate-device-lost 1"` 付きの `tools\replay_verify.bat` が 17 job すべて一致すること (Editor.exe を使う chain / time-travel / what-if を含む)。
7. EditorApp の `probeSet_` (ReflectionProbeArray) と `reflectionProbes` を OnDeviceLost で手放し、復旧後はシーン読み込み時と同じ扱いで戻す (焼いたものがディスクにあれば読み直す。ディスクへは書かない)。焼いたプローブを持つシーンで、消失ありと無しのスクショが一致すること (履歴無効の条件で)。
8. エディタ側で描画に `meshes.Get` を使っている箇所 (SceneView 等) を `MeshLibrary::GetDrawable` に置き換える (sub-04 で追加した、vb/ib が null のメッシュを読み飛ばす入口)。
9. ADR-026 には、spec 8. の sub-04 の知見 (容量カウンタは Shutdown で 0 に戻す / CPU 側の履歴は消さない) と、既知の差 K1 を含める。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Editor.exe ... --simulate-device-lost 60 --screenshot ...` (PowerShell ツールから)
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
