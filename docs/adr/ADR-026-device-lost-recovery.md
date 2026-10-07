# ADR-026: GPU デバイス消失 (DEVICE_REMOVED / RESET) をプロセス内で復旧する

- 状態: **確定** (2026-10-07、M88)
- 出所: 依頼「A5の復旧まで実装」。計画は `plans\m88-device-lost-recovery\spec.md`。
- 実体: `src\Engine\Engine\Loop\EngineLoop.cpp` の `RecoverDevice` (復旧手順) と `DeviceLostLimiter` / `DeviceFatalInfo`、
  `src\Engine\Renderer\Device\GraphicsDevice.{h,cpp}` の `RecycleDevice` と旧デバイス参照数の測定、
  `src\Engine\Renderer\Device\GpuResources.{h,cpp}` の `RenderResources::ReleaseGpu / RecreateGpu`、
  エディタは `src\Editor\App\EditorApp.cpp` の `OnDeviceLost / OnDeviceRestored / OnDeviceFatal`。
  検証は `DeviceRecoverySelfTest` と `--simulate-device-lost`。
- 番号: ABI v27 = 158 スロット、`kSimSnapshotVersion` 39、TypeId の追加なし (いずれも変更していない)。

## 背景

GPU ドライバのリセット (TDR)・ドライバ更新・外部 GPU の取り外しで D3D11 デバイスが消えると、以前は `SwapChain::Present` が
ログを 1 行出すだけで、消えたデバイスへ描画を投げ続けた (画面は黒または固まり、エディタの未保存編集は失われた)。
AGENTS.md §3.4 の「一部の失敗で正常な機能まで使用不能にしない」「再初期化・再生成を行えるようにする」に合わない。

## 決定 1: 自動再起動ではなく、プロセス内で描画系だけを作り直す

- 検出点は `Present` の HRESULT とフレーム末の `GetDeviceRemovedReason` の 2 つだけ。検出したフレームの次のフレーム頭
  (ホットリロードと同じセーフポイント) で、同期的に復旧する。非同期化しない (決定性とデバッグ性のため)。
- 復旧は描画専用。ECS・RNG・物理・スクリプト・入力・replay には触れない。復旧中も tick は進む (`kMaxTicksPerFrame` で追い付きは打ち切られる)。
  復旧の前後で `HashWorld` が一致することをログに出す。
- 却下: 「退避保存 → 同じシーンで自動再起動」。UE / Unity のエディタはこちらに近く、1 サブで済む。ただし依頼が「復旧まで」であること、
  カメラ・選択・Undo・Play 状態などエディタの文脈を失うこと、Runtime では再起動の手段が無いことから、プロセス内復旧を選んだ。
  プロセス内復旧が途中で破綻しても、決定 3 のゲートと決定 5 の致命停止が下限を保証する。
- 却下: 復旧のたびに WARP へ逃げる。決定 6 を参照。

## 決定 2: 既存の Shutdown → Init を再利用する (サブシステムごとの復旧 API を作らない)

GPU を持つ所有者は `EngineLoop::Run` にローカル変数としてまとまっていて、`Shutdown` と `Init` が対になっている。復旧は
「終了順に手放す → デバイスを作り直す → 起動順に作る」の既存の列をそのまま使う。例外は次の 3 種類だけ。

- ID を保ったまま中身だけ入れ替えるもの (メッシュ / テクスチャ / マテリアル / compute のハンドル): `ReleaseGpu` と `RecreateGpu` の組。
  CPU 側のデータ (`positions` / `indices`、テクスチャの再生成レシピ) は保持し、AssetID と `Get()` のポインタは復旧をまたいで変わらない。
- テクスチャの再生成レシピ: ファイル (現ディスクから読み直す) / エンコード済みバイト列 (コピー保持) / RGBA8 (コピー保持。単色もこれ)。
  読み込み中・レシピ無し・作り直し失敗は、新しい White を共有するプレースホルダにして続行する (致命にしない)。
