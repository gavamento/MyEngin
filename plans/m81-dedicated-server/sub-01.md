# sub-01: ヘッドレス Server.exe で golden .rep を照合する縦切り

- 依存: なし
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec D1 / D2 / H1〜H3。**M81 全体の荷重のかかる未知** (GPU 無しで RunOneTick が Runtime と同じハッシュ列を出すか) を、ネットを一切作らずに最小で潰す。

1. `build\Server.vcxproj` (Console サブシステム、Engine.lib をリンク) と `src\Server\ServerMain.cpp` を追加し、`MyEngine.sln` と `tools\gen_project_files.ps1` に登録する。リンクする lib は Runtime と同じでよい (D2)。d3d11.dll / dxgi.dll / d3dcompiler_47.dll は `/DELAYLOAD` にする (`delayimp.lib`)。
2. ヘッドレス sim ホスト (置き場所は Engine 側、例 `src\Engine\Engine\Loop\HeadlessSim.*` — sub-04 の selftest が 1 プロセスに複数立てるので、**グローバル状態を持たず複数インスタンス化できる形**):
   - GraphicsDevice / 窓 / オーディオデバイス / ImGui を作らずに、`TickServices` の全メンバを用意する。RenderResources は device 無しで CPU 側 (Mesh::positions / indices、SkinnedModelLibrary) だけ持つ。VfxRenderer / ParticleSystem 等の描画系は実体を作って描かないだけ (RunOneTick が参照を外さずに済む)。
   - シーンの用意は Runtime / Editor の `--replay-verify` と**同じ規則**: 同じデモフラグ (`--local-demo` / `--physics-demo` / ...) で同じシーンを組む、または .rep の埋め込みスナップショットから復元する。GameLogic.dll をロードする (ホットリロード無し)。C# は起動しない。
   - tick ループは EngineLoop の verify 経路と同じ順序: 入力の置換 → `RunOneTick` → `ApplyStructuralChanges` 後のハッシュ照合 → 不一致なら `<rep>.tickN.actual.dump` を残して exit 1。実時間を待たない (最速)。
   - **planner の事前調査 (Explore、2026-10-01) で分かっている落とし穴** — coder は鵜呑みにせず確認すること:
     a. 初期化は `EngineLoop::Run` の一枚岩 (`EngineLoop.cpp:107-591`)。スキーマ型の登録は DLL ロードより前 (TypeId がずれる、:415-420)、TagNames / UI 基準解像度 / フォント計測 / AssetDatabase のキー解決 (:529) / CookedCache / コライダ類の Install (:284-307) は OnStart より前。**手で写すと 1 つ漏れただけで tick 0 から MISMATCH になるので、GPU 系 (Window / Device / SwapChain / ImGui / 描画パス / UIRenderer / RenderSystem) を除いた sim 側初期化を、順序を変えずに EngineLoop と共有する関数へ切り出す**のが本筋。
     b. golden .rep は **Editor.exe** が `--rep-snapshot` 無しで録っている (`replay_verify.bat:321`) ので、シーンは Editor の OnStart と同じ手順で組み直す必要がある。`--parts-demo` / `--flow-demo` は `ShowcaseScenes.cpp:28-32` で editorOnly (Runtime は検証できない) → ヘッドレス側は editor=true 扱いで引く。Editor 固有の `--save-scene-on-start` (cache\*.scene.json の保存と、存在時のロード分岐。CWD 相対) と、Play 開始時の Save+Load (`PlayModeController.cpp:17-24`) も再現する。
     c. `resources.Init(device)` を呼ばない代わりに、組込みプリミティブ 7 種 (`Cube`〜`WaterPlane`) を明示的に登録する (未登録だとメッシュコライダ・凸包が解決できない、`MeshColliderLibrary.cpp:181`)。device 無しの `MeshLibrary::Register` は positions / indices / AABB を保持する (`GpuResources.cpp:163-219`)。SkinnedModelLibrary は CPU のみ (selftest で実証済み)。
     d. `particleSystem.Init` の代わりに `LoadSettings(<assets>\project_settings.json)` だけ呼ぶ (CPU/GPU バックエンドの選択がそこで決まる、`ParticleSystem.cpp:24,119-120`)。`vfxRenderer` は Init 無しで UpdateTrails / Reset が安全。`SetComputeAbi` は null を渡す (null 安全は selftest 済み)。
     e. **AudioSystem::Init を呼ばない** (XAudio2 を作る。H2 で xaudio2 のロードを禁じる)。未初期化でも tick 末の出力レーンは動く (`AudioSystem.cpp:222-234`) — 実際に確認すること。
     f. テクスチャは device 無しで null の AssetID が返る。テクスチャ ID をコンポーネントへ書くデモ (`DemoContent.cpp:1393-1395`、render-demo のみ) は golden 対象外だが、他にあればハッシュが割れるので洗い出す。
     g. **cooked キャッシュの汚染**: ヘッドレスでモデルをクックすると texture=0 の材質を .mmdl に書く (`ModelLoader.cpp:194,212-233`)。Editor / Runtime と共有の `<exeDir>\cache\cooked` を汚さないよう、Server は cook キャッシュを読まない/書かない (`useCookCache=false` 相当) か別ディレクトリにする。DllReloader のシャドウコピー先 `cache\hot` も並列実行で衝突しうる (`EngineLoop.cpp:439-449`) ので Server 用に分ける。
     h. Engine.lib を使う限り d3dcompiler / nethost / xaudio2 の import は消えない。`/DELAYLOAD` の対象に d3dcompiler_47.dll と xaudio2_9.dll も入れ、呼ばない限りロードされないことを H2 で確かめる。nethost.dll は ManagedHost が呼ばなければロードされないかを確認 (されるなら H2 の検査対象外として理由を書く)。
     i. フレーム末の `transformSystem.Update` (`EngineLoop.cpp:2406`) はヘッドレスに無い。再シム経路が tick 連続実行でハッシュ一致している前例があるので影響は低い見込みだが、H1 の結果で確かめる。
