# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

MyEngine — C++20 / DirectX 11 の自作ゲームエンジン (VS2022 / x64 / Windows のみ)。
仕様は `engine_spec.md`、設計判断は `docs/adr/`、README は日本語の機能概要。

**このリポジトリの最大の制約は「Debug / Release / CI (WARP) でシミュレーションがビット一致すること」**。
ビルド設定・コンポーネント追加・ABI 追加・シェーダ定数・UI 文字列の手順がすべてここから導かれる。
迷ったら「この変更は tick の結果を機種依存にしないか」を先に問うこと。

## ビルド

- `MyEngine.sln` = ネイティブ 4 プロジェクト (Engine.lib / Editor.exe / Runtime.exe / GameLogic.dll)。
  出力は `bin\x64\<Config>\`、中間は `obj\`。共通設定は `build\Common.props` に一元化 (/fp:precise・
  /Zi 固定・RuntimeLibrary 統一)。**`.vcxproj` を個別にいじらない**。
- MSBuild のパスは vswhere で解決する (`tools\*.bat` と同じ手順):
  ```
  for /f "usebackq tokens=*" %i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%i
  "%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
  ```
- **ソースファイルを追加・移動・削除したら `pwsh -File tools\gen_project_files.ps1`**。
  `.vcxproj` のソース列は `<!-- BEGIN FILES -->` 区間の生成物で、手書きしない。
- C# スクリプトホストは **sln の外** — `tools\build_managed.bat Debug` / `Release` を別途実行
  (`bin\x64\<Config>\MyeScripting.dll`。両構成とも起動時にロードされるので両方作る)。
- GameLogic.dll だけ焼き直す: `tools\build_scripts.bat <Config>`
  (エディタの「Rebuild Scripts」ボタンと同一。実行中のエディタが ~0.5s でホットリロードする)。

## 検証 (変更後に回すもの)

| コマンド | 担保するもの |
|---|---|
| `bin\x64\Debug\Editor.exe --selftest` | ヘッドレス回帰 43 スイート (D3D もウィンドウも作らない) |
| `tools\replay_verify.bat [ticks]` | 8 ビルド → 並列 10 ジョブ (7 シーンチェーン = 記録 `--replay-fast` + snapshot 往復付き照合 + Release 照合 / タイムトラベル ×2 / 規則検査)。1 本だけ回すなら `--job <名前>` 再入 (ビルド済み前提)、並列度は `MYE_REPLAY_JOBS` |
| `tools\shot_verify.bat [--update]` | 決定的スクショ 17 枚を `tests\golden\*.png` と比較 (CI 判定は 10 枚 — FXAA / TAA / SSR / froxel / fog の 5 枚は分岐反転や GPU sim で機種差が増幅するので tol=0 のローカル限定。地形の 1 枚だけ異方性フィルタの実装依存で tol=12。**物理・関節・霧の 3 枚だけ frame 120 で撮る** — 他は frame 3 = ほぼ初期配置なので物理も粒子も絵に出ない。**先に Release ビルドが必要**) |
| `pwsh -File tools\check_rules.ps1` | 規則 1/2/4/7/8/9/10/11 の静的検査 |
| `tools\crash_verify.bat [Debug\|Release]` | 5 経路で実際に落として crash バンドル → .rep が再生・再現すること (**CI 対象外**) |
| `tools\net_verify.bat [ticks]` | host/join 2 プロセスの .rep が一致 + ローカル 2P 参照とも一致 + ロールバック 3 帯 + desync 注入の検出 (**CI 対象外**) |

- **CI (`.github\workflows\ci.yml`) はこの bat をそのまま呼ぶ。CI 専用の検証ロジックを書かない。**
  CI 固有の事情は環境変数 4 種だけで注入する: `MYE_EXTRA_ARGS` (`--warp --no-audio`)、
  `MYE_MSBUILD_ARGS` (`/p:MyeWarnAsError=true`)、`MYE_DOTNET_ARGS` (`/p:TreatWarningsAsErrors=true`)、
  `MYE_SHOT_SKIP_FXAA` / `_TAA` / `_SSR` / `_FROXEL` / `_FOG` (機種差が増幅する 5 枚をランナーでは撮らない)。
  ※ C++ の警告 0 は `/p:TreatWarningAsError=true` では**効かない** (ClCompile の項目メタデータなので
  グローバルプロパティは誰にも読まれない)。`Common.props` の `MyeWarnAsError` 橋渡しを使う。
- **セルフテストに絞り込みフラグは無い**。1 本だけ回したいときは `src\Editor\EditorMain.cpp` の
  `if (selftest)` 内の `&&` 連鎖を一時的に削る。連鎖は短絡なので、最初に落ちた 1 本で以降は走らない。
- リプレイが割れたら: `--hash-diff <expected.dump> <actual.dump>` でフィールド単位まで落とす
  (失敗側の `.actual.dump` と `.mismatch.txt` は EngineLoop が自動で残す)。
  いつ壊れたかは `tools\bisect_replay.bat` を `git bisect run` に噛ませる。
- 手動確認用の主な CLI (`Editor.exe` / `Runtime.exe` 共通のものが多い):
  `--replay-record F --replay-ticks N` / `--replay-verify F` / `--snapshot-stress N` /
  `--timetravel-selftest [N]` /
  `--crash-test <av|purecall|terminate|invalidparam|stackoverflow>` /
  `--crash-at-tick N` / `--no-crash-handler` /
  `--local-players N` / `--synth-input` / `--local-demo` /
  `--net-host [PORT]` / `--net-join HOST:PORT` / `--net-delay N` / `--net-loss N` /
  `--net-no-rollback` / `--net-no-halt-on-desync` / `--net-poke-tick N` / `--net-demo` /
  `--rep-diff A B` /
  `--scene PATH` (相対可) / `--screenshot PNG --shot-frame N --frames N` / `--warp` /
  `--font-embedded` / `--no-audio` / `--parts-demo` / `--flow-demo` / `--rt-demo` /
  `--render-demo` (M54a: 描画ショーケース。スクショ回帰 6/7 枚目) /
  `--terrain-demo` (M58c: 地形ショーケース。8 枚目) / `--terrain-lod N` / `--terrain-skirt N` /
  `--physics-demo` (M59d: 空力/浮力/材料のショーケース。replay 5 ペア目 + スクショ 13 枚目) /
  `--joint-demo` (M60i: 関節/機構/ラグドール/車のショーケース。replay 6 ペア目 + スクショ 14 枚目。substeps 16) /
  `--fog-demo` (M57追補: 霧 + GPU 粒子 + Sprite/Trail/TextMesh のショーケース。スクショ 15 枚目 =
  GPU 描画経路と VfxRenderer の唯一のピクセル被覆) /
  `--acoustic-demo` (M65b/M65c: 音響伝播のショーケース。L 字の廊下 + 2 部屋 +
  床材タイル 6 枚を往復する歩行者 + 金属板へ落ちる箱。replay 7 ペア目 =
  波スロット表がハッシュに載る唯一の場所。**波は SceneView の「音響」トグルでしか見えない** —
  ライティングへの差し込みは M65e) /
  `--particle-backend <cpu|gpu>` / `--particle-compare` (M57追補: バックエンドの CLI 固定。
  project_settings.json より優先し**書き戻さない**。GPU 粒子を --screenshot で撮る唯一の口) /
  `--taa` (M55d) / `--ssr` (M56d) / `--froxel` (M57) / `--hzb-debug N` (M56c) /
  `--velocity-debug` (M55c) / `--froxel-dump N` / `--froxel-no-temporal` (M57) /
  `--package DIR` / `--img-diff A B [--tol N]`。

## 決定論の契約

`engine_spec.md` 11.2 の規則 1-11 が本文。実務上効いてくるのは:

- `#ifdef _DEBUG` でロジックを分岐しない (ログ・可視化のみ可)。`assert` ではなく `MYE_CHECK`。
  宣言時に必ず初期化。`/fp:fast` 禁止。