- 履歴 (TAA / フロクセル / RT の時間フィルタ / GPU 粒子の現在状態) は GPU 上にしか無いので捨てる。復元しない。作り直し後は初期状態から数フレームで収束する。
  復元するには GPU バッファの読み戻しと保存が要り、復旧の目的 (描画を再開する) に対して重すぎる。

**Shutdown に入れる / 入れないものの規則** (sub-04 で実シーンから見つかった 2 件):

- 遅延で伸びる GPU バッファの容量カウンタ (`UIRenderer` / `VfxRenderer` の `vbCapacity_`、`EditorLinePass` の `vbCapacity_`) は、
  Shutdown で 0 に戻す。戻さないと再 Init 後に「容量は足りている」と判断してバッファを作り直さず、null の `vb_` を Map して d3d11 内で落ちる。
- CPU 側の履歴 (`VfxRenderer` の `trails_` など) は Shutdown で消さない。GPU を手放すことと、描画に使う CPU の状態を消すことは別である。
  消すと復旧の前後で絵が変わる (fog デモのトレイルが不一致になった)。
- ImGui: `ImGui_ImplDX11_Shutdown` は `DestroyPlatformWindows` でメインビューポートの Win32 側データ (`PlatformUserData`) も破棄する。
  DX11 バックエンドだけを畳むと、次のフレームで `ImGui_ImplWin32_GetWindowDpiScale` が null を読んで落ちる。
  `ImGuiRenderer::ReleaseDevice / RecreateDevice` は Win32 バックエンドも一緒に Shutdown / Init し直す。
  ImGui のコンテキスト (ドッキング配置・スタイル) は残り、フォントアトラスは 1.92 の動的テクスチャとして次の `NewFrame` で作り直される。

## 決定 3: 旧デバイスの外部参照数をゲートにする (期待値 0)

D3D11 のデバイス子オブジェクトは親デバイスへの参照を持つので、古いリソースが 1 つでも残ると旧デバイスは解放されない。
旧デバイスを解放する前に検査用に 1 参照だけ持ち、他の全参照が手放された後の参照数を `GraphicsDevice` の中で測り、整数で返す
(生の D3D 型を Renderer の外へ出さない)。期待値は **0** (`kExpectedExternalDeviceRefs`)。Debug / Release × HW / WARP の 4 通りで実測して確定した。

- 0 でなければ、どこかが旧デバイスの子を握っている。新デバイスで描くと未定義動作になるので、復旧せず致命停止へ落とす (残参照数をログに出す)。
- この性質により、復旧を段階的に実装している間も「未対応の所有者が残る → ゲート不合格 → 致命停止」となり、壊れた描画で続行しない。
  取りこぼしは「描画が壊れる」ではなく「終了と理由の表示」で表に出る。
- Editor.exe で最初に測った残参照は 899 (sub-02 時点)、sub-04 で 23 (プローブ束と ImTextureID の持ち主)、sub-05 で 0 になった。
  エディタの所有者 (Scene / Game ビューの RT、`AssetPreviewCache`、エディタ専用パス、プローブ束) は `IEngineApp::OnDeviceLost` で手放す。
- テスト専用に子オブジェクトを意図的に握らせるとゲートが不合格になり、旧デバイスが保たれることを `DeviceRecoverySelfTest` で確認している。

## 決定 4: ABI は変えない

GameLogic DLL / C# には生のデバイスを渡しておらず、compute のハンドルは `ComputeAbiRunner` が `buf / srv / uav` を所有している。
runner が同じハンドル・同じ desc でゼロ内容のまま作り直すので、DLL 側からは何も変わらない。
消失通知 (`OnDeviceRestored`) を ABI に足すとスロット数とバージョンを上げる (v28) ことになる。要望が出たら足す (後回し)。
compute バッファを sim が読み戻す経路は ABI に無い (Create / Release / Set / Dispatch のみ) ので、中身が消えても決定性は壊れない。

## 決定 5: 復旧できないときは致命停止し、編集内容を退避する

