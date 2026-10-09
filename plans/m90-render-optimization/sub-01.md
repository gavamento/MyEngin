# sub-01: 計測の土台 (GPU ms・影の統計・ビュー別集計・render_bench・stats dump)

- 依存: なし
- 状態: OK (commit は司会が記入)
- 往復: 1

## やること
spec §4.1.1、§2 #14 #15。以降のサブが「効いた」を決定的な数で示すための土台。
1. `GpuTimer` を GBuffer (Deferred の不透明)、Forward の不透明、フレーム全体に足す (CSM・局所影アトラス・HZB は既存の計時を確認し、無ければ足す)。ProfilerWindow に並べる。
2. `prof::RenderStats` に影の draw / tri を別欄で足す。ビュー (viewKey) 別の集計を足し、従来の累積値と既存表示は残す。
3. 後のサブが埋める欄を先に用意する: LOD 段ごとの描画数、カスケード別の影 draw、パレット評価数 / 再利用数、オクルージョンのフェーズ別描画数・落とした数。このサブでは 0 のままでよい (欄と dump の形を固定するのが目的)。
4. コード生成のデモシーン `render_bench`: 多数 (目安 2000〜5000) の不透明メッシュをグリッドに、遮蔽する大きな壁、遠景、スキンのキャラ数体 (近・遠)、**カメラの視錐台の外にあって影だけが画面に落ちるキャスター**。固定カメラ (必要なら固定のカメラ位置を数点、tick で切り替えてカメラカットも含める)。既存デモの生成関数・順序は触らない。
5. CLI `--render-stats-dump <file>`: 決定的撮影の条件 (`--screenshot` と同じ固定条件) で指定フレーム描き、最後のフレームのビュー別統計と各段の GPU ms を JSON に書いて終了。Editor.exe と Runtime.exe のどちらで動かすかは既存の `--screenshot` の流儀に合わせる (memory「一時スクショ検証の道具の使い分け」: シーン撮りは Runtime.exe)。CLI の解析は `EngineCli.cpp` の表に足し、`EngineCliSelfTest.cpp` に 1 件。
6. `design-draft.md` と `harness.md` と本 plans ディレクトリ一式をこのコミットに含める (台帳の申し送り)。

