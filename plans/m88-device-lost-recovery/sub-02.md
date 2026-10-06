# sub-02: 復旧の骨格と旧デバイス参照数ゲート

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.1 の Recovering、4.1.3 の再試行と打ち切り、4.1.4 のゲートを作り、
デバイス・スワップチェーン・ImGui・ShaderManager・ForwardPath/DeferredPath (配下のパス群を含む) を作り直せるようにする。

**最初にやること (荷重のかかる未知 R1)**: 起動直後に「全 GPU 所有者を Shutdown → 旧デバイス参照数を測る」を
HW とローカルの `--warp` で行い、期待値 (検査用 1 のみ、になるか) を実測して実装メモに記録する。
デバッグレイヤ有無 (Debug/Release) でも測る。安定しない・説明できない値なら、実装を進めずに SELF_EVAL の「不安・質問」で planner へ戻す。

1. EngineLoop: Lost → (次フレーム頭のセーフポイント) → Recovering。手順は spec 4.1.1 の 1〜8。
   既存の Shutdown/Init を再利用し、起動順・終了順と同じ順にする。手順は 1 つの関数 (またはローカルラムダ) にまとめ、起動時の Init 列と二重管理にならないよう、可能なら起動時と同じコードを通す。
2. GraphicsDevice: 「消失前と同じ種類 (HW/WARP) で作り直す」入口 (WARP へ自動フォールバックしない)。旧デバイス参照数の測定 (整数を返すだけ。D3D 型を外へ出さない)。
3. 再試行 `kDeviceRecreateAttempts = 10` × `kDeviceRecreateRetryMs = 500`、連続消失 `kDeviceLostMaxInWindow = 3` / `kDeviceLostWindowSec = 60`。打ち切りは Fatal (sub-01 の経路)。
4. ゲート不合格 → Fatal。ログに残参照数、Debug でデバッグレイヤが有効なら ReportLiveDeviceObjects。
5. テスト専用の注入: 旧デバイスの子オブジェクトを 1 つ握らせる手段 (SelfTest からのみ使う) で、ゲートが不合格 → Fatal になることを確かめる。
6. `--simulate-device-lost-fatal` が無ければ復旧を試みるよう sub-01 の分岐を差し替える。
6b. Lost を検出したら、次フレーム頭の Recovering までの間は描画 (OnRenderViews 以降・ImGui 描画・Present) を飛ばす (spec 8. 2026-10-07 の読み替え)。疑似消失は one-shot にする (現状の `frameIndex >= 指定` のままだと復旧後も毎フレーム発火する)。
7. 疑似消失を同一実行で複数回起こせるよう CLI を拡張する (例: カンマ区切りのフレーム列)。spec 受け入れ 7 用。

この時点ではアセット・UI/VFX・粒子などが未対応なので、通常のシーンではゲート不合格 → Fatal になるのが**正しい**。
ゲートの残参照数が sub-03/04 で減っていくことが進捗の観測値になる。

## やらないこと (このサブでは)
- アセット (sub-03)、UI/VFX/粒子/RT/Probe/compute (sub-04)、エディタの RT (sub-05)。

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp:120-509` (Init 列)、`:2952-2967` (Shutdown 列)、セーフポイント (ホットリロード適用の位置)
- `C:\HAL\MyEngin\src\Engine\Renderer\Device\GraphicsDevice.h/.cpp`、`SwapChain.h/.cpp`
- `C:\HAL\MyEngin\src\Engine\Renderer\ImGui\ImGuiRenderer.cpp` (フォントアトラス含む)
- `C:\HAL\MyEngin\src\Engine\Renderer\Shader\ShaderManager.h/.cpp` (コンパイル済みバイトコードを保持しているなら再作成はバイトコードから。所要時間を実測)
- `C:\HAL\MyEngin\src\Engine\Renderer\Pipeline\ForwardPath.*`、`DeferredPath.*`、`src\Engine\Renderer\Passes\*`、`PostFx\*`、`Device\GpuTimer.*`、`Device\RenderTexture.*`、`Device\VolumeTexture.*`
- `Shutdown` が無い・Init を 2 回呼べない型は、Init 前の状態へ戻す手段を足す (既存の型の責務の範囲で)

## 受け入れ条件 (このサブ)
1. 実測: Shutdown 後の旧デバイス参照数 (HW / WARP × Debug / Release) が実装メモにある。
2. (spec 12) 注入でゲート不合格 → Fatal を SelfTest で確認。
3. (spec 5 の途中観測) 空に近いシーン (アセット・UI・粒子なし、coder が用意する最小構成。テスト専用シーンを増やすならテスト資産の置き場に従う) で `Runtime.exe --simulate-device-lost 30` が復旧し、ゲート合格、0 終了。代表シーンでは Fatal + 残参照数のログ (期待どおり)。
4. (spec 7) 最小構成で 2 回の疑似消失が 2 回とも復旧、3 回目 (60 秒以内) で Fatal。
5. 復旧の所要時間 (シェーダ含む) を実装メモに記録。
6. (spec 14) selftest (Debug/Release)、check_rules、replay_verify。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Runtime.exe <最小構成> --simulate-device-lost 30`、`--warp` 付きでも
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要点)
- R1 実測 (全所有者を手放した後の旧デバイス外部参照数、`GraphicsDevice::CountExternalDeviceRefs`): アセット無し構成で HW / WARP × Debug / Release の 4 通りとも 0 (期待値 `kExpectedExternalDeviceRefs = 0`)。取りこぼしが 1 つあると参照数に出ることも確認 (SelfTest で子バッファ 1 個を握らせると 1)。
- R2 実測 (復旧所要): Release 255〜295 ms (うちデバイス 130〜180 ms、シェーダ 40〜50 ms)。Debug 約 2.2 s (うち UI フォント 1.9 s)。シェーダを `--no-shader-cache` で毎回コンパイルすると 6.2 s (空シーンのプログラム数)。
- 空シーンの最小構成の作り方: 空のプロジェクトを作る (`<dir>\assets\scenes\empty.scene.json` に `assets\scenes\scene_b.scene.json` のコピー) → `Runtime.exe --project <dir> --scene <dir>\assets\scenes\empty.scene.json --frames 90 --no-audio --simulate-device-lost 30 --simulate-device-lost-drop-assets`。Runtime は起動時にデモ用のメッシュ (jdemo_wheel) とテクスチャ (vdemo_*) を無条件で登録するので、sub-03 までは `--simulate-device-lost-drop-assets` (検証専用) で捨てないと残参照 6 でゲート不合格になる。
- 疑似消失の複数指定: `--simulate-device-lost 30,60,90` (昇順にソートされ、各フレームで 1 回ずつ発火)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。前倒し (UI/VFX/粒子/RenderSystem/組込みメッシュ) と手順 1 の省略を承認し、spec 8. に記録。drop-assets フラグは sub-03 で削除する。新しく出た rule 7 の警告 (ShaderManager の programs_ 走査) は nit として申し送り。