3. `Server.exe --replay-verify <rep> [<デモフラグ>]` で 2. を回し、`verified N ticks` / 不一致 tick をログに出して終了コードで返す。
4. 終了前に `GetModuleHandleW` で d3d11.dll / dxgi.dll / d3dcompiler_47.dll / xaudio2_9.dll が**ロードされていない**ことを自己検査 (H2)。ロードされていたら exit 2 とログ (どの DLL か)。
5. **常設の見張りにする**: `tools\replay_verify.bat` の `:chain` (シーンごとの record → verify(Debug) → verify(Release)) の末尾に「Release の Server.exe で同じ .rep を `--replay-verify`」を足す (H1 で通ったシーンだけ。通らないシーンはその理由をバッチのコメントに書く)。CI (`.github\workflows\ci.yml` の replay determinism ステップ) はこの bat をそのまま呼ぶので、追加の CI 定義は要らない。以降の全サブの C1 がヘッドレスのビット一致も検査することになる。
6. **EngineLoop 側との二重実装を作らない**: TickServices の組み立て・シーン準備のうち、EngineLoop と HeadlessSim で同じことをする部分は共通関数へ寄せるか、寄せない理由 (描画系の初期化と絡み合っている等) を SELF_EVAL に書く。EngineLoop の挙動 (replay_verify のハッシュ列) は 1 tick も変えない。

## やらないこと (このサブでは)
- ネット・プロトコル・ホスティング・実時間ペース (sub-04/05)
- Session 型 / .rep v9 (sub-02)
- Engine を sim 専用ライブラリへ分割すること (D2 で却下)
- 通らないシーンを通すための大きな改修 — 原因の特定まで。直すかどうかは planner が VERDICT で裁定する

## 触る場所 (planner の見立て)
- 新規 `build\Server.vcxproj` (+ `.filters`)、`MyEngine.sln`、`tools\gen_project_files.ps1` (Server の節)
- 新規 `src\Server\ServerMain.cpp`、`src\Engine\Engine\Loop\HeadlessSim.h/.cpp` (名前と置き場は coder が既存に合わせて決めてよい)
- `src\Engine\Engine\Loop\EngineLoop.cpp` (TickServices とシーン準備の共通化をするなら)
- `src\Engine\Renderer\Device\GpuResources.*` (device 無しの Mesh 登録 / SkinnedModel 登録が CPU データを保持できない場合のみ。既存の「device 無し = ヘッドレス」(M48a) の流儀に合わせる)
- `src\Engine\Engine\App\EngineCli.cpp` (Server が同じパーサを使うなら)