## やらないこと (このサブでは)
- LOD・オクルージョン・カリングの変更。描画結果を 1 画素も変えない。
- ms を受け入れ条件にすること。

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Device\GpuTimer.*`、`src\Engine\Renderer\Pipeline\DeferredPath.cpp` / `ForwardPath.cpp` (計時の挿入点)
- `src\Engine\Core\Diagnostics\Profiler.h/.cpp` (`RenderStats`、`AddDraw`) と、影の draw 地点 `src\Engine\Renderer\Passes\ShadowPass.cpp:289` 付近、`ShadowAtlas.cpp`
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (viewKey の受け渡し)
- Editor の ProfilerWindow
- デモシーンの登録箇所 (既存デモの列挙。`replay_verify.bat` のシーン一覧へ入れるかは任意 — 入れるなら tick 数を短く)
- `src\Engine\Engine\App\EngineCli.cpp`

## 受け入れ条件 (このサブ)
1. ProfilerWindow に GBuffer / Forward / CSM / 局所影 / フレーム全体の GPU ms が出る (オクルージョンの欄は sub-02 で埋まる)。 — スクショ (Release、ユーザー目視用に保存先を報告)
2. 影の draw / tri が本描画と別欄。ビュー別の値の和が従来の累積値と一致する。 — selftest
3. `render_bench` を Debug / Release / `--warp` で `--render-stats-dump` し、ms 以外の全欄が 3 本で一致する。 — 3 本の JSON の比較結果を SELF_EVAL に貼る
4. 既存 golden が全部 PASS (このサブは絵を変えない)。 — `Editor.exe --selftest` Debug / Release
5. `tools\check_rules.ps1` PASS、新しい UI 文字列は `Tr()` で両言語。

## 検証コマンド
- ビルド Debug / Release (`MyEngine.sln`)。ファイル追加時は `tools\gen_project_files.ps1`
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- `tools\check_rules.ps1`
- `--render-stats-dump` を 3 構成で
- (デモを replay_verify に入れた場合) `tools\replay_verify.bat --job <scene>`

## 実装メモ (coder が追記)

### SELF_EVAL: sub-01 (round 1)

実装:
- `src/Engine/Core/Diagnostics/Profiler.h/.cpp` — `RenderStats` に影の draw / tri (`shadowDrawCalls` / `shadowTriangles` / `shadowCascadeDraws[3]`) と、後続サブが埋める欄 (`lodDraws[4]` / `paletteEvaluated` / `paletteReused` / `occlusionPhase1Draws` / `occlusionPhase2Draws` / `occluded`) を追加。`AddShadowDraw` / `AddRenderStats` / `SetRenderStatsView` / `GetRenderStatsForView` を追加。加算は `Accumulate` 1 箇所で累積値と viewKey 別の両方へ入れる (和の一致を構造で保つ)。`AddDraw` / `AddCulled` / `GetRenderStats` は従来のまま。
- `src/Engine/Renderer/Passes/ShadowPass.cpp` / `ShadowAtlas.cpp` — 影の DrawIndexed 全 5 地点へ `prof::AddShadowDraw` (CSM はカスケード番号つき)。
- `src/Engine/Renderer/Device/GpuTimer.h/.cpp` — `ready_` を追加 (Init 失敗なら Begin / End は何もしない)。
- `src/Engine/Renderer/Pipeline/RenderPath.h`, `DeferredPath.h/.cpp`, `ForwardPath.h/.cpp` — GBuffer (RenderGeometry、地形込み) と Forward 不透明 (地形込み) の GpuTimer。`GbufferGpuMs()` / `ForwardOpaqueGpuMs()`。
- `src/Engine/Engine/Rendering/RenderSystem.h/.cpp` — Render 全体の `frameTimer_` (遅延 Init、`ReleaseGpu` で戻す)、`prof::SetRenderStatsView(target.viewKey)`、パス計測の写し、アクセサ `FrameGpuMs` / `GbufferGpuMs` / `ForwardOpaqueGpuMs`。
- `src/Editor/Windows/Debug/ProfilerWindow.cpp` + `LocalizationTable.inl` — GPU 行 (frame / gbuffer / forward opaque)、影の行、ビュー別の行 (`Tr()` 3 本、en / ja)。CSM / 局所影の ms は既存の shadow 行。
- `src/Engine/Engine/Rendering/RenderStatsDump.h/.cpp` (新規) — `--render-stats-dump` の収集 / JSON 整形 / 書き出し。決定的な数は `counts`、ms は `gpuMs` の別節。
- `src/Engine/Engine/Rendering/RenderStatsSelfTest.h/.cpp` (新規) — ビュー別の和 = 累積値、影が本描画の欄に混ざらない、JSON の節分離。`EditorMain.cpp` の selftest 列へ登録。
- `src/Engine/Engine/Loop/EngineLoop.h/.cpp`, `src/Engine/Engine/App/EngineCli.cpp`, `EngineCliSelfTest.cpp`, `src/Editor/App/EditorMain.cpp` — `--render-stats-dump <file>` (表に 1 行 + selftest 1 件)。`EngineConfig::IsCaptureRun()` を足し、`--screenshot` の条件 (決定的撮影・バッチ実行・DPI 固定・露出即収束など 7 箇所) をそれに置き換えた。`--shot-frame` のフレームを描いた直後に書いて終了 (exit code 6 = 書けなかった)。
- `src/Engine/Engine/Demo/DemoContent.h/.cpp`, `ShowcaseScenes.cpp` — `BuildRenderBenchScene` と `--render-bench-demo` (表の末尾)。3660 個のグリッド (立方体 / 球 x 3 材質)、壁、遠景の球 8、Tower (カメラ背後・視錐台外・影だけ手前に落ちる)、スキンのキャラ 7 (近景 2 / 遠景 2 / 壁の裏 2 / カメラ背後 1)。固定カメラ、整数式の座標のみ。既存デモの生成関数・順序は無変更。
- `build/Engine.vcxproj` / `.filters` — `tools\gen_project_files.ps1` による更新 (新規 4 ファイルぶん。生成物だがコミットに要る)。

仕様との差分:
- [未実装] render_bench の「カメラカット」(固定カメラ位置を tick で切り替える) — sub では「必要なら」。カメラの切り替えには sim 側の仕組み (スクリプトかシステム) が要り、sub-01 の範囲を超えるため入れていない。sub-02 の「カメラカット直後に欠けない」検証は、`--shot-frame` と別カメラ位置のシーン (または GameLogic のスクリプト) が別途要る。
- [未実装] ProfilerWindow のオクルージョン欄 — spec どおり sub-02 で足す。
- [追加] `EngineConfig::IsCaptureRun()` と `--render-stats-dump` による「決定的撮影の判定」の置換 (7 箇所) — `--screenshot` と同じ固定条件で回す要件を、PNG を要求せずに満たすため。`--screenshot` だけのときの挙動は 1 ビットも変えていない (判定式が同値)。
- [追加] `GpuTimer::ready_` — フレーム全体の計時の Init 失敗で Begin / End が null クエリを叩かないようにするため。既存の全計時にも効くが、Init 成功時の動作は同一。
- [追加] 影の draw をカスケード別にも数えた (`shadowCascadeDraws`) — sub-04 の受け入れ条件 6 の「前」の値がこのサブで取れるようにするため (sub では 0 のままでよいとされた欄だが、数える地点が既にあった)。
- [追加] replay_verify のシーン一覧へは入れていない (sub では任意)。

検証:
- ビルド Debug / Release (`MyEngine.sln`、x64) → 成功 (警告・エラーなし)。
- `bin\x64\Release\Editor.exe --selftest` → exit 0 (2 回、最終ソースで 67 秒)。新規 `Render stats self test` 12 件と `Engine CLI self test` (`--render-stats-dump` の 1 件) が PASS。
- `bin\x64\Debug\Editor.exe --selftest` → **1 回目 exit 1、2 回目 exit 0 (6 分 40 秒)**。1 回目の失敗は `Server/client net self test: 2 FAILED` (`V1 LoadPersist / LoadGame in a session` の `[replay] cannot open ...\bin\x64\Debug\cache\server_net_selftest\save_boundary_late.rep` と `..._full.rep` / `.dump`、書いた直後のファイルが開けない)。描画・統計・CLI とは無関係の箇所で、再実行で同じソース・同じバイナリが通ったため一過性 (ファイルロック等) と判断したが、原因は断定していない。
- `tools\check_rules.ps1` → 0 error / 50 warning (既存の rule 7 警告のみ)。
- `tools\shot_verify.bat` (Release、30 枚の golden) → PASS (絵を変えていない証拠。受け入れ条件 4 の補強)。
- `--render-stats-dump` 4 構成 (`Runtime.exe --render-bench-demo --deferred --no-audio --font-embedded --width 960 --height 540 --shot-frame 30`、Debug / Debug `--warp` / Release / Release `--warp`) → `gpuMs` 以外の全欄が 4 本で一致 (JSON を読んで `gpuMs` 以外を辞書比較、ALL IDENTICAL)。値: `drawCalls 16 / triangles 1322628 / culled 307 / shadowDrawCalls 10092 / shadowTriangles 3967884 / shadowCascadeDraws [3364,3364,3364]`、`viewKey 1` の 1 ビューのみ。ms は構成で大きく違う (frame: Debug 実機 60 ms、Debug WARP 275 ms、Release 実機 2.75 ms、Release WARP 156 ms)。
- Forward (`--deferred` なし) の Release 実機 dump → `forwardOpaque` 2.84 ms、`gbuffer` 0。
- D3D デバッグレイヤー on の Debug 実機実行で、フレーム全体の GpuTimer (他の GpuTimer を内包する入れ子の TIMESTAMP_DISJOINT) に D3D のエラー・警告は出なかった。
- ProfilerWindow のスクショ (Release、WARP、Deferred、Scene ビューが viewKey 2): `C:\HAL\MyEngin\bin\x64\Release\shots\m90a_profiler_deferred.png`。確認項目: gpu 行 (frame 158.3 / gbuffer 54.9 / forward opaque 0.0 ms)、shadow 行 (csm 56.8 / atlas 0.0 ms)、`shadow: 10194 draw calls ...`、`view 2: ...`。**この PNG は撮影のためだけに Profiler を一時的に浮かせて大きくした一時コードで撮った** (コードは元に戻し、Release を再ビルドして再度 selftest を通した)。通常のレイアウトでは Profiler はドック内のタブで、GPU 行はスクロールしないと見えない (ユーザー目視用)。
- 未実施: `tools\replay_verify.bat` (このサブは sim・ABI・snapshot に触れていない。render_bench を replay_verify に入れていないので `--job` も無し)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜5 を満たした。減点: 「カメラカット」未実装 (sub では任意)、ProfilerWindow のスクショは一時コードで撮った。
  正しさ: 4 — 4 構成で決定的な数が一致、golden 30 枚 PASS、Release / Debug selftest PASS (Debug は 1 回目に無関係のネットテストが落ち、再実行で通った)。減点: その一過性の失敗の原因を断定していない。
  コード品質: 4 — 加算を 1 箇所に集めて和の一致を構造で保証。`IsCaptureRun` で `--screenshot` の判定式を 1 本化。減点: ProfilerWindow の新行は既存行と同じく技術用語が英語主体。
  テスト: 4 — 統計の集計・JSON 形・CLI 解析を selftest で押さえた。減点: render_bench のシーン構築・dump の実走は selftest ではなく手動実行 (CI の WARP でも回る形にはしていない)。

不安・質問:
- (planner へ) render_bench に「カメラカット」を入れるなら、sub-02 でカメラを tick で動かす手段 (GameLogic のスクリプトか、デモ専用の sim 系) を決める必要がある。sub-01 では入れていない。
- (planner / sub-04 へ) 影のキャスターは `queue_.Sort()` の前に収集順のまま `ShadowPass::Render` へ渡っている (`RenderSystem::Render` で `RenderCascadeShadows` が `queue_.Sort()` より前)。このため CSM では (material, mesh) の run が作られず、render_bench では影が 1 カスケードあたり 3364 draw のバラ描きになっている (本描画は run が効いて 16 draw)。sub-04 のカスケード別リストを作るときに、並びを揃えるかどうかを仕様として決めてほしい (並びを変えると影の描画順が変わるが、深度だけの描画なので絵は変わらないはず — 未検証)。
- (planner へ) Debug の selftest が 1 回だけ ServerNetSelfTest で落ちた件 (上記)。司会 / reviewer が Debug selftest を回したときに再発したら、このサブの変更が原因ではないか (ファイル I/O のタイミング) を見てほしい。

触ったファイル:
- src/Engine/Core/Diagnostics/Profiler.h, Profiler.cpp
- src/Engine/Core/Localization/LocalizationTable.inl
- src/Engine/Renderer/Device/GpuTimer.h, GpuTimer.cpp
- src/Engine/Renderer/Passes/ShadowPass.cpp, ShadowAtlas.cpp
- src/Engine/Renderer/Pipeline/RenderPath.h, DeferredPath.h, DeferredPath.cpp, ForwardPath.h, ForwardPath.cpp
- src/Engine/Engine/Rendering/RenderSystem.h, RenderSystem.cpp
- src/Engine/Engine/Rendering/RenderStatsDump.h, RenderStatsDump.cpp (新規)
- src/Engine/Engine/Rendering/RenderStatsSelfTest.h, RenderStatsSelfTest.cpp (新規)
- src/Engine/Engine/Loop/EngineLoop.h, EngineLoop.cpp
- src/Engine/Engine/App/EngineCli.cpp, EngineCliSelfTest.cpp
- src/Engine/Engine/Demo/DemoContent.h, DemoContent.cpp, ShowcaseScenes.cpp
- src/Editor/Windows/Debug/ProfilerWindow.cpp
- src/Editor/App/EditorMain.cpp
- (生成物だがコミットに要る) build/Engine.vcxproj, build/Engine.vcxproj.filters

申し送り:
- sub-02 以降へ: 動かし方 — `Runtime.exe --render-bench-demo --deferred --no-audio --font-embedded --width 960 --height 540 --shot-frame 30 --render-stats-dump <out.json> [--warp]`。`cache\render_bench.scene.json` が残っているとシーンを組まずに読む (他のデモと同じ。実行前に消すこと)。dump の `counts.total` が sub-02 以降の基準値 (オクルージョン ON/OFF の比較など)。
- sub-02: `prof::AddRenderStats` で `occlusionPhase1Draws` / `occlusionPhase2Draws` / `occluded` を加算する (dump に既に欄がある)。ProfilerWindow のオクルージョン欄と `RenderStatsDump` の `gpuMs` へ項目を足すときは、`counts` と `gpuMs` の節分離を保つこと (ms を `counts` に入れると構成間の一致比較が崩れる)。
- フレーム全体の `frameTimer_` は `RenderSystem::Render` の先頭から `ResolvePost` の終わりを囲む。他の GpuTimer を内包して入れ子の TIMESTAMP_DISJOINT になるが、デバッグレイヤー on の実機で問題は出なかった。
- 影の draw は GBuffer / Forward の `AddDraw` とは別欄で、`prof::GetRenderStats().drawCalls` には入らない (従来の Prof_Draw 表示は変わらない)。
- 一時スクショ検証でファイルを書き戻す際は `Copy-Item` が更新日時を保つので、MSBuild が「最新」と見なして再ビルドしない (今回 1 回踏んだ)。`LastWriteTime` を触ってから再ビルドすること。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-09)。受け入れ条件 1〜5 を根拠欄で確認 (4 構成の dump 一致、shot_verify 30 枚 PASS、check_rules 0 error)。should: フレーム GPU ms は Render 1 回分 (複数ビューでは最後のビュー) なので表示名を実態に合わせる → sub-02 へ移管。カメラカット → sub-02 へ移管 (描画側のカメラ上書きで、sim に触れない)。影のキャスターの並べ替えとインスタンシング → sub-04 へ。Debug selftest の ServerNetSelfTest の一過性 FAIL は本サブと無関係と判断 (触ったファイルにネット・セーブの経路が無い)。再発したら台帳に記録する。nit: `ImGui::Text(Tr(...), ...)` は規約 4.3 の文言とずれるが、同じファイルの既存 12 箇所と同じ流儀なので今回は不問 (reviewer 判断へ)。
