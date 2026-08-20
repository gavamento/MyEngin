# M52: 決定論の再利用 — 診断 / CI / タイムトラベル / クラッシュ再現 / ロールバック・ネットコード

## Context

M51 完遂 (`a0d5081` → 後続 `760ac83`、ABI v12、kEngineVersion 0.66) の次期マイルストーン。

このエンジンは既に **「600 tick のワールドハッシュが Debug/Release でビット一致する」を 3 ペアで毎回証明している**
(`tools\replay_verify.bat`)。M52 はその決定論という**既に払い終わったコスト**を、これまで使ってこなかった
5 方向へ転用する:

| 転用先 | 現状のギャップ |
|---|---|
| ロールバック・ネットコード | ネットワークコードが 1 行も無い。決定論 lockstep が成立する engine は学生作品にまず無い |
| タイムトラベル・デバッグ | `PlayModeController` の JSON スナップショットは Play 開始の 1 枚だけ。過去 tick へ戻れない |
| クラッシュ時 .rep 自動添付 | 例外ハンドラが**存在しない** (`SetUnhandledExceptionFilter` の呼び出し 0 件)。落ちたら手掛かりはイベントログのみ |
| ハッシュ分岐の詳細診断 | `HashWorldDetailed` は**エンティティ単位まで**で、しかも失敗側の値しか出ない (`EngineLoop.cpp:706-715`)。「どのフィールドが最初に割れたか」は今も手作業 |
| CI / スクショ回帰 | remote (github.com/gavamento/MyEngin) があるのに CI が無い。`GraphicsDevice::Init` は `D3D_DRIVER_TYPE_HARDWARE` 固定 (`GraphicsDevice.cpp:22`) で GPU 無し runner では device 生成すら通らない |

**この 5 つのうち 3 つ (ネット・タイムトラベル・クラッシュ) は同じ 1 個の部品を必要とする**:
「ある tick の sim 状態を丸ごと保存し、後でビット同一に復元し、そこから同じ入力で回すと同じハッシュ列になる」
スナップショット基盤。M52 はこれを 1 サブに切り出して 3 者で共有する。

---

## 実装開始時の手順

1. 本計画をリポジトリ `plans\wistful-tumbling-lantern.md` へコピーしてコミット対象にする (マイルストーン運用の定型)。
2. 1 サブ = 1 コミット (`M52a:` 形式) = 1 セッション + /clear。進捗の一次情報は git log、進捗表には計画外の事実・罠・申し送りのみ書く。

## 再開手順 (セッション跨ぎ用)

1. `git log --oneline -5` で最後に完了した M52x を確認。
2. 本ファイルの該当サブの節を読んで着手。
3. 全サブ共通検証: Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS → `tools\replay_verify.bat` PASS → `tools\check_rules.ps1` 0 error。ソース追加時は `pwsh tools\gen_project_files.ps1`。M52i は `tools\build_managed.bat` 両構成も。
4. 新規 UI 文字列は `LocalizationTable.inl` に `MYE_STR(id, en, ja)` (規則 10)。
5. **M52d 以降は ReplayFile v4** — それ以前の手元 .rep は verify 不可 (replay_verify は毎回録り直すので無風)。**WorldHash 構成は M52 を通して不変** (= 決定論の意味論を変えずに、その決定論を使う側だけを足すマイルストーン)。

## 進捗表

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M52a ハッシュ差分診断 | 完了 | `55fe1ce` | 下記「M52a の申し送り」参照 |
| M52b CI + WARP | 完了 | (本コミット) | 下記「M52b の申し送り」参照 |
| M52c スクショ回帰 | 完了 | (本コミット) | 下記「M52c の申し送り」参照 |
| M52d SimSnapshot 基盤 | 未着手 | | |
| M52e タイムトラベル | 未着手 | | |
| M52f クラッシュバンドル | 未着手 | | |
| M52g マルチ入力レーン | 未着手 | | |
| M52h UDP + ロックステップ | 未着手 | | |
| M52i ロールバック + ABI v13 | 未着手 | | |

### M52a の申し送り (計画外の事実・罠)

1. **「3 出口の一致」を守る検証は selftest では足りない。** 走査を 1 実装へ畳む作業は
   「規約は同じだが畳み込み順序が 1 か所ずれる」で静かに壊れる。selftest の小さな世界では
   その順序差が出ないことがある。**リファクタ前に実データの .rep を録っておき、リファクタ後に
   それを verify する**のが唯一効く検証だった (demo + parts の 2 ペアで実施、600 tick 一致)。
   同じ手は M52d の tick 本体抽出でもそのまま使える (むしろ必須)。