- **ポインタ値・`unordered_*` の走査順・実時間・`rand()`/`std::random_device` を sim に混ぜない。**
  乱数はエンジンの PCG32 のみ。ソートは明示的な決定論キーで。
  unordered を舐めてバイト列を作るときは昇順に整列してから (SimSnapshot の override 表が実例)。
- `check_rules.ps1` が拾うのは静的に見える違反だけ。**時計依存・順序依存は拾えない** —
  そこは `replay_verify.bat` の 6 ペア照合が唯一の検出手段。
- **入力レーン (M52g)**: sim は `EngineContext::inputs[kMaxPlayers=4]` を消費する
  (`Input()` = レーン 0 = 従来の単一入力の別名)。レーン 0 だけがキーボード/マウスを持ち、
  レーン n は XInput スロット n。`.rep` の `playerCount` がレーン数で、**検証時は .rep が
  `--local-players` に勝つ**。レーンの配線をハッシュに載せているのは `PlayerInputComponent`
  のミラーだけ — ここに `kFieldNoSerialize` を付けるとハッシュ対象から外れて被覆が消える。
- **ネット (M52h/M52i)**: 2 人 P2P の遅延ロックステップ + 予測ロールバック。`NetSession` は
  「tick T が消費するレーンを全 peer 分そろえる」だけで **sim 状態を 1 バイトも書かない**。
  ハンドシェイクで proto / `MYE_API_VERSION` / .rep 版 / snapshot 版 / 入力遅延 /
  起動オプション / **開始ワールドハッシュ**を照合して不一致は接続拒否する。ネット中は
  C# レーン停止 + `LoadGame` 禁止 (`TickServices::netLockstep`) + タイムトラベル起動禁止。
  **オーディオは止めない**。
  自レーンの値は「tick t を回す直前に t+delay を 1 回だけ確定」で送る (2 回送ると desync)。