- 打ち切り: 新デバイスの作成は最大 10 回 × 500 ms (`kDeviceRecreateAttempts` / `kDeviceRecreateRetryMs`)。直近 60 秒に 3 回目の消失
  (`kDeviceLostWindowSec` / `kDeviceLostMaxInWindow`) は復旧を試みない (復旧直後に毎回落ちるドライバやシェーダの無限ループを防ぐ)。
- 致命停止 (`OnDeviceFatal`): 以降 D3D を呼ばない。エディタは開いているシーンの編集状態 (Play 中なら Play 開始前の状態) を、
  元ファイルを上書きせずに `<project>\crash\device_lost_<日時>\<シーン名>.scene.json` へ保存し、理由と退避先をメッセージボックスで出して
  `kExitCodeDeviceLost` (6) で終了する。非対話の実行ではログだけ。Runtime は編集データを持たないので理由の表示と終了だけ。
- 退避は通常のシーン保存と同じシリアライザ。Play 中の実行時状態は保存しない。

## 決定 6: WARP へ黙って落とさない

`GraphicsDevice::Init` は HW に失敗すると WARP へ自動で落ちる。復旧時はこれを使わず、消失前と同じ種類 (HW / WARP) で作り直し、
上の再試行で作れなければ致命停止する。WARP での続行は桁違いに遅く、「動いているが原因が分からない」状態を作る。

## 決定 7: 試す入口 (疑似消失)

D3D11 にはデバイスを即時に消す API が無い。`Present` の判定の直前で「消えた」と扱わせる入口を作り、以降は本物と完全に同じ経路を通す。

- CLI `--simulate-device-lost <frame>[,<frame>...]` (複数指定は昇順に 1 回ずつ発火)、`--simulate-device-lost-fatal` (復旧せず致命停止へ)。
- エディタのメニュー View > Rendering の「デバイス消失を偽装」。`EngineContext::requestSimulatedDeviceLost` を立て、EngineLoop が
  フレーム末に読んで消す。
- `_DEBUG` / `NDEBUG` で分岐しない。sim に触れないので AGENTS.md §4.3 に抵触しない。
- 本物の TDR は `dxcap -forcetdr` (管理者、実機 GPU) で人が確認する。手順は `docs\test_checklists.md` の M88。

## 既知の差と未対応

- **K1**: `--render-demo --deferred --probe-bake-all` で、同じ実行の中で BakeAll を 2 回走らせると deferred の絵が変わる
  (復旧後の再ベイクで maxDiff 17〜21)。復旧後にベイクを遅らせると一致するので復旧の不具合ではなく、ベイカー側の既存の性質と推定している。
  原因は未特定で M88 の範囲外 (CLI の診断経路でしか通らない)。エディタの復旧は束を持っていたときだけ `OnDeviceRestored` で焼き直す。
- 履歴 (TAA / 時間フィルタ有りのフロクセルと RT / GPU 粒子) は復旧後に初期状態から再開するため、復旧直後のフレームは消失なしの実行と一致しない。
  履歴を切った条件では tol=0 で一致する。
- 復旧時間は既定デモで Release 約 4 s / Debug 約 13 s (2048² の PNG 56 枚のデコードが支配的)。TDR 自体が数秒画面を止め、sim は止まらず、
  ログに開始・完了・所要時間が出るので許容した。段階的な差し替えと DDS クックは後回し。
- ProjectManager のミニループは対象外 (シーンも sim も持たない)。

## 影響

- GPU を持つ型を新しく足すときは、`Shutdown` で GPU を手放し、容量カウンタを 0 に戻し、CPU の履歴を消さず、`Init` で作り直せるようにする。
  取りこぼしは `--simulate-device-lost` でゲートが不合格になるので見つかる (手順: `Editor.exe --simulate-device-lost 30 --frames 90`)。
- エディタのウィンドウが GPU オブジェクトや `ImTextureID` を保持するなら、`EditorApp::OnDeviceLost` から手放す口を足す。