2. **ロールアップ行を差分件数から外すのが設計の要。** 1 フィールド変異は「そのフィールドの行」と
   「そのエンティティの `#entity` 行」の 2 行を割る。`#` 始まりのフィールド (`#nameHash` /
   `#entity` / `#total`) を `rollupDiffs` へ分離して初めて「1 変異 = valueDiffs 1」が成立する。
   副産物として `rollupDiffs > 0 かつ valueDiffs == 0` は「ダンプがハッシュ対象を取りこぼして
   いる」という診断自身のバグ検出になる (自己診断としてログに出す)。
3. **畳み込み列は「行の同一性」には使えない。** 1 つ割れると以降が全部ずれるので、
   件数は必ず値列で数え、畳み込み列は `firstFoldLine` (最初の乖離点) の特定だけに使う。
4. **パーティクルの SoA 配列だけは生 hex にしない。** 要素数が数千になりうるので
   `n=<件数>#<配列サブハッシュ>` に畳む (エミッタと配列名までは特定できる)。
   コンポーネントのフィールドは String256 (512 文字) でも生 hex のまま — M48i の罠を
   見落とさないため。
5. **`:diagnose` はコード変異では実証できない。** 期待側を「同じコマンドで録り直す」方式なので、
   コードを変異させると record 側も verify 側も一緒に変異して差が出ない。実証は
   **「古い golden を新ビルドで verify」** で行うこと (Rotator の quat z に tick 200 以降だけ
   +0.001 を入れた temp ビルドで実走 → tick 200 で MISMATCH、7529 行中
   `3:1 "Spinner" LocalTransform.rotation` の 1 行だけを指した)。
   `:diagnose` 本体は同じ本文を一時 bat へ写して単体実走で確認済み。
