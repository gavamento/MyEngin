# MyEngine

[![CI](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml/badge.svg)](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml)

Windows 向けの **C++20 / DirectX 11 製自作 3D ゲームエンジン**です。ImGui エディタでシーン・アセットを編集し、C++ / C# のスクリプトでゲームを作り、エディタ UI を持たない Runtime として配布できます。

中心にあるのは、**ゲームを動かし、その内部状態を理解し、同じ条件から結果を再現できること**です。固定 Tick のシミュレーションと状態の記録・復元を共通基盤にして、リプレイ、タイムトラベル、未来の分岐比較、ネット対戦のロールバック、クラッシュ直前の再現へつなげています。描画、物理、音響伝播、制作ツールを含め、就職活動用ポートフォリオとして開発しています。

本 README は **2026-09-22 時点のソースツリー**に合わせた入口です。機能説明は実装の存在・接続を示すもので、全機能の実機試験や最新 CI の合格を保証するものではありません。仕様は [engine_spec.md](engine_spec.md)、作業規則は [AGENTS.md](AGENTS.md) を参照してください。

## 目次

- [ビルドと起動](#ビルドと起動)
- [プロジェクトを作ってゲームを動かす](#プロジェクトを作ってゲームを動かす)
- [主要機能](#主要機能)
- [プロジェクト専用シェーダーとポストエフェクト](#プロジェクト専用シェーダーとポストエフェクト)
- [エディタ操作](#エディタ操作)
- [ショーケースと CLI](#ショーケースと-cli)
- [配布パッケージ](#配布パッケージ)
- [検証と CI](#検証と-ci)
- [現在の制限](#現在の制限)
- [構成と設計方針](#構成と設計方針)
- [ドキュメント](#ドキュメント)

## ビルドと起動

### 必要な環境

| 対象 | 必要なもの |
|---|---|
| ネイティブ本体 | Windows 10 / 11、Visual Studio 2022 の「C++ によるデスクトップ開発」、MSVC v143、Windows SDK、x64 構成 |
| 描画 | DirectX 11 Feature Level 11_0。検証用に WARP も指定可能 |
| C# スクリプト（任意） | .NET 8 対応 SDK。ホストのターゲットは `net8.0` |
| エディタ内 Git（任意） | Git、rustup の stable ツールチェーン（サービスのビルド時） |
| 検証スクリプト | PowerShell 7（`pwsh`）。Replay の並列実行でも使用 |

C++ の外部依存ソースは [external/](external/) に同梱しています。依存一覧とライセンスは [external/VERSIONS.md](external/VERSIONS.md) を参照してください。C# の NuGet パッケージ、Rust の依存クレート、Deep-Modal の学習環境は別途取得が必要です。

### Visual Studio

1. [MyEngine.sln](MyEngine.sln) を開きます。
2. `Debug | x64` を選択し、`Editor` をスタートアッププロジェクトにします。
3. ビルドして F5 で起動します。通常起動ではプロジェクト管理画面から作成・選択できます。

新しい Visual Studio を使う場合も、プロジェクトが指定する **v143** ツールセットをインストールしてください。共通設定は [build/Common.props](build/Common.props) にあり、C++20、`/utf-8`、`/fp:precise` を使用します。CRT は Debug が `/MTd`、Release が `/MT` です。

### コマンドライン

以降の例は、特記がなければ **リポジトリルートを作業ディレクトリとした PowerShell** で実行します。MSBuild の例には Visual Studio の Developer PowerShell を使用してください。

```powershell
MSBuild.exe MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
.\bin\x64\Debug\Editor.exe

# 配布・性能確認用
MSBuild.exe MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
```

| 成果物 | 役割 |
|---|---|
| `Engine.lib` | Platform / Core / Renderer / Engine の共通機能 |
| `Editor.exe` | シーンとアセットの制作環境 |
| `Runtime.exe` | エディタ UI を持たない実行ホスト |
| `GameLogic.dll` | ホットリロード対象の C++ ゲームコード |

出力先は `bin/x64/Debug/` または `bin/x64/Release/`、中間生成物は `obj/` です。

### 任意機能の追加ビルド

C# ホストと Git サービスは `MyEngine.sln` の外にあります。未導入でもエディタ本体は起動し、該当機能が利用不可になります。

```powershell
# C# ホストと Roslyn。使用する構成ごとにビルド
.\tools\build_managed.bat Debug
.\tools\build_managed.bat Release

# Rust 製 Git サービス。1 回のビルドで Debug / Release 両方へ配置
.\tools\build_collab.bat
```

C# は `MyeScripting.dll` とその依存ファイル、Git は `MyeCollab.dll` / `MyeCollabCli.exe` が本体の隣に出力されます。Git 連携は Git 管理された外部プロジェクトを `--project` で開いて使用します。リモート認証は事前にターミナル側で済ませてください。バックグラウンド取得は認証ダイアログを表示しません。

## プロジェクトを作ってゲームを動かす

エンジン本体と制作するゲームを分けて管理できます。CLI で作る場合は、未作成または空のディレクトリを指定します。

```powershell
# パスは自分の制作先へ置き換える
.\bin\x64\Debug\Editor.exe --create-project "C:\MyGames\FirstGame" --template demo
.\bin\x64\Debug\Editor.exe --project "C:\MyGames\FirstGame"
```

`--template` は `empty` / `demo` に対応し、省略時は `empty` です。プロジェクトには `project.mye.json`、`assets/`、ローカル設定用の `.mye/` などが作成されます。テンプレート生成だけでは Git リポジトリは初期化しません。

基本の制作手順は次のとおりです。

1. Hierarchy でオブジェクトを配置し、Inspector からコンポーネントを追加・調整します。
2. `assets/` にモデル、テクスチャ、音、スクリプトなどを配置します。参照を保つため、アセット本体と `.meta` は一緒に管理します。
3. C++ / C# スクリプトをコンポーネントとして割り当てます。C++ は Rebuild Scripts でビルドし、登録済みフィールドを Inspector から編集します。
4. シーンを保存し、Play で動作を確認します。Play 中の編集は Stop 時に開始前の状態へ戻ります。
5. Runtime でも同じプロジェクトを起動し、ゲーム画面・入力・音・シーン遷移を確認します。

```powershell
.\bin\x64\Debug\Runtime.exe --project "C:\MyGames\FirstGame"
```

外部プロジェクトの C++ DLL はプロジェクトの `cache/GameLogic.dll` に置かれます。エンジンの `bin/` にある DLL と取り違えないでください。`Runtime --project` は開発実行として扱われ、配布先では生成したパッケージの `Runtime.exe` を起動します。

既存の制作例・課題は [ドッグフーディング記録](docs/dogfooding.md)、音のステルスゲームに向けた対応は [三校実装状況](docs/sanko-implementation-status.md) にまとめています。これらの記録は記載時点のもので、同じゲームの完成版や配布物が本リポジトリに含まれることを意味しません。

## 主要機能

### オブジェクト・スクリプト・アセット

| 機能 | 内容 |
|---|---|
| ハイブリッド ECS | 利用側は GameObject / コンポーネント、内部はアーキタイプ別 SoA。世代付き EntityID と構造変更バッファを使用 |
| 階層・有効状態 | LocalTransform から WorldMatrix を計算。親の Active を子へ伝播 |
| リフレクション | 登録フィールドを Inspector、JSON、DLL 状態移行、ワールドハッシュで利用 |
| C++ ホットリロード | GameLogic.dll と PDB をコピーして差し替え、名前と型が一致する登録フィールドを移行 |
| C# スクリプト | .NET ホストと Roslyn による別の記述経路。決定論検証とは分離 |
| データスキーマ | `.component.schema.json` からコンポーネントを定義し、C++ / C# 向けアクセスコードを生成 |
| シーン・構成アセット | `.scene.json`、`.actor.json` / `.prefab.json`、インスタンス化、Apply / Revert、フィールド・コンポーネントの上書き |
| 部位・ソケット | Part の名前・タグ検索、骨への追従、部位の箱・球による判定 |
| アセット管理 | GUID と `.meta`、モデルの GUID 由来サブアセット ID、クック済みキャッシュ、変更監視 |
| ゲーム基盤 | 入力アクション、ゲームパッド、シーン遷移、セーブ、ローカル複数プレイヤー |

主なアセット形式は glTF / GLB / FBX / OBJ、PNG / JPEG / TGA / BMP / DDS、WAV / OGG です。マテリアル、アニメーション、音設定、物理材料、地形などには専用 JSON 資産を使います。

**未登録コンポーネントの JSON は保持して再保存する実装になっています。** 型が未登録のまま実行できるわけではありませんが、旧 README にあった「未登録なら保存で消える」という説明は現状には当てはまりません。

スクリプト API の正本は [src/Shared/EngineAPI.h](src/Shared/EngineAPI.h) と [src/Shared/ScriptAPI.h](src/Shared/ScriptAPI.h) です。現行の `MYE_API_VERSION` は **21**。ABI を更新した場合は本体、ゲーム DLL、言語間ミラーの整合が必要です。

### 描画・演出

- **Forward / Deferred** を実行時に切替。PBR、IBL、平行光・点光源・スポット光、影、透明描画に対応。
- **DirectX 11 Compute Shader による二次光線追跡**。自前 BVH で拡散 GI・平行光の影・反射を計算し、SVGF でデノイズ。DXR は使用しません。
- **ReSTIR 反射**。時空間のサンプル再利用と、映る物体の `ReflectionClass` に応じた再利用制御。既定無効。
- **SSR、反射プローブ、デカール、TAA、Bloom、トーンマップ、フォグ**。SSR・TAA などには Deferred の条件があります。
- **ボリュメトリックフォグ**。フロクセルに光と密度を格納し、メッシュ・地形・空・パーティクル・VFX へ合成。
- **地形・アニメーション**。ハイトフィールド、編集、LOD / スカート、スケルタルアニメーション、ブレンド、Animator Controller。
- **CPU / GPU パーティクル**。CPU は SoA + SSE、GPU は Compute + indirect draw。切替・比較モードを持ちます。
- **Sprite / Trail / TextMesh / Effect** による演出と、プロジェクト専用のポスト・Compute 処理。
- **水面波**。`WaterWaveComponent` の Gerstner 波による描画と、動的波高に連動した浮力。

光線追跡の構成・過去の計測条件は [ADR-009](docs/adr/ADR-009-hybrid-path-tracing.md)、反射の再利用は [ADR-016](docs/adr/ADR-016-restir-reflection.md) を参照してください。過去の GPU 時間を、現在のシーンや別 GPU の性能保証として扱わないでください。

### 物理・音響・AI

| 分野 | 内容 |
|---|---|
| 自作剛体ソルバ | 接触の蓄積インパルス、サブステップ、静動摩擦、転がり抵抗、スリープ、アイランド、CCD |
| 形状・クエリ | 単純形状、メッシュ、凸包、複合コライダー、地形、Raycast / SphereCast / Overlap |
| 力と機構 | 空力、翼面、マグヌス、浮力、ばね、関節、リミット、モータ、破断、ラグドール、車両 |
| XPBD | ロープと剛体への双方向アタッチ。変形体全体としては部分実装 |
| 3D 音声 | 音源定位、遮蔽、回折、ミキサー、残響 |
| 音響伝播 | 整数距離場による波面を、可視化・敵の聴覚・経路探索・プレイヤーに届く音へ利用 |
| 敵 AI | 音・光のセンサー、状態機械、グリッド上の経路探索 |
| Deep-Modal | 形状・材質・接触位置・衝突の強さに応じたモーダル衝突音の合成 |

Deep-Modal は、学習済み `.dmnet` からメッシュ単位の特徴を `.msfm` に焼き、`ModalSound` を持つ物体の衝突から音を合成します。録音 SE の一律再生とは異なり、形状や励起条件が音色・音量へ影響します。モデルの汎化やマスクによって無音になるメッシュがあり、資産ごとの確認が必要です。学習環境と操作は [tools/deepmodal/README.md](tools/deepmodal/README.md)、設計は [ADR-020](docs/adr/ADR-020-deep-modal.md) を参照してください。

### ゲーム内 UI

基準 1920×1080 のキャンバス単位でレイアウトし、画面に応じて一様に拡縮します。アンカー、描画と共通のヒット判定、マウス・ゲームパッドのフォーカス操作に対応します。

ボタンに加えて Toggle / Slider などのウィジェットを持ち、クリックやフォーカスなどの入力状態を Tick 内で確定してスクリプトへ渡します。UI 操作を含む Replay デモは `--ui-demo --ui-demo-input` です。

### 再現・比較・障害解析

- **Replay** — 入力と Tick ごとのワールドハッシュを記録し、Debug / Release 間で比較。差異をエンティティ・フィールド単位へ掘り下げます。
- **スナップショット / タイムトラベル** — 過去の状態を復元し、通常実行と共通の `RunOneTick` で再シミュレーション。
- **What-if 分岐** — 過去へ戻って入力や状態を変え、元の未来を別レーンとして保持。Timeline の差分と SceneView のゴーストで比較。
- **クラッシュ記録** — minidump、`crash.txt`、記録可能な範囲の `crash.rep` を出力し、障害直前のシミュレーション状態の再現に利用。
- **2 人 P2P** — UDP、遅延ロックステップ、予測ロールバック。接続条件を照合し、状態不一致時は診断用バンドルを出力。

対象は明示的に管理・記録されたシミュレーション状態です。GPU 出力、C# の任意状態、外部 I/O、クラッシュの原因そのものまで自動的に再現する仕組みではありません。

## プロジェクト専用シェーダーとポストエフェクト

現行では、エンジン組込みシェーダーの上書きに加え、ゲーム側で専用ポストと Compute Shader を追加できます。

| 入口 | 用途 |
|---|---|
| `assets/shaders/` | 同名のエンジン組込みシェーダーを上書き。指定がなければエンジン側を使用 |
| `*.post.hlsl` | プロジェクトの `assets/` 配下に置くポストエフェクト |
| `*.cs.hlsl` | プロジェクトの `assets/` 配下に置く Compute Shader |
| `*.fxstack.json` | ポスト / Compute の実行列とプロパティ設定 |
| `CameraPostFx.fxStack` | カメラに使用するスタックを割り当てる AssetRef |

Asset Browser に作成用の入口があり、シェーダー変更は include 依存を含めて監視します。コンパイル失敗時には最後に有効だったプログラムを維持します。プロジェクトシェーダーは短名で索引されるため、`assets/` 内で名前が重複しないようにします。

**専用スタックの見た目は Game ビュー / Runtime で確認します。** Scene ビューのカメラ上書き経路ではユーザーポストとスタック内 Compute を実行しません。

Tex2D の既定名には制限があります。現在 `white` は白テクスチャ、`gray` / `black` / `bump` は警告付きで白へフォールバックします。[既定テクスチャの説明](docs/project-shaders-tex2d-defaults.md) と、実装の [ShaderManager](src/Engine/Renderer/ShaderManager.h)、[FxStackAsset](src/Engine/Renderer/FxStackAsset.h)、[ComputeAbiRunner](src/Engine/Renderer/ComputeAbiRunner.h) を参照してください。

## エディタ操作

| 画面・操作 | 役割 |
|---|---|
| Hierarchy / Inspector | 親子構造、選択、コンポーネント追加、登録フィールドの編集 |
| Scene ビュー | 右ドラッグ + WASDQE でカメラ移動、Shift で加速。配置・ギズモ・デバッグ表示 |
| Game ビュー | シーンのカメラから描画。ゲーム操作は割り当てたスクリプトと入力設定に従う |
| Play / Pause / Step | 実行、一時停止、Tick 単位の進行。Stop で Play 開始前へ復元 |
| Asset Browser | アセットの検索、作成、インポート、移動、参照管理 |
| Search | エンティティ・アセットの検索と、選択対象への参照確認 |
| Timeline | シーク、分岐切替、分岐点・乖離 Tick・入力区間の表示 |
| Animation / Animator Controller | クリップと状態遷移の編集 |
| Particle Settings / Audio Mixer / SoundGen | 粒子、音量系統、音生成の調整 |
| Profiler / Console | 処理時間、ログ、エラーの確認 |
| Project Settings / Build Settings | プロジェクト設定と配布ビルド |
| Source Control | 変更一覧、stage / commit / push、fetch / pull、ブランチ、競合への対応 |

基本ショートカットは `Ctrl+S`（保存）、`Ctrl+Z` / `Ctrl+Y`（Undo / Redo）、`Ctrl+D`（複製）、`F`（選択へフォーカス）、`F2`（名前変更）、`Ctrl+C/X/V`（コピー / 切り取り / 貼り付け）、`Delete`（削除）です。操作対象や編集中の状態によって有効範囲が変わります。

UI は日本語・英語に対応し、メニューから切り替えられます。CLI で固定する場合は `--lang ja` / `--lang en` を指定します。

## ショーケースと CLI

### まず試す

```powershell
# 描画ショーケース
.\bin\x64\Release\Runtime.exe --render-demo --deferred

# 物理と関節・機構
.\bin\x64\Debug\Editor.exe --physics-demo --autoplay
.\bin\x64\Debug\Editor.exe --joint-demo --autoplay

# 音響 / UI
.\bin\x64\Debug\Editor.exe --acoustic-demo --autoplay
.\bin\x64\Debug\Editor.exe --ui-demo --autoplay
```

| フラグ | 内容 |
|---|---|
| `--render-demo` | 光源、反射床、フォグなど |
| `--rt-demo` | コーネル箱。GI・影・反射は下記のフラグで指定 |
| `--terrain-demo` | 地形。`--terrain-lod N` / `--terrain-skirt N` で調整 |
| `--physics-demo` / `--joint-demo` | 物理、関節、ラグドール、車両 |
| `--fog-demo` / `--particle-demo` | フォグ、CPU / GPU 粒子 |
| `--acoustic-demo` | 波面と聴覚・AI。波の可視化は Scene ビューの音響表示で確認 |
| `--modal-demo` | 材質の異なる物体の合成衝突音 |
| `--ui-demo` | UI ウィジェット |
| `--local-demo` / `--net-demo` | ローカル複数プレイヤー / ネット対戦 |
| `--parts-demo` / `--flow-demo` | 部位追従 / シーン遷移・セーブ。Editor 専用 |

```powershell
# Compute Shader による GI・影・反射
.\bin\x64\Release\Runtime.exe --rt-demo --deferred --rt-gi --rt-shadow --rt-refl --rt-anim-seed

# ReSTIR 反射 / ボリュメトリックフォグ
.\bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-restir
.\bin\x64\Release\Runtime.exe --fog-demo --froxel --particle-backend gpu

# 明示したシーンを開く（実在するパスへ置き換える）
.\bin\x64\Debug\Editor.exe --scene "assets\scenes\main.scene.json"

# 指定 Tick の画像を取得
.\bin\x64\Release\Runtime.exe --render-demo --screenshot "cache\render.png" --shot-frame 120 --frames 121
```

### 診断用の例

```powershell
# 音響経路 / モーダル合成のログ。聴感確認は別途必要
.\bin\x64\Debug\Editor.exe --acoustic-demo --acoustic-audio-log 300 --synth-input
.\bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input

# モデルを .msfm へベイク
.\bin\x64\Release\Editor.exe --modal-bake --project "C:\MyGames\FirstGame" --modal-backend cpu

# 2 人対戦（ホストと参加側を別プロセスで起動）
.\bin\x64\Release\Runtime.exe --net-demo --net-host 7777
.\bin\x64\Release\Runtime.exe --net-demo --net-join 127.0.0.1:7777 --net-delay 3
```

`--warp` はソフトウェア描画を指定します。`--no-audio` は音声初期化を無効にするため、音の確認には使用しません。

CLI の正本は [EngineCli.cpp](src/Engine/Engine/EngineCli.cpp)（共通フラグ）、[EditorMain.cpp](src/Editor/EditorMain.cpp)、[RuntimeMain.cpp](src/Runtime/RuntimeMain.cpp)、[ShowcaseScenes.cpp](src/Engine/Engine/ShowcaseScenes.cpp)（デモ一覧）です。デモや検証は `cache/` やシーンなどの生成物を書き出す場合があります。

## 配布パッケージ

Release の本体をビルドし、外部プロジェクトのスクリプトをビルド・保存した後、Build Settings または CLI から生成します。出力先には新しい専用フォルダを指定してください。

```powershell
.\bin\x64\Release\Editor.exe --project "C:\MyGames\FirstGame" --package "C:\MyGames\FirstGame\dist\release-01" --package-boot main.scene.json --package-dds --package-zip
```

`--package-boot` は `assets/scenes/` 内のシーン名です。`--package-dds` はテクスチャの DDS クック、`--package-zip` は ZIP 作成を追加します。実行元 Editor と同じ出力ディレクトリの Runtime を使うため、配布用は Release の Editor で実行してください。

パイプラインはスクリプトビルド、アセットのクック、コピーなどを処理します。プロジェクトの `cache/GameLogic.dll`、アセット、エンジン組込みシェーダーを集め、選んだ起動シーンを配布先の `assets/scenes/main.scene.json` に配置します。C# を使う場合はホストと .NET 実行依存も確認します。Git サービスや `.git` は配布対象に含めません。

生成後は **出力先の `Runtime.exe`** を起動して、起動シーン・入力・音・フォント・シーン遷移を確認してください。パッケージ生成成功と、配布先でゲームを遊び通せることは別の確認です。処理の正本は [BuildSettingsWindow.cpp](src/Editor/Windows/BuildSettingsWindow.cpp) です。

## 検証と CI

### 基本確認

```powershell
pwsh -NoProfile -File .\tools\check_rules.ps1
.\bin\x64\Debug\Editor.exe --selftest
.\bin\x64\Release\Editor.exe --selftest
```

SelfTest は ECS、シリアライズ、物理、アセット、スクリプト API、エディタ機能などの回帰テストを集めた入口です。テストデータの相対パスを解決できるよう、リポジトリルートから実行してください。任意サービスに依存する項目のスキップは、合格とは区別します。

### Replay と個別診断

```powershell
# 両構成のビルドを含む検証一式
.\tools\replay_verify.bat

# 個別の記録 / 照合
.\bin\x64\Debug\Editor.exe --replay-record "cache\sample.rep" --replay-ticks 600
.\bin\x64\Release\Editor.exe --replay-verify "cache\sample.rep"

# 復元 / 分岐の検証
.\bin\x64\Debug\Editor.exe --timetravel-selftest 400
.\bin\x64\Debug\Editor.exe --whatif-selftest 400
```

現行の `replay_verify.bat` は、既定デモ・部位・ゲームフロー・ローカル 2P・物理・関節・音響・UI の **8 シーン**を検証します。Debug 記録、Debug / Release 照合、スナップショット往復、タイムトラベル、What-if、静的規則検査を含みます。ログは `cache/replay_logs/` に出力します。

このスクリプトはクック済みキャッシュや過去の検証ログを消して再生成します。保存したい生成物がある場合は実行前に退避してください。

| コマンド | 確認する内容 |
|---|---|
| `tools\shot_verify.bat` | Release Runtime で撮影し、`tests/golden/` とピクセル比較。差分は `tests/actual/` |
| `tools\crash_verify.bat` | 意図的なクラッシュを起こし、バンドルと Replay の再現性を検査 |
| `tools\net_verify.bat` | 複数プロセスでネット対戦、記録の一致、desync 検出を検査 |
| `tools\collab_verify.bat` | Rust サービスに Git 操作シナリオを流し、期待出力と比較 |
| `tools\gen_project_files.ps1` | ソース追加・移動後に `.vcxproj` / `.filters` の一覧を更新 |

画像検証は先に Release をビルドします。`shot_verify.bat --update` は比較基準そのものを更新する操作です。通常の検証では付けず、意図した見た目の変更を確認してから使います。

### CI の範囲

[.github/workflows/ci.yml](.github/workflows/ci.yml) は push / pull request / 手動実行を入口に、Windows runner で C#・Rust のビルド、Rust テスト、Git 連携検証、Replay、両構成の SelfTest、画像回帰、パッケージ作成と内容検査を呼びます。

| 環境変数 | CI での用途 |
|---|---|
| `MYE_EXTRA_ARGS` | `--warp --no-audio` を検証実行へ渡す |
| `MYE_MSBUILD_ARGS` | `/p:MyeWarnAsError=true` で C++ の警告をエラーにする |
| `MYE_DOTNET_ARGS` | `/p:TreatWarningsAsErrors=true` で C# の警告をエラーにする |
| `MYE_SHOT_SKIP_*` | 機種差・描画経路の条件に応じて一部の画像検証を除外 |
| `MYE_COLLAB_REQUIRED` | Git サービス不足による SelfTest のスキップを失敗扱いにする |

検証件数や除外対象は各スクリプトが正本です。CI の画像検査は実 GPU・実音声・実ゲームパッド・日本語フォント・ゲーム全編の操作試験の代替にはなりません。

## 現在の制限

- **C# は Replay 記録・検証、ネット対戦、再シミュレーションの実行対象外**です。決定性が必要なゲーム状態は C++ 側の対応範囲で管理します。
- **XPBD はロープまでの部分実装**です。布、ソフトボディ、粒子と世界の衝突は未実装。破砕や汎用の熱・流体なども計画と実装を区別してください。
- **CharacterController と Rigidbody の相互作用には制限**があります。ジャンプの接地判定は呼び出し側で行います。
- **GPU 描画・GPU 粒子・音声出力はワールドハッシュ一致だけでは検証できません**。必要な経路を実機で確認します。
- **Actor / Prefab の構造上書きは子エンティティ自体の追加・削除を追跡する範囲ではありません**。
- **Source Control は Editor 専用**です。PR、レビュー、LFS、sparse checkout、シーンの 3-way マージ、認証 UI は v1 の対象外です。
- **プロジェクトポストの Tex2D 既定色、Deep-Modal の資産ごとの発音**には前述の制限があります。

詳細な仕様と残作業は [engine_spec.md](engine_spec.md) と各設計資料へ分けて記録しています。過去の課題台帳の件数を、現在の未解決件数として読み替えないでください。

## 構成と設計方針

```text
MyEngine.sln
build/           Visual Studio プロジェクトと共通設定
src/
  Editor/        ImGui エディタ、制作・配布・Git ツール
  Runtime/       配布用の実行ホスト
  GameLogic/     ホットリロード対象の C++ DLL
  Shared/        C ABI と POD による DLL 境界
  Scripting/     .NET / C# スクリプトホスト
  Engine/
    Platform/    Win32、入力、時刻、DLL、通信
    Core/        ECS、リフレクション、RNG、ジョブ、ログ
    Renderer/    DirectX 11、描画パス、シェーダー、GPU リソース
    Engine/      シーン、Tick、物理、音、AI、Replay、アセット
assets/          シェーダー、デモ・テスト用アセット
external/        同梱する外部ライブラリ
tools/           ビルド、検証、データ生成、Git サービス
tests/           画像回帰の基準・出力など
docs/            機能説明、ADR、検証資料、履歴
plans/           個別機能の計画・作業記録
```

設計上の必須条件は、上位層から下位層への依存、Renderer 外へ生の D3D 型を露出させないこと、`src/Shared/` を C ABI + POD に限定することです。ソース管理機能は Editor に置き、Engine / Runtime / GameLogic / Shared から依存させません。

シミュレーションは固定 **60 Hz Tick**、描画は Frame として分けます。PCG32 の seed、処理順序、構造変更の適用点、状態の所有権を管理し、Debug / Release で状態を変える条件分岐を避けます。通常 Tick・巻き戻し・ネットの再実行は [TickRunner.cpp](src/Engine/Engine/TickRunner.cpp) の共通経路を使います。

開発時の判断・コーディング規約・変更に応じた検証は [AGENTS.md](AGENTS.md) に集約しています。設計方針を、すべての既存コードが既に満たしているという監査結果と混同しないでください。

## ドキュメント

| 資料 | 読む目的 |
|---|---|
| [engine_spec.md](engine_spec.md) | 機能仕様、技術制約、詳細な検証方針 |
| [AGENTS.md](AGENTS.md) | 共通作業規則、実装・検証・報告の基準 |
| [全機能ガイド](docs/engine-feature-guide.md) | 制作者向けの機能説明と実装の入口（記載時点の調査） |
| [ADR 一覧](docs/adr/) | 設計判断とトレードオフ |
| [ドッグフーディング記録](docs/dogfooding.md) | 外部プロジェクトで遭遇した課題と当時の対応 |
| [三校実装状況](docs/sanko-implementation-status.md) | 音のステルスゲームに向けた対応と検証記録 |
| [手動テスト項目](docs/test_checklists.md) | 実操作による確認手順 |
| [デモ台本](docs/demo_script.md) | 紹介時の操作・説明の参考 |
| [プロジェクトポストの既定テクスチャ](docs/project-shaders-tex2d-defaults.md) | Tex2D の現行フォールバック |
| [Deep-Modal](tools/deepmodal/README.md) | データ生成、学習、モデル配置、音の評価 |
| [実装履歴](docs/history/README.md) | 分野別の変更経緯 |
| [plans/](plans/) | 個別機能の計画。実装済みの証拠とは分けて参照 |
| [外部依存とライセンス](external/VERSIONS.md) | 同梱ライブラリの版と権利表示 |