## 受け入れ条件 (このサブ)
spec 5. の **H1, H2, H3, H4** と **C1, C2, C3, C5, C6**。
- H1 の照合対象: `tools\replay_verify.bat` が `cache\` に残す golden .rep。**必須 5 本**: `golden_mp.rep` (`--local-demo`)、`golden_physics.rep` (`--physics-demo`)、`golden_parts.rep` (`--parts-demo`、スキンドモデル + 部位追従)、`golden_joints.rep` (`--joint-demo`)、`golden_fracture.rep` (破壊)。残り 4 本 (既定デモ / flow / acoustic / ui) も試し、結果を表で SELF_EVAL に貼る。通らないものは原因 (file:line) と「直すのに何が要るか」の見立てを書く。
- Debug と Release の Server.exe の両方で照合する (Debug で録った .rep を Release の Server.exe で通す = 構成間のビット一致も兼ねる)。

## 検証コマンド
- `tools\replay_verify.bat` (golden .rep の生成と C1 を兼ねる)
- `bin\x64\Debug\Server.exe --replay-verify cache\golden_mp.rep --local-demo` 等 9 本 × Debug / Release (実際のフラグは replay_verify.bat の各ジョブに合わせる)
- `pwsh -File tools\check_rules.ps1`
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- exe は PowerShell ツールから実行する (Bash の `cmd /c` は実行されない — メモリ「exe の実行は PowerShell ツールから」)

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-01 (round 1)
実装:
  - build\Server.vcxproj (+ .filters) / MyEngine.sln / tools\gen_project_files.ps1 — Console サブシステムの Server を追加。Runtime と同じ lib をリンクし、d3d11 / dxgi / d3dcompiler_47 / xaudio2_9 / Xinput9_1_0 / nethost を /DELAYLOAD (delayimp.lib、LNK4199 は /IGNORE)。dumpbin /dependents の結果は Debug = KERNEL32 + ole32 のみ (遅延は XAudio2_9 / D3DCOMPILER_47 / nethost)、Release = 同じく KERNEL32 + ole32 (遅延は XAudio2_9 / D3DCOMPILER_47)。d3d11 / dxgi / Xinput はリンカが未参照の GPU 系オブジェクトを引かないので import 自体が無い
  - src\Server\ServerMain.cpp — wmain。共有パーサ (ParseEngineCliFlag) + --*-demo (editor=true で引く) / --scene / --project / --terrain-*。`--replay-verify` を HeadlessSim へ。終了前に GetModuleHandleW で 6 DLL (d3d11 / dxgi / d3dcompiler_47 / xaudio2_9 / xinput9_1_0 / nethost) が未ロードを自己検査 (ロードされていたら exit 2)。シャドウコピー先が Editor / Runtime の cache\hot と別であることも検査してログに出す。exit 0 = 全 tick 一致 / 1 = 不一致・実行不能・未知引数 / 2 = H2 違反
  - src\Engine\Engine\Loop\HeadlessSim.h/.cpp — GPU / 窓 / オーディオ / ImGui 無しの sim ホスト。状態は全部インスタンスメンバ (pimpl)、TickServices の全メンバを揃え、verify ループは「入力置換 → RunOneTick」だけ (ハッシュ照合 + <rep>.tickN.actual.dump + .mismatch.txt は RunOneTick 内の既存経路)。RenderResources は InitHeadless() (組込みメッシュ 7 種を CPU データで登録)、ParticleSystem は LoadSettings のみ、ShaderManager / AudioSystem / ManagedHost は Init しない (Load は ID 予約・再生 no-op・C# レーン停止 = ヘッドレス規約)。クックキャッシュは強制 off、シャドウコピーは <exeDir>\cache\hot_server\i<連番>、セーブは <exeDir>\save_server (Editor / Runtime と共有しない)
  - src\Engine\Engine\Loop\SimInit.h/.cpp (新規) + EngineLoop.cpp — EngineLoop::Run から「GPU 系を除く sim 側の起動手順」を切り出して両者が呼ぶ: ResolveAssetsRoot / InstallSimLibraries (+ UninstallSimLibraries) / InitSimProjectState (UI 基準解像度・フォント計測・タグ名) / LoadGameLogic (スキーマ型登録 → DLL。順序が TypeId を決める) / WireScriptServices / InitSimAssets (アクションマップ + AssetDatabase) / ConfigureSimCaches。EngineLoop は各呼び出しを元の位置に置換しただけ
  - src\Engine\Engine\Demo\StartScene.h/.cpp (新規) + src\Runtime\RuntimeMain.cpp — RuntimeApp::OnStart の本体 (実体登録 → シーン解決 → ロード/ビルド → Save+Load 正規化) を PrepareStartScene へ移し、Runtime とヘッドレスが共有 (Runtime の挙動は不変)
  - src\Engine\Renderer\Device\GpuResources.h/.cpp — MeshLibrary::InitHeadless / RenderResources::InitHeadless を追加 (Init と同じ 7 種・同じ順序の登録)。TextureLibrary::LoadFile は device 無しなら decode 前に null を返す (従来も最終的に null + ERROR ログだったものを、PNG の無駄な decode と ERROR を省いて同じ結果にした)
  - tools\replay_verify.bat — :chain の末尾に Release Server.exe の `--replay-verify` を追加 (9 シーン全部)。ヘッダコメントも更新。CI は bat をそのまま呼ぶので ci.yml は無変更
仕様との差分:
  - [追加] StartScene.h/.cpp (Runtime の OnStart 本体の抽出) と RuntimeMain.cpp の変更 — sub の「触る場所」に無いが、sub 項 6 (二重実装を作らない) の「シーン準備の共通化」を満たすため。Editor の OnStart は Editor 固有処理 (選択・設定・Undo 等) と絡むので寄せていない (Runtime と同じ手順であることは Runtime が Editor 録画の .rep を通せる事実で担保されている)
  - [追加] TextureLibrary::LoadFile の device 無し早期 return — Server の起動ログに数百行の ERROR と Debug で 95 秒の無駄な PNG decode が出たため。戻り値は不変
  - [追加] Server は cook キャッシュを常に無効にする (useCookCache を強制 false) — 落とし穴 g の回避を「別ディレクトリ」ではなく「読まない/書かない」で実装。代償: FBX/GLB を毎回パースするので起動が遅い (Release 約 0.9 秒、Debug 約 6 秒)
  - [追加] Server 専用のセーブ先 save_server — 落とし穴に無いが、フロー系シーンの SaveGame 書出 (出力レーン) が Editor / Runtime の save\ を触らないように
  - [追加] 検証用の確認として CHECK 対象 DLL に Xinput9_1_0 と nethost を足した (H2 の 4 本に加えて) — 落とし穴 h の「nethost がロードされるか」を確認し、されなかったので検査対象に含めた (除外にはしていない)
  - [未実装] --save-scene-on-start (Editor 固有。bat のジョブは別途 Editor を 1 回回して cache\parts_showcase.scene.json を作っており、Server はそのファイルをロードして通る) / クラッシュハンドラの設置 (Server はまだ運用ループを持たない)
  - [逸脱] 落とし穴 g の「DllReloader のシャドウコピー先 cache\hot が並列実行で衝突しうる」は、DllReloader::Init が既に PID 別の棚 (p<pid>) と死んだプロセスの棚の掃除を持っていて衝突はしない。それでも H2 の要求どおり Editor / Runtime とは別の置き場 (hot_server) に分けた
検証:
  - ビルド: Debug / Release とも MSBuild MyEngine.sln → 成功 (Server.exe 生成)。LNK4204 (libtess の外部 C) は Server のリンクで出るが既存プロジェクトと同種
  - Server.exe --replay-verify (Debug、ベースライン = 変更前 Editor Debug が録った 9 本、cwd = リポジトリ直下) → 9/9 exit 0、600 tick 全一致 (ms/tick: demo 8.5 / parts 1.0 / flow 0.9 / mp 0.44 / physics 2.6 / joints 32.3 / acoustic 11.8 / ui 9.4 / fracture 11.3、起動のアセット走査 5〜8 秒)
  - Server.exe --replay-verify (Release、同じ 9 本) → 9/9 exit 0 (ms/tick: demo 0.24 / parts 0.03 / flow 0.02 / mp 0.01 / physics 0.28 / joints 3.5 / acoustic 1.2 / ui 0.16 / fracture 2.5、アセット走査 約 0.9 秒)。Debug で録った .rep を Release の Server が通す = 構成間 + 実行形態間のビット一致
  - H1 の表 (9 シーン): 全部ヘッドレスで通る。通らなかったシーンは無い (golden_mp / physics / parts / joints / fracture の必須 5 本 + 既定デモ / flow / acoustic / ui)
  - 負のコントロール: `--net-poke-tick 100` 付きで mp を照合 → exit 1、`HASH MISMATCH at tick 100`、`poke.rep.tick100.actual.dump` と `.mismatch.txt` が出る。シーン違い (mp の .rep を既定デモで) → tick 0 で MISMATCH exit 1。存在しない .rep → exit 1。未知引数 → exit 1 + usage。(いずれも手動実行で、自動テスト化はしていない)
  - H2: 全 9 実行で `module check OK: d3d11 / dxgi / d3dcompiler_47 / xaudio2_9 / xinput / nethost are not loaded`。ログに isolation: cook cache disabled, shadow copy = ...\cache\hot_server\i0
  - EngineLoop を 1 tick も変えていない証拠: 変更前の Editor が録った 9 本の .rep と、変更後の Editor が replay_verify.bat で録り直した 9 本は SHA256 が全て一致 (バイト同一)
  - tools\replay_verify.bat (最終版、Server の末尾ステップ込み) → exit 0 `[PASS] replay consistency ... all 14 jobs passed in 132.1s`。ログに `=== verify ... : Release, headless Server.exe ===` が 9 本、各 `VERIFY PASS: hash-identical`
  - pwsh -File tools\check_rules.ps1 → exit 0 (0 error / 0 warning)
  - Editor.exe --selftest (Debug / Release) → どちらも exit 1。原因は Source control self test の 2 項目 (`external cherry-pick state closes the normal write gate` / `external revert state survives status refresh`) だけで、他の 50 スイートは ALL PASS。**変更前の HEAD (4e67907) を git archive して Release の Editor をビルドし直した結果も同じ 2 項目で exit 1** = 既存の失敗で今回の変更とは無関係 (SourceControl のセッション論理のテスト)
  - Runtime.exe --replay-verify (Release、--warp --no-audio) を mp / physics / fracture の Editor 録画 .rep で実行 → 3/3 VERIFY PASS (StartScene 抽出後の Runtime が壊れていないこと)
  - 1 プロセスに HeadlessSim を 2 個同時に生かして順に --replay-verify (使い捨てプローブ、コミットに含めない) → mp / fracture / joints / parts の 4 シーンで両方 600 tick 一致 (GameLogic.dll が 2 回ロードされる)。tick を交互に刻む使い方は未検証
自己採点 (1-5):
  仕様適合: 4 — H1〜H4 / C1〜C3 / C5 / C6 を満たした。逸脱は「追加」のみで上に全部列挙。EngineLoop 側との共通化は起動手順 (SimInit) とシーン準備 (StartScene) まで。TickServices の組み立てと verify ループの小片は EngineLoop のローカル変数に紐づくため寄せていない (下の「不安・質問」)
  正しさ: 4 — 9 シーン × 2 構成で全 tick 一致、負のコントロールで検出が生きていることを確認、変更前後の .rep がバイト同一。未検証: GPU パーティクルバックエンド (project_settings.json が gpu のとき)、--project 起動、Windows CI 実機 (--warp --no-audio を後置した Server の実行は共有パーサが受け取るところまで)、複数インスタンスの tick 交互実行
  コード品質: 4 — 既存の流儀 (日本語コメント / 4 スペース / PascalCase) と契約コメントを維持。SimInit の関数群は呼ぶ順序が契約なのでヘッダに順序を明記
  テスト: 3 — 回帰は replay_verify.bat (C1) が常設で守る。HeadlessSim 単体の selftest と負のコントロールの自動化はしていない (sub-04 の 1 プロセス内検証で扱う想定)
不安・質問:
  1. (planner 判断) cook キャッシュ無効のコスト: Release は約 0.9 秒だが、モデルの多いプロジェクトでは起動が伸びる。サーバ専用の cook 置き場 (cache\cooked_server) を有効にする案もある (texture=0 の材質が入るのは Server 専用なので汚染は起きない) が、「毎回パース ≡ クック再生」のビット一致はヘッドレスでは未証明のため sub-01 では見送った。後続で足すか判断してほしい
  2. (planner 判断) ParticleSystem は GPU バックエンドが project_settings.json で選ばれていると GpuParticleBackend::Update をデバイス無しで呼ぶ。既存の golden は cpu なので未検証。サーバ対象プロジェクトで gpu を許すなら「Server では CPU へ強制する」かの裁定が要る (sim の乱数消費列は CPU と同じ設計だが、デバイス無しの安全性は未確認)
  3. (planner 判断) スクリプトのコンピュート ABI (v21) はデバイス無しで 0 / no-op を返す。GPU の結果を読み戻して sim 状態へ入れるスクリプトがあれば Editor と Server で割れる。現状の golden には該当なし
  4. EngineLoop の tick ループ周辺 (TickServices の組み立て ~50 行、verify の入力置換) は HeadlessSim と二重になっている。EngineLoop のローカル変数 (window / device / netRb / recorder 等と同居) に紐づき、切り出すと Run 全体の再編になるため寄せていない。この 2 箇所を足す・消すときは両方を見る必要がある (HeadlessSim.cpp の BuildTickServices に TickServices の契約コメントへの参照を残した) — 許容でよいか
触ったファイル:
  - build\Server.vcxproj (新規) / build\Server.vcxproj.filters (新規) / build\Engine.vcxproj / build\Engine.vcxproj.filters (gen_project_files.ps1 の再生成) / MyEngine.sln / tools\gen_project_files.ps1 / tools\replay_verify.bat
  - src\Server\ServerMain.cpp (新規)
  - src\Engine\Engine\Loop\HeadlessSim.h / HeadlessSim.cpp / SimInit.h / SimInit.cpp (新規) / EngineLoop.cpp
  - src\Engine\Engine\Demo\StartScene.h / StartScene.cpp (新規) / src\Runtime\RuntimeMain.cpp
  - src\Engine\Renderer\Device\GpuResources.h / GpuResources.cpp
申し送り:
  - 次サブ (sub-02〜): HeadlessSim は pimpl で状態をインスタンスに持つが、プロセス共有の注入 (meshcol:: 等のライブラリ、ジョブシステム、AssetDatabase のキー解決、SceneSerializer の ManagedHost) は 1 つしか持てない。複数インスタンスを交互に回すときは tick の直前に Activate() を呼ぶこと。シャドウコピーはインスタンス連番で分けてあり、同一プロセスに 2 個立てても衝突しないことを確認済み
  - sub-04 向け: TickServices の組み立ては HeadlessSim::Impl::BuildTickServices、sim の準備は HeadlessSim::Init。ネットで確定入力を差し込む口は VerifyReplay の `ctx.inputs[p] = ...` の 1 箇所を一般化すればよい (RunOneTick の契約は無変更)
  - Debug の Server は joints で 32 ms/tick (Debug の物理が重い)。Release は 3.5 ms/tick。spec 4.4 の「4 クライアント相当で tick 平均 < 4ms」の参考値として mp 相当のシーンは 0.01 ms/tick
  - 検証に使った作業物 (リポジトリ外 / .gitignore 内): C:\HAL\_m81_sub01_base (変更前 HEAD を git archive したビルド一式 = 約 1 本ぶんのビルド出力。要らなければ削除してよい。AGENTS.md 第 1 章に従い、coder は削除していない)、cache\sub01_baseline\ (変更前 Editor が録った 9 本の .rep)、cache\sub01_neg\、tmp\sub01_*.log
  - Source control self test の 2 項目は HEAD 時点から失敗している (既存)。M66 側で直す必要がある
  - 既存: `/p:MyeWarnAsError=true` (CI 相当) のビルドはこの環境 (VS 18) では HEAD から ProjectComputeRunnerSelfTest.cpp の C4127 (条件式が定数です) で失敗する。今回追加・変更したファイル (HeadlessSim / SimInit / StartScene / ServerMain / EngineLoop / GpuResources / RuntimeMain) は Release で警告 0 を確認した
  - Server の初回リンクで LNK4204 (Engine.pdb にモジュール情報なし、libtess などの外部 C オブジェクト) が出る。Runtime の Debug 再リンクでも同種 (imgui オブジェクト) が出るので既存の性質
```

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。H1 は 9/9 シーン × Debug/Release でヘッドレス一致、H2 の自己検査、H3 (TickRunner 無変更を git diff で確認)、H4 (replay_verify に 9 本常設)。C3 は基点から失敗している Source control の 2 項目を除外 (spec C3 に明記)。不安 1→R-8 (cook 無効のまま)、2→R-6 (CPU へ強制しない)、3→R-7 (既知制限として文書化)、4→重複は許容、sub-04 で入力置換を共通化。should: HeadlessSim の負のコントロール (poke で MISMATCH) を自動化していない → sub-04 の selftest で扱う。