6. **Git Bash から `cmd /c` は使えない** (MSYS が `/c` を `C:\` へパス変換してしまい、
   対話 cmd が起動して固まる)。GUI サブシステムの exe を待つのは
   **PowerShell から `cmd /c "..."`** の一手に統一する。
7. M52d 以降で ReplayFile を v4 にするとき、`--hash-dump` の出力形式 (`#mye-hash-dump v1`) は
   .rep とは独立に版を持つ。ダンプ形式を変えるときはこのヘッダ行の版を上げること。

### M52b の申し送り (計画外の事実・罠)

1. **`/p:TreatWarningAsError=true` は C++ に効かない。** 計画に書いてあった通りに CI へ入れると
   「警告 0 を機械化した」という**嘘の緑**になる。C++ の `TreatWarningAsError` は
   ClCompile の**項目メタデータ**で、MSBuild のグローバルプロパティは誰にも読まれず素通りする
   (実測: C4189 を仕込んでも warning のまま exit 0)。橋渡しが要る:
   `Common.props` の ItemDefinitionGroup に
   `<TreatWarningAsError Condition="'$(MyeWarnAsError)'=='true'">true</TreatWarningAsError>`
   を置き、CI は `/p:MyeWarnAsError=true` を渡す。**既定 off なのでローカル開発は止まらない**
   (計画が Common.props を触らないと決めた動機はここで満たされる)。
   `external\` の第三者ソース 8 本は `Engine.vcxproj` 側で個別に `false` へ落とす
   (手書き ItemGroup なので `gen_project_files.ps1` は上書きしない)。
   検証は「仕込んで赤 → 外して全体 Rebuild が緑」の両方向で実施。
2. **WARP のハッシュは実 GPU と一致する — 実測済み。** 計画では「sim は CPU 専用だから同一のはず」
   という推論だったが、**WARP で録った 3 本の .rep を RTX 3060 でそのまま `--replay-verify` して
   600 tick 全一致**することを確認した (逆向きも同様)。つまり CI と開発機の .rep は相互運用でき、
   CI 失敗時に artifact の .rep を持ち帰ってローカルで `--hash-diff` にかけられる。
3. **WARP でも所要時間はほとんど増えない。** tick はアキュムレータで追いつくので、
   描画が遅いフレームほど 1 フレームあたりの tick 数が増えるだけ (600 tick が 10 frames で消化された)。
   sim の回数は同じ = リプレイ検証の壁時計は描画性能にほぼ依存しない。
   ただし**スクショ回帰 (M52c) は逆** — あちらはフレーム番号基準なので WARP の遅さが直撃する。
4. **`--package` は成否を終了コードに載せていなかった** (ログに PASS/FAIL を出すだけ)。
   CI が機械判定できないので `EditorApp::packageExitCode` を足して `EditorMain` が返すようにした
   (エンジン自体の失敗コードが優先)。CI 側は exit code に加えて dist の中身も見る。
5. **push には `workflow` スコープが要る。** `gh auth login` の既定トークン (repo/gist/read:org) では
   `.github\workflows\*` を含む push が remote 側で拒否される。
   `gh auth refresh -h github.com -s workflow` を**人間が対話で**一度実行する必要がある。
6. runner image `windows-2022` は 2026-08 時点で現役 (deprecated ではない)。
   固定した理由は再現性 — `windows-latest` は Windows Server 2025 を指す。
7. CI のステップは `shell: cmd` を使う。GUI サブシステムの exe を待つには cmd 経由が要る
   (M52a の罠 6 と同根。pwsh から `& Editor.exe` は待たない)。
8. Hub (`ProjectManager`) は `GraphicsDevice::Init()` の既定引数を使うので、
   `--warp` は届かないが**自動フォールバック側に乗る** (GPU 無し環境でも Hub は起動する)。

### M52c の申し送り (計画外の事実・罠)

1. **計画の撮影条件 (`--shot-frame 3 --frames 8`) は決定的ではなかった — 実測で崩れた。**
   1 フレームに何 tick 回るかは `accumulator += dt` の **実時間**で決まる (`EngineLoop.cpp:538`)。
   WARP は 1 フレーム数十 ms かかるので、同じコマンドを 2 回叩くと撮影フレームまでの
   tick 数が変わり **PNG がバイト不一致**になった (計測: 同条件 2 回で 273669B と 273612B、
   どちらも「8 frames / 22 ticks」なのに**フレームごとの配分**が違う)。
   対処は `EngineConfig::shotRealtime` の裏返しとして入れた**決定的撮影モード**:
   `--screenshot` 指定 (連番 `--shot-every` を除く) で自動 on になり、
   ①フレームの `dt` を `kFixedDt` に固定する (accumulator が毎フレームちょうど 1 tick
   消費して 0 に戻る = **frame 番号 == tick 番号**。同じ double を足して引くので誤差ゼロ)、
   ②`TextureLibrary::WaitForAsyncLoads()` で非同期デコードを撮影前に待ち切る
   (M23 の非同期テクスチャは「間に合ったかどうか」が実時間依存 = もう 1 つの非決定性)。
   これで同条件 2 回が SHA256 一致になった。副産物として計画の ★罠 (`--shot-frame` は
   tick ではなくフレーム番号) は**消滅した** — 撮影中は両者が一致する。
   解除は `--shot-realtime` (連番ライブ撮影は従来どおり実時間)。
2. **フォントも機種依存だった。** `FontAtlas::Init` は `assetsonts\*.ttf` → システムの
   YuGothM/meiryo/msgothic の順で探し、全滅すると内蔵 8x8 (ASCII) に落ちる。
   **英語版 Windows Server の CI ランナーには日本語 TTF が入っていない**ので、
   探索させると golden と別の絵になる (テキストのあるシーンは全滅する)。
   `--font-embedded` を足して撮影時は内蔵 8x8 に固定した。これで golden は
   「そのマシンに何のフォントが入っているか」から完全に切り離される。
   代償として **CI のスクショは日本語グリフ焼成を被覆しない** (probe シーンでは `??????` になる)。
   被覆を戻したければ OFL 系の日本語 TTF を `assetsonts\` に同梱すればよい (M53 候補)。
3. **測った数字 (許容値の根拠)。**
   - Debug と Release は WARP 同士で**ビット一致** (maxDiff=0) → 撮影は Release だけでよい。
   - WARP と実 GPU (RTX 3060) は同じシーンで **maxDiff=2、518400 画素中 376856 が非ゼロ差**。
     FXAA/トーンマップの丸め差。**つまり golden は必ず `--warp` で撮る**。
   - 逆に言えば「まるごと別のラスタライザ」でも差は 2 に収まる。既定 tolerance を
     **2** にしたのはこの実測が根拠 (`MYE_SHOT_TOL` で上書き可)。実測 maxDiff は毎回ログに出す。
   - Forward と Deferred は maxDiff=84 / 195759 画素で違う = 2 本撮る価値がある (`--deferred` が
     本当に効いていることの確認も兼ねる)。
4. **`--img-diff` は「差が無い」と「比べられなかった」を分ける** (0 / 1 / **2**)。
   寸法違いや読み込み失敗を PASS 側に混ぜると、撮影自体が壊れた日に回帰テストが静かに緑になる。
5. **`.bat` は CRLF で書くこと。** LF だけで書いた `shot_verify.bat` は cmd.exe が
   行の途中で切って「'y_verify.bat' is not recognized」等の意味不明なエラーを撒いた
   (UTF-8 の日本語コメントと組み合わさると特に壊れる)。既存の `replay_verify.bat` も CRLF。
6. **`Runtime.exe --scene` は相対パスで通る** (計画の未検証事項)。
   `cache\parts_showcase.scene.json` も `assets\sceneslow_title.scene.json` もそのまま読める。
7. **`shot_verify.bat` はビルドしない。** `replay_verify.bat` の後に回す前提 (CI もその順序)。
   parts/flow のシーンは shot 側でも組み直す (replay_verify と同じ流儀 — 単体で回せるように)。
8. **M52b の CI が赤かった件 (`selftest (Debug)` の CookedCache 1 項目) をここで直した。**
   環境差ではなく**時計運**だった: `CookedCache::ReadValidated` は「サイズ同一 かつ mtime 同一」
   なら内容を読まずに hit する高速路を持ち (M51b の設計意図)、テストは同サイズ書換で miss を
   期待して「mtime も動く」を暗黙の前提にしていた。NTFS の最終更新時刻は約 14 ms 刻みなので
   2 回の書込が同じ刻みに入ると mtime が動かず**正当に** hit する。修理はテスト側で
   `fs::last_write_time` を秒単位で明示的にずらす (エンジンの高速路は触らない)。
   ★同型の罠: 「速く書けば時刻が動く」に依存したテストは全部これになる。
9. **CI ランナー上での実地確認だけは未。** ローカルでは 8 ビルド (警告 0、`MyeWarnAsError=true`)
   / selftest 両構成 / replay_verify 3 ペア / check_rules 0 error / shot_verify 5 本 maxDiff=0 /
   golden 1 画素改竄で赤 まで確認済み。残る未知数は **runner の WARP が開発機の WARP と
   何レベル違うか** (tol=2 で足りるか) と **ウィンドウのクライアント実寸が 960x540 になるか**
   (DPI/デスクトップ解像度次第。ズレたら `--img-diff` が exit 2 =「比較不能」で明示的に落ちる)。
   初回 push の run で確認し、tol が足りなければ `MYE_SHOT_TOL` を CI の env に足す。

---

## 決定台帳 (着手前に確定した論点)

### 1. スナップショットの境界 = 既存の決定論レーンと同一
対象は **sim レーンのみ**: World (全アーキタイプのカラム生バイト + records + freeIndices + firstRoot + RNG)、
Scene の `time_`/`persist_`/`nextFileId_`/`sourcePath_`/`overrides_`、CpuParticleBackend の pools、
CollisionSystem の `prevPairs_`/`prevSolidPairs_`、ScriptHost の `started_`、EngineLoop の `prevTickInput`。
**対象外**: C# (ManagedHost) レーン / GPU パーティクル / VfxRenderer トレイル / TransformSystem 側テーブル (M51c) /
オーディオ。前 3 者は復元時に Reset、側テーブルは無効化して再計算に落とす (= 結果ビット同値)。
C# 非対応は record/verify と**同じ境界**であり、新しい制約ではない (タイムトラベル・ネット対戦とも C++ スクリプト前提)。

### 2. tick 本体は関数抽出して 3 経路で共有する
通常 tick / タイムトラベル再シム / ロールバック再シムが `RunOneTick()` の**同一コード**を通る。
分岐が増えるほど「再シムだけ挙動が違う」種類のバグが入るため。抽出の合格条件は
**3 ペアの replay_verify がビット一致すること** ただ 1 つ。

### 3. .rep のバージョン bump は M52d の 1 回だけ (v3 → v4)
`snapshotSize` (埋め込み初期状態、M52f が使う) と `playerCount` (M52g が使う) を **同時に**入れる。
使い始めるのは後のサブでも、フォーマットの改版は 1 回に束ねる (M51h の ABI 束ねと同じ思想)。
v4 の tick レコードは `InputSnapshot[playerCount] + uint64 hash`。

### 4. WARP は「自動フォールバック + 明示 `--warp`」
`D3D_DRIVER_TYPE_HARDWARE` 失敗時に WARP で再試行し、採用アダプタをログに出す。規則 1 非抵触
(構成値による分岐であって `#ifdef _DEBUG` ではない)。**sim は CPU 専用なので replay のハッシュは WARP でも同一** —
CI で `replay_verify` を回せる根拠はここ。**golden スクショは `--warp` 固定で撮る** (CI と開発機を一致させるため)。

### 5. ネットは「遅延ロックステップ + 予測ロールバック」
入力遅延 3 tick を基本とし、未着入力は「前 tick の繰り返し」で予測して先へ進む。実入力が予測と食い違ったら
最終確定 tick へ Restore して再シム (上限 8 tick、超えたら stall へフォールバック)。
**ネット層は sim 状態を書かない**: 「いつ tick が回るか」は非決定論でよいが「tick が何を消費するか」は
確定入力だけで決まる。`LoadScene` はネット中も許可 (record/verify で既に許可済み)、`LoadGame` は禁止 (同上)。

### 6. desync / クラッシュの一次成果物は「再現可能なバグ報告」
どちらも同じバンドラで `crash\<stamp>\` に **minidump + 埋め込みスナップショット付き crash.rep + hashdump + crash.txt**
を吐く。`--replay-verify crash.rep` が**シーン非依存に**クラッシュ直前まで再現できる状態をゴールとする。
`crash.txt` には障害モジュール + RVA + exe TimeDateStamp を入れて [[crash-triage-recipe]] の手順 1-2 を省略させる。

### 7. CI は「ローカルの bat をそのまま呼ぶ」単一 job
CI 専用の検証ロジックを書かない (二重メンテを作らない)。`replay_verify.bat` / `shot_verify.bat` /
`Editor.exe --selftest` / `check_rules.ps1` をそのまま実行する。CI 固有の引数 (`--no-audio` / `--warp`) は
bat が読む環境変数 `MYE_EXTRA_ARGS` 経由で注入し、bat 本体はローカルと 1 文字も変えない。

### 8. 依存順序
安い順に先行させて足場を作る: **a 診断 → b CI → c スクショ** (以降の全サブが CI とピクセル回帰に守られる) →
**d スナップショット基盤** (最大の共有部品) → **e タイムトラベル → f クラッシュ** (d の消費者) →
**g マルチ入力 → h UDP/ロックステップ → i ロールバック + ABI v13** (ABI 束ねは最後、既存規則どおり)。

---

## サブ分割 (9 分割)

### M52a: ハッシュ分岐の詳細診断 — フィールド単位ダンプ + 差分ツール
- **目的**: MISMATCH した tick は既に出る。埋めるのは「**どのエンティティのどのコンポーネントのどのフィールドが**割れたか」の側。
- **設計**: `HashWorld` / `HashWorldDetailed` / 新 `HashWorldDump` を**内部 1 実装の 3 出口**に一本化する
  (3 実装が乖離すると診断が嘘をつく — selftest で 3 者の total 一致を固定)。
  ダンプはタブ区切り 1 行 1 フィールド (`tick / entity idx:gen / 名前 / コンポーネント名 / フィールド名 / 値 hex / 畳み込み hash`)。
  `FieldDesc::name` があるのでフィールド名は既に取れる。**★`HashEntity` は `FieldTypeSize(f.type)` 分まるごと読む** —
  ダンプも同じバイト範囲を出さないと String64 の終端以降 (M48i の罠) を見落とす。
- **触る**: `src\Engine\Engine\Replay\WorldHasher.h/.cpp`、`Engine\EngineLoop.cpp` (MISMATCH 時に
  `<rep>.tickNNNN.actual.dump` を自動書き出し)、`src\Editor\EditorMain.cpp` / `src\Runtime\RuntimeMain.cpp`
  (`--hash-dump PATH` / `--hash-dump-tick T` / `--hash-diff A B`)、`tools\replay_verify.bat` (失敗時のみ
  PASS 側の dump を撮って diff を表示)、新規 `tools\bisect_replay.bat` (`git bisect run` ラッパ)。
- **検証**: selftest (2 世界を 1 フィールドだけ変えて `--hash-diff` 相当が**その 1 行だけ**を報告 / 3 出口の total 一致 /
  String64 の終端以降の差分を検出) / M49 の実証レシピ (値を +0.001 変異させた temp ビルドで実 MISMATCH を起こし、
  診断が正しい行を指すことを実地確認) / replay_verify 3 ペア無風。

### M52b: CI (GitHub Actions) + WARP フォールバック
- **目的**: push で 8 ビルド + selftest + check_rules + replay_verify 3 ペア + package が回る。README にバッジ。
- **触る**: `src\Engine\Renderer\GraphicsDevice.h/.cpp` (HARDWARE 失敗時の WARP 再試行 + `EngineConfig::forceWarp` +
  採用アダプタのログ)、`Engine\EngineLoop.h` / 両 Main (`--warp`)、`tools\replay_verify.bat` (`MYE_EXTRA_ARGS` 対応)、
  新規 `.github\workflows\ci.yml`、`README.md` (バッジ + CI で回る検証の一覧)。
- **ワークフロー**: windows-2022 / 単一 job 直列 / `concurrency` で同一 ref の古い run をキャンセル / `timeout-minutes` /
  トリガは push・pull_request・workflow_dispatch。段は
  ①`tools\build_managed.bat` 両構成 → ②`tools\replay_verify.bat` (内部で 8 ビルド + 3 ペア + check_rules を実施) →
  ③`Editor.exe --selftest` 両構成 → ④`--package` スモーク。警告 0 は `/p:TreatWarningAsError=true` で機械化
  (Common.props は触らない — ローカル開発の外部ヘッダで詰まらせないため)。失敗時は `cache\*.rep` / ログ / dump を artifact 化。
- **検証**: **先にローカルで `--warp` を付けた replay_verify 3 ペアと selftest が PASS すること**を確認してから push
  (runner でのウィンドウ生成が本サブ唯一の未知数。ダメなら該当段だけ `continue-on-error` で先に緑にし、
  進捗表に事実を残す) / 実際に push して緑を確認 / 意図的に規則違反を 1 行入れて赤になることを確認。

### M52c: スクリーンショット回帰テスト
- **目的**: 決定的スクショを golden と比較して CI でピクセル差分を検出する。レシピは既に確立済み (M46 系)。
- **設計**: 撮影は **Runtime.exe** で行う (`enableImGui=false` = imgui.ini とカーソル位置に依存しない)。
  `--warp --no-audio --width 960 --height 540 --shot-frame 3 --frames 8` 固定。golden は `tests\golden\*.png`。
  対象は 5 本: 既定デモ (Forward) / 既定デモ (`--deferred`) / parts showcase / flow title / UI プローブシーン (新規に小さいものを 1 本コミット)。
  **RT デモは WARP では重すぎるので CI 対象外** (ローカル任意)。
- **触る**: 新規 `src\Engine\Renderer\ImageDiff.h/.cpp` (stb_image で読み、最大チャンネル差 / 差分画素数 / 差分ヒート PNG)、
  `src\Editor\EditorMain.cpp` (`--img-diff A B [--tol N] [--diff-out PNG]`、exit 0/1)、
  新規 `tools\shot_verify.bat` (`--update` で golden 再生成)、`tests\golden\`、`.gitignore` (`tests\actual\`)、`.github\workflows\ci.yml`。
- **検証**: selftest (ImageDiff: 同一画像 = 0 / 1 画素だけ変えた画像 = 差分 1 / サイズ違いはエラー) /
  `shot_verify.bat` が 5 本 PASS / golden を 1 画素改竄して赤になることを確認 / CI で失敗時に actual + diff が artifact に出ること。
- **★罠**: `--shot-frame` は**フレーム番号 (tick ではない)** — ヘッドレスは高 fps で frame≠tick (M51h/M51j の既知罠)。
  自動露出は screenshot 指定時に既に instant 化されている (`EngineLoop.cpp:343`) ので追加対処は不要。

### M52d: SimSnapshot 基盤 + tick 本体の関数抽出 + ReplayFile v4
- **目的**: 「撮って戻して同じ入力で回すと同じハッシュ列」を機械保証する共通部品。e / f / i の全てがこれに乗る。
- **触る**: 新規 `src\Engine\Engine\Replay\SimSnapshot.h/.cpp` (`SimRefs` で sim レーンの所有者を束ね、`Capture` / `Restore`)、
  `src\Engine\Core\World.h/.cpp` (private メンバへ触るための `SnapshotWrite`/`SnapshotRead`)、
  `Core\Archetype.h` (カラム生バイトの取り出し)、`Engine\Engine\EngineLoop.cpp` (tick 本体 = 現 `EngineLoop.cpp:548-820` を
  `RunOneTick(TickServices&, ...)` へ抽出)、`Engine\Replay\Replay.h/.cpp` (**v4**: `snapshotSize` + `playerCount` を追加、
  ヘッダ直後に snapshot blob。blob があれば EngineLoop はシーンロードの代わりに Restore する)、両 Main (`--snapshot-stress N`)。
- **★不変条件**: Capture はアーキタイプの**生成順**を保存する (`ForEachArchetype` の列挙順 = ハッシュ順序に直結)。
  `commands_` / `cmdPayloads_` が空である tick 末 (= `ApplyStructuralChanges` 直後) でしか撮らない (MYE_CHECK)。
  `queryCache_` と Scene の `fileIdCache_` は派生なので復元時に破棄。
- **検証**: selftest (Capture → 世界を壊す → Restore → `HashWorld` 一致 / EntityID の generation と freeIndices の LIFO 順まで一致 /
  アーキタイプ生成順の保存) / **`--snapshot-stress 37`: verify 中 37 tick ごとに Capture+Restore を挟んでも
  600 tick の期待ハッシュが全一致する** — これを 3 ペアすべてで実行し `replay_verify.bat` の 4 段目に恒久追加する
  (既存 .rep を再利用するので追加コストは verify 1 回分) / スナップショット 1 枚のバイト数と Capture/Restore 実測 ms をログ。

### M52e: タイムトラベル・デバッグ (巻き戻しスクラブ)
- **目的**: Play 中のワールドをタイムラインで過去 tick へシークし、そこから再シミュレートして観察・分岐できるようにする。
- **設計**: リング = 30 tick ごとにスナップショット (最大 120 枚 = 60 秒、バイト上限でも制限) + **全 tick の入力**。
  シーク = 「target 以下の最寄りスナップショットへ Restore → 記録入力で target まで描画なし再シム」。
  シーク後に Play を続けると**その時点から分岐**し未来のリングは破棄する (Unity に無い挙動なので UI で明示)。
  再シム中は出力レーン (オーディオ / 振動 / セーブ書出) を record/verify と同じ条件で抑止する。
- **触る**: 新規 `src\Editor\TimeTravel.h/.cpp`、新規 `src\Editor\Windows\TimelineWindow.h/.cpp`、
  `Editor\EditorApp.h/.cpp` (窓登録 + ドックレイアウト)、`Editor\EditorToolbar.cpp` (巻き戻しボタン)、
  `Engine\Engine\EngineLoop.cpp` (再シムゲート)、`LocalizationTable.inl`。
- **検証**: 自動プローブ `--timetravel-selftest` (`--autoplay` と同じ流儀の CLI 検証。tick T まで進める → T-K へシーク →
  記録入力で T まで再シム → ハッシュが元の T と一致、を複数の K で) / replay_verify 3 ペア + snapshot-stress 無風 /
  手動: スクラブ → Inspector で過去の値を観察 → 再開で分岐。
- **★罠**: Play 中のみ有効 (編集中は tick が進まずリングが空)。ミニシーン編集モード中は Play 自体が禁止なので無効。
  C# スクリプトの状態は戻らない (決定台帳 1) — 窓にその旨を出す。

### M52f: クラッシュ時 .rep 自動添付
- **目的**: 落ちたら「再現可能なバグ報告」が自動で残る。Editor と **Runtime の両方** (配布ビルドのバグ報告が本命)。
- **触る**: 新規 `src\Engine\Platform\CrashHandler.h/.cpp` (`SetUnhandledExceptionFilter` + `set_terminate` +
  `_set_purecall_handler` + `_set_invalid_parameter_handler`、`MiniDumpWriteDump`、**ハンドラ内では事前確保バッファのみ使用**)、
  `Engine\EngineLoop.cpp` (クラッシュ用の小さいリング = スナップショット 1 枚 + 直近 600 tick 入力を常時維持。
  M52e のエディタ用リングとは別インスタンス・別上限)、両 Main (インストール + `--crash-test <av|purecall|terminate>`)、
  `build\Common.props` (git ハッシュのビルド時埋め込み)、`Engine\Replay\Replay.cpp` (v4 の snapshot blob 書き出し)。
- **出力**: `crash\<yyyyMMdd_HHmmss>\` に `minidump.dmp` / `crash.rep` (埋め込みスナップショット付き) /
  `crash.txt` (例外コード・障害モジュール + RVA・exe TimeDateStamp・構成・git ハッシュ・現 tick・直近ログ) / `scene.json`。
- **検証**: `--crash-test av` で実際に落として bundle が生成されること → **生成された `crash.rep` を
  `--replay-verify` に食わせるとクラッシュ直前 tick まで再現し最後に同じ例外で落ちる** (= 再現性の実証)。
  3 種のハンドラそれぞれで実施。CI では走らせない (プロセスが落ちるため) — ローカル手順を bat 化して残す。
- **★罠**: ハンドラ内で数百 MB を書くのは自殺行為。リングは数 MB に収める。二重フォルト対策に再入ガードを置く。

### M52g: マルチプレイヤー入力レーン (ネット 1/3)
- **目的**: sim が「1 本の InputSnapshot」ではなく「プレイヤー数分のレーン」を消費する形にする。**ネット無しで完結**し、
  ローカル 2P で証明する (トランスポート導入前に決定論側の変更を全部片付ける)。
- **触る**: `src\Engine\Engine\EngineLoop.h` (`EngineContext::inputs[kMaxPlayers=4]`。既存 `input` は player 0 の別名として維持)、
  `src\Engine\Platform\InputActions.h/.cpp` (プレイヤー毎の評価と参照)、`Engine\EngineLoop.cpp` (tick 頭の評価をレーン分)、
  `Engine\Replay\Replay.cpp` (v4 の `playerCount` を実際に 1 より大きくする)、両 Main (`--local-players N`)、
  `Engine\DemoContent.cpp` (ローカル 2P デモシーン)。**ABI は M52i まで触らない** (C++ スクリプトからエンジン内 API で読む)。
- **検証**: replay_verify に **4 ペア目 (ローカル 2P デモ)** を追加 / 既存 3 ペアは playerCount=1 で録り直して無風 /
  selftest (レーン毎のアクション評価が独立していること・未接続レーンがゼロ値であること)。

### M52h: UDP トランスポート + 遅延ロックステップ (ネット 2/3)
- **目的**: 2 台 (プロセス) が同じ tick 列を回す。desync 検出は M52i、ここは「揃った入力で回る」ことに集中する。
- **触る**: 新規 `src\Engine\Platform\Net\UdpSocket.h/.cpp` (Winsock2 非ブロッキング、`ws2_32.lib`)、
  新規 `src\Engine\Engine\Net\NetSession.h/.cpp` (ホスト/参加、ハンドシェイク、入力交換、状態機械)、
  `Engine\EngineLoop.cpp` (tick 入力ソースを local / replay / net で切替)、両 Main
  (`--net-host PORT [--net-players N]` / `--net-join HOST:PORT` / `--net-delay N`)、新規 `tools\net_verify.bat`。
- **設計**: パケットは `{magic, protoVersion, sessionId, playerIndex, baseTick, count, InputSnapshot[count], lastAckTick}`。
  **直近 8 tick 分を毎回冗長送信**して再送機構を持たない (ロスに強く実装が単純)。入力遅延は既定 3 tick。
  全 peer の tick T 入力が揃うまで tick を進めない (stall)、溜まったら 1 フレーム複数 tick (既存の tick ループを使う)。
  ハンドシェイクで **protoVersion + `MYE_API_VERSION` + 構成 + 開始シーンの WorldHash + ReplayFile 版**を照合し、
  不一致は接続拒否 (desync の最大要因を入口で潰す)。ネット中は record/verify と同じく C# レーン停止 + オーディオ suspend。
- **検証**: `tools\net_verify.bat` = 同一マシンで host/joiner の 2 プロセスを起動して両方 `--replay-record` させ、
  **2 本の .rep がバイト一致すること**を M52a の差分ツール (.rep 対応を追加) で機械検証する (= 本当に同じ tick を回した証明) /
  `--net-loss N` (意図的パケット破棄) を入れても .rep 一致 / selftest (パケット直列化の往復・順序入替耐性)。

### M52i: 予測ロールバック + desync 検出 + ABI v13 + デモ + 仕上げ (ネット 3/3)
- **目的**: 未着入力を予測して先へ進み、外れたら巻き戻して再シムする。割れたら即検出して再現可能な報告を吐く。
- **触る**: `Engine\Net\NetSession.cpp` (予測 = 前 tick の繰り返し、確定時の差分判定、`SimSnapshot` による巻き戻しと
  `RunOneTick` による再シム、上限 8 tick、超過時は stall)、`Engine\EngineLoop.cpp` (再シム中の出力レーン抑止 = M52e と共用)、
  `src\Shared\EngineAPI.h` / `Shared\ScriptAPI.h` / `Engine\Script\EngineApiTable.cpp` / `src\Scripting\Interop.cs`
  (**ABI v13**: `NetLocalPlayer` / `NetPlayerCount` / `NetIsConnected` / `NetPingMs` / `NetRollbackCount` /
  `GetActionForPlayer` / `GetAxisForPlayer`)、`tools\check_rules.ps1` (規則 11 の版⇄スロット数表を更新)、
  `Engine\DemoContent.cpp` (`--net-demo` = 2P 対戦の小デモ。**C++ スクリプトで書く** — C# はネット中に走らない)、
  新規 `src\Editor\Windows\NetWindow.*` (接続状態 / ping / 遅延 / ロールバック回数 / 直近ハッシュ)、
  `engine_spec.md` (11.4 ネット決定論 + ADR-013 ロールバック採用 + ADR-014 CI とピクセル回帰)、`README.md`。
- **desync 検出**: tick T のハッシュを T+delay のパケットに載せて交換し、両者が揃った時点で比較。不一致なら
  M52f のバンドラで `crash\desync_<tick>\` に .rep + hashdump を吐き、`--hash-diff` の手順をログで案内して停止
  (`--net-halt-on-desync`、既定 on)。
- **検証**: `net_verify.bat` をロールバック有効 + `--net-loss 20` で回して .rep 一致 /
  **意図的 desync 注入** (`--net-poke-tick N` で片側の 1 フィールドを変異) → 検出が働き bundle が出て
  `--hash-diff` が変異したフィールドを指すこと (a と i の統合実証) / 8 ビルド + build_managed 両構成 /
  規則 11 がスロット数一致を報告 + 変異テスト / replay_verify 4 ペア + snapshot-stress + shot_verify + CI 緑。

---

## 見送り (M53 候補メモ)

- 3 人以上のメッシュ / リレーサーバ / NAT 越え (M52 は 2 人 P2P に限定)
- C# レーンの決定論化 (現状は record/verify・ネット・タイムトラベルの全てで対象外)
- スナップショットの差分圧縮 (現状は毎回フルコピー。60 秒リングが重ければここ)
- 実 GPU のセルフホストランナー (WARP と実機のピクセル差を CI で見る)
- OFL 日本語フォントの `assetsonts\` 同梱 (CI のスクショに日本語グリフ焼成を戻す。M52c 申し送り 2)
- ネットの遅延補償 (補間・スムージング) / 観戦モード / .rep からの動画書き出し

## 実装セッション冒頭で要確認 (未検証事項)

- `World` の private メンバへスナップショットが触る手段 (メンバ関数追加 vs friend) — レイヤ規約との相性
- `AudioSystem::Init` が音声デバイス無し環境で失敗したときの挙動 (CI runner。`--no-audio` で回避できるはずだが未確認)
- GitHub Actions windows-2022 での D3D11 WARP デバイス生成 + ウィンドウ生成の実地確認 (**M52b 唯一の未知数**)
- `EngineLoop.cpp` tick 本体のローカル変数依存の全量 (M52d の `TickServices` 構造体サイズ = 抽出コスト)