- **ロールバック (M52i)**: 未着レーンは `NetSession::PredictLane` (直近確定値の繰り返し) で
  埋めて先行し、外れたら `NetRollback::SnapshotBefore` へ Restore → `RunOneTick` で再シム。
  上限 `kNetMaxSpeculation=8` tick、超過は stall (= M52h へ縮退。`--net-no-rollback` で常時)。
  ★**予測で走った tick は .rep に書かない / ハッシュも主張しない** — 記録は RunOneTick から
  EngineLoop の確定処理 (`NetCommitConfirmed`) へ移してある。ここを崩すと「ロールバック有りの
  .rep がロックステップとバイト一致する」= net_verify の主張が消える。
  ★`net.OnTickConsumed` を進めるのも**確定 tick まで**。投機 tick で進めると後から届いた
  本物の入力が捨てられ、予測が永久に直らない。
  desync 検出は 8 tick 刻み (`kNetHashCheckpoint`) の確定ハッシュ交換。割れたら
  `crash\desync_<tick>_p<lane>\` を吐いて **exit 4** (`--net-no-halt-on-desync` で継続)。
- 描画結果は決定論の対象外 (ハッシュに入らない) が、**スクショ回帰は別途機種依存を殺している**:
  ラスタライザは `--warp` 固定、フォントは `--font-embedded` 固定、`--screenshot` 指定時は
  dt を固定 tick 長にして **frame 番号 == tick 番号** にし、非同期テクスチャを撮影前に drain する。

## 横断的な変更のチェックリスト

**コンポーネントを足す** — `Components.h` に POD で定義 → `RegisterBuiltinComponents()` の
**末尾に append** (TypeId は登録順で決まる。途中挿入は既存シーンと .rep を壊す。
現行の末尾は **49 = AgentBrain** — M65a が 45〜49 を取り、M60′ の Cloth/SoftBody 予約は
50/51 へ繰り下げてある) →
`FieldDesc` 表を書く (`MYE_JP("表示名", MYE_FIELD(...))`) → ハッシュ対象になるか確認 →
影響するなら `SceneSerializer` の版と `.rep` の版を検討。

**`FieldType` を足す** — Inspector widget / JSON シリアライズ / DLL リロード時の状態移行 /
ワールドハッシュの **4 者すべて**に対応を入れる (`Reflection.h` 冒頭のコメントが正本)。
`FieldDesc` のメンバ追加は**必ず末尾**へ (先頭 4 メンバは位置指定初期化で使われている)。

**ABI スロットを足す** (`src\Shared\EngineAPI.h`) — `Interop.cs` は**位置ベースのミラーで実行時の
版検証が無い**。順序・件数・名前・引数個数を揃え、`EngineApiTable.cpp` で全スロットを充填し、
`MYE_API_VERSION` の bump と `check_rules.ps1` の `$apiVersionSlots` 表の更新を**同時に**行う
(片方だけだと規則 11 で止まる)。現行は v15 = 104 スロット。C# レーンは replay 被覆の外なので、
実走確認は一時的な probe スクリプトで行う。
**v13 の `Net*` 5 本は機種依存の値を返す** — スクリプトが sim 状態へ書き戻すと 2 台の
ワールドハッシュが割れる。禁止する手段は無いので desync 検出が唯一の防波堤
(`--net-demo` の `NetHudDemo` = UIElement へ書くだけ、が正しい使い分けの実例)。

**C++ と HLSL で定数を共有する** — 追加したら `check_rules.ps1` の `$constGroups` にも登録する
(食い違いは定数バッファ不一致として静かに壊れるので、機械照合が唯一の防波堤)。

**UI 文字列** — `src\Engine\Core\LocalizationTable.inl` に en/ja 両方を書き、`Tr()` 経由で読む。
`Tr()` を printf 系の**唯一の引数**にしない (`TextUnformatted(Tr(x))` か `Text("%s", Tr(x))`)。
`###` の右辺 (= ImGui ID) は両言語一致かつテーブル内で一意。書式指定子の並びも一致必須。

## アーキテクチャで押さえること

```
Editor → GameLogic → Engine → Renderer → Core → Platform   (上位は下位のみに依存)
```

- **生の D3D 型を Renderer 層より上に出さない。`src\Shared\` の DLL 境界は C ABI + POD のみ**
  (STL / vtable / 例外は越えない)。
- **ハイブリッド ECS** — 外部 API は `GameObject` / `GetComponent<T>()`、内部はアーキタイプ別 SoA。
  世代付き `EntityID`。構造変更はコマンドバッファで tick 末に一括適用。
- **スクリプトの状態はエンジン側 ECS に置く** (GameLogic.dll はロジックのみを持つ)。
  これがホットリロードで状態の退避・復元を不要にしている設計の核 (ADR-002/003)。
- スクリプトは 2 レーン: C++ (`src\GameLogic\Scripts\*.cpp`、DLL ホットリロード) と
  C# (`src\Scripting\`、CoreCLR + Roslyn。エンジンとは `EngineAPI.h` のスロット表だけで会話)。
- tick 本体は `TickRunner.cpp` の `RunOneTick(TickServices&)` に抽出済み。フレームループ
  (`EngineLoop`) は固定 60Hz のアキュムレータでこれを呼ぶ。sim 状態の撮影/復元は
  `Replay\SimSnapshot.*` (`.rep` v4 は開始スナップショットを埋め込めるのでシーン非依存に再生できる)。
- **タイムトラベル** (`Replay\TimeTravel.*`) は「30 sim tick ごとのスナップショット + 全 tick の入力」の
  リング。シークは `EngineLoop` の `SeekTo` が Restore → `RunOneTick` で再シムし、記録ハッシュと
  照合して自己検証する。**再シム中の抑止は `TickServices::resim` 1 本**(出力レーンと C# だけ。
  `LoadScene`/`LoadGame` の読みは抑止しない — 抑止すると必ずハッシュが割れる)。
  スクラブ中は EngineLoop が tick を止める (止めないとポーズ tick がリングの未来を消す)。
- **起動経路が 2 つある**: プロジェクト起動 (`--project DIR`) と裸起動 (プロジェクトマネージャ)。
  **分岐は必ず `config.projectRoot` の有無で判定する**。シェーダは
  「プロジェクトの `assets\shaders` → エンジンリポジトリの `assets\shaders`」の 2 ルート解決
  (`PathUtil.h` の `FindEngineShaderDir`)。
- パス比較・AssetID のキーは必ず `NormalizePathKey()` を通す。**モデル由来のサブアセット ID は
  正規化した絶対パスのハッシュ**なので、それを含むシーン JSON はチェックアウト先に依存する =
  コミットできない。そういうシーンはコード (`DemoContent`) を正本にして毎回組み直し、生成物は
  gitignore する (`cache\parts_showcase.scene.json`、`assets\scenes\flow_*.scene.json` が実例)。

## 規約

- **コメントは日本語**。何をしているかより「なぜそうなっているか」「踏んだ罠」を書く
  (既存コードのコメント密度と口調に合わせる)。
- 4 スペース、関数の `{` は次行、型/関数 `PascalCase`、ローカル `camelCase`、定数 `kCamelCase`。
- テストは機能の隣に `*SelfTest.cpp` / `*SelfTest.h` で置き、`EditorMain.cpp` の連鎖に足す。
  ECS / シリアライズ / リプレイ / アセット / ローカライズ / レンダラ定数 / ホットリロードに
  触れたら必ずテストを足す。
- コミット件名はマイルストーン接頭辞つきの日本語 (`M52d: ...`)。ABI 変更は本文で明示する。
- **ファイルを消す前に絶対パス・目的・影響を提示して承認を取る** (`AGENTS.md`)。

## 環境の罠

- `Editor.exe` / `Runtime.exe` は **Windows サブシステム (GUI)** なので、PowerShell から
  `& Editor.exe` すると**待たずに戻り exit code も出力も取れない**。`cmd /c` を挟むこと
  (CI のステップが `shell: cmd` なのも同じ理由)。
- `tools\*.bat` は **CRLF で書く** (LF だと cmd.exe が行を途中で切る — CP932 環境では
  日本語 rem コメントの途中で割れて断片がコマンド実行される)。`.gitattributes` の
  `*.bat text eol=crlf` が checkout 時に強制する (blob は LF のまま)。リポジトリの blob は
  全て LF なので、`core.autocrlf=false` のチェックアウトでは bat 以外も含めディスクが LF に
  なり、この罠を踏む + CRLF を書くツール (gen_project_files 等) の diff が全域ノイズ化する
  — **このリポジトリは `core.autocrlf=true` 前提** (リポジトリローカルに設定済み)。
- bat で終了コードを見るときは `if errorlevel 1` を使わない。SEH で落ちた exit code
  (`0xC0000005` 等) は符号付きだと負なので「1 以上か」の判定が**偽になる**。
  `if !ERRORLEVEL! NEQ 0` の数値比較で書くこと (M52f)。
- `build\Common.props` の **XML コメントに `--` を書くと MSBuild が読めない** (`MSB4024`)。
  git のオプション名などを地の文に書くときは注意 (M52f で全ビルドを一度落とした)。
- `.gitattributes` は `*.png binary` を明示している — golden スクショが改行変換されると
  「ピクセル回帰が理由不明で赤い」形で出るため。
- `bin/`、`obj/`、`cache/`、`crash/`、`tests/actual/`、`assets/scripts/Generated/` は生成物
  (gitignore)。`tests\golden\*.png` だけが版管理された正解。
  `obj\generated\<Config>\MyeBuildInfo.h` は Engine プロジェクトのビルドが吐く
  (git ハッシュと構成。クラッシュ報告用)。
- 計画ファイルは `plans\*.md`、進捗の一次情報は `git log`。
