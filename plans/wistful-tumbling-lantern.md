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
| M52d SimSnapshot 基盤 | 完了 | (本コミット) | 下記「M52d の申し送り」参照 |
| M52e タイムトラベル | 完了 | (本コミット) | 下記「M52e の申し送り」参照 |
| M52f クラッシュバンドル | 完了 | (本コミット) | 下記「M52f の申し送り」参照 |
| M52g マルチ入力レーン | 完了 | (本コミット) | 下記「M52g の申し送り」参照 |
| M52h UDP + ロックステップ | 完了 | (本コミット) | 下記「M52h の申し送り」参照 |
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


### M52d の申し送り (計画外の事実・罠)

1. **抽出の検証順序が肝だった。** `RunOneTick` の抽出 (`TickRunner.h/.cpp` へ 289 行を移動) は
   M52a 申し送り 1 の手をそのまま使った: **旧ビルドで demo / parts / flow の .rep を録っておき、
   抽出後の新ビルドでそれを verify** (3 本とも 600 tick 全一致)。
   ★**この検証を済ませてから ReplayFile を v4 に上げること** — 順序を逆にすると v3 の .rep が
   読めなくなり、抽出が挙動を変えていないことを確かめる唯一の手段を自分で捨てる。
2. **`recorder.IsActive()` を tick 頭でキャッシュしてはいけない。** 記録は最終 tick の**途中**で
   `Finish()` を呼んで `active_` を落とすので、`const bool recording` に畳むと同じ tick の後半
   (`pendingLoadSlot` のゲート) の挙動が変わる。`RunOneTick` では毎回引き直すラムダ
   (`Recording()` / `Verifying()`) にして、移動が純粋な「移動」で済むようにした。
3. **World の節は blob の最後に置く。** 復元は「小さい節 (Scene / パーティクル / 衝突 /
   スクリプト / prevTickInput) を全部一時領域へ読み切る → `World::SnapshotRead` (それ自体が
   全読み後の一括差し替え)」の順で走る。この順序だから**どこで失敗しても現世界に手が
   付いていない**。逆順だと壊れた blob で「途中まで復元された世界」ができ、これが一番たちが悪い。
4. **`hierarchyDirty_ = true` が TransformSystem 側テーブル (M51c) の唯一の解除口。**
   復元で立て忘れると「LocalTransform が前回と同ビットだから据え置き」判定に入り、
   復元前の WorldMatrix が生き残る。`Rebuild` が `lastTrs_` / `state_` を全無効化する
   既存経路にそのまま相乗りできるので、TransformSystem 側は 1 行も触っていない。
5. **blob のバイト決定性は unordered コンテナの整列で作る。** Scene の override 表
   (`unordered_map`) と ScriptHost の `started_` (`unordered_set`) は昇順へ整列してから書く。
   selftest の「撮り直しがバイト同一」はこれが無いと落ちる。同じ状態 → 同じ blob は
   M52i の desync 比較が乗る土台なので、ここは仕様として固定した。
6. **非 sim レーン (C# / GPU パーティクル / トレイル) の Reset は `RestoreSimSnapshot` の
   仕事にしなかった** (決定台帳 1 は「復元時に Reset」とだけ書いていた)。
   「戻した後どう見せたいか」は消費者ごとに違うため — タイムトラベルは未来のトレイルを
   消したいが、`--snapshot-stress` は描画を乱したくない。**M52e で明示的に呼ぶこと**
   (`SimSnapshot.h` の冒頭コメントに責務を明記した)。
7. **実測 (1 枚あたり)**:
   | シーン | エンティティ | blob | capture (Dbg/Rel) | restore (Dbg/Rel) |
   |---|---|---|---|---|
   | 既定デモ | 528 | 148 KB | 0.65 / **0.040** ms | 0.66 / **0.059** ms |
   | parts | 31 | 10.0 KB | 0.13 / 0.010 ms | 0.47 / 0.030 ms |
   | flow | 10-13 | 5.2 KB | 0.10 / 0.007 ms | 0.37 / 0.021 ms |

   ★**Release は Debug の約 16 倍速い** (STL のイテレータデバッグが効く形のコードなので差が大きい)。
   M52i のロールバック予算はこの Release 値で見ること — 528 エンティティのシーンでも
   「Restore + 8 tick 再シム」の Restore 側は 0.06 ms しか食わない。
   **3 ペアとも「全 600 tick で毎 tick 往復」させても期待ハッシュ全一致**を実走で確認した
   (bat には 37 tick 間隔を恒久化 = verify 1 回分の追加コスト)。
   M52e のリング (30 tick ごと最大 120 枚) は既定デモ規模で **約 17 MB** になる勘定。
   差分圧縮が要るかはこの数字で判断する (M53 候補のまま据え置き)。
8. **`--snapshot-stress` の撮影点は tick 境界 (`RunOneTick` 直後)。** LoadScene を跨ぐ flow デモを
   毎 tick 撮っても `commands_` 空の `MYE_CHECK` は一度も鳴らなかった = **シーンロードは
   tick 内で構造変更を閉じている**ことが実走で確かめられた (計画の未検証事項の 1 つ)。
9. **v4 の tick レコードは `InputSnapshot × playerCount + uint64 hash`。** 書き手は
   `playerCount = 1` 固定だが、読み書きのループは playerCount 一般で回してあるので
   **M52g は header の値を変えるだけ**で済む。`snapshotSize` は M52d では常に 0
   (埋め込みを使い始めるのは M52f)。`ReplayTick` 構造体は廃止し、入力列とハッシュ列を
   別々に持って書き出しで綴じ直す形にした (playerCount 可変への素直な対応)。
10. **v4 の埋め込みスナップショットは M52d で実走まで通した** (計画では「blob があれば
    EngineLoop はシーンロードの代わりに Restore する」とだけ書かれていた)。
    使い始めるのは M52f だが、**producer が無いと consumer は必ず腐る**ので
    `--rep-snapshot` (記録時に開始状態を埋め込む、既定 off) を足して両側を実走で固定した。
    ★実証: **`--flow-demo` で録った .rep を `--flow-demo` 無しの既定デモ起動で verify して
    300 tick 全一致**した = 埋め込みがあれば .rep は**起動シーンに依存しない**。
    これが M52f の「クラッシュ .rep をシーン非依存に再現する」の土台そのもの。
    埋め込み時は RNG も prevTickInput も blob 側に入っているので、従来の
    `Rng().Restore(header)` 経路は通らない (埋め込みが無い .rep は従来どおり)。

11. **`TickServices` は 41 フィールド** (計画の未検証事項「tick 本体のローカル変数依存の全量」の答え)。
    内訳はシステム 15 / tick 内バッファ 4 / 出力レーン 5 / 遅延要求 3 / その他 14。
    全部このスコープのローカルなので、ループ前に 1 回組んで使い回している。

### M52e の申し送り (計画外の事実・罠)

1. **リングは `src\Editor\` ではなく `src\Engine\Engine\Replay\TimeTravel.h/.cpp` に置いた。**
   計画のファイル一覧は Editor 配下だったが、①再シムには `TickServices` が要り、それは
   EngineLoop のスコープにしかない ②M52f のクラッシュ用リングは「M52e と別インスタンス」=
   同じ型を**エンジン側から**使う、の 2 点で Editor 層には置けない (層規則: Editor → Engine)。
   結果として `TimeTravel` は**リングの器と方針だけを持つ純データ構造** (エンジン参照ゼロ) になり、
   Restore と再シムは EngineLoop の `SeekTo` ラムダが回す。この分離のおかげで
   分岐切り捨て・追い出し・最寄り探索はライブなワールド無しで selftest できている。
   エディタ側の新規は `Windows\TimelineWindow.h/.cpp` (UI) だけ。

2. ★**「ポーズ tick がリングの未来を食う」がこのサブ最大の罠だった。**
   `ctx.tickIndex` は Play/編集/ポーズに関係なく毎 tick 進む (RunOneTick 末尾)。素直に作ると
   シークで tick 100 へ戻した次のフレームのポーズ tick が `OnTickEnd(100)` を呼び、
   「シーク後に走った tick = 分岐」判定で**記録済みの未来 (101..400) を消してしまう** —
   行ったり来たりのスクラブが原理的に成立しない。対策は「スクラブ中は EngineLoop が
   tick を 1 本も進めない」(`scrubbing` で while を跨ぐ + accumulator を 0 に落とす)。
   これは Paused より強いゲートで、`--timetravel-selftest` の `scrub hold` 検査が固定している。

3. ★**ポーズ tick は「飛ばして良い tick」ではない。** sim (stepSim) は止まるが
   `inputActions.Evaluate` → `prevTickInput = ctx.input` はゲートの外なので、
   ポーズ中も sim 状態が 1 つだけ動く。リングは**全 tick** を
   `{input, simulated, hashAfter}` で記録し、再シムでは `ctx.simulateScripts` を
   記録値どおりに戻して忠実になぞる。飛ばすとポーズ明けの pressed/released が割れる。
   一方でスナップショットの間隔は **sim tick でだけ数える** (止まった世界を 148KB ずつ
   撮り続けない)。

4. ★**`audioHandleSeq` がスナップショットから抜けていた (M52d の穴を M52e で塞いだ)。**
   `EngineApiTable` の PlaySound 系は `++(*audioHandleSeq)` の戻り値をスクリプトへ返し、
   スクリプトはそれを**コンポーネントのフィールドに保存できる** (実例: `AudioDemo` の
   `voiceLo`/`voiceHi` = ハッシュ対象)。復元してもカウンタが戻らないと、再シムで採番が
   ずれて世界が割れる。**`--snapshot-stress` は同一 tick の往復しか見ないのでこの穴を
   構造的に検出できない** — 複数 tick を跨ぐ再シムで初めて効く種類の非対称性。
   `SimRefs::audioHandleSeq` を追加し `kSimSnapshotVersion` を **v2** へ (LOP 節に u64 追加)。
   ※実際の破れの再現までは未実施 (プローブは入力ゼロなので PlaySound を踏まない)。
   経路が実在することの確認まで。**M52i でも「スクリプトへ返す採番」を足したら同じ検算を
   すること** — hash に出ないが sim へ戻る値がこの型の穴になる。

5. **再シムのゲートは `TickServices::resim` 1 本にした。** 抑止するのは
   出力レーン (オーディオ drain / セーブ書出) と C# レーンの 3 つだけ。
   ★**入力側 (LoadGame の読み込み・LoadScene) は抑止しない** — 抑止すると元の tick 列と
   違う世界になって必ずハッシュが割れる。「読むもの」と「書き出すもの」を混同しないこと。
   パッド振動は EngineLoop 側 (フレーム単位) で `Scrubbing()` を足して 0 にしている。

6. **シークは毎回自己検証する。** 戻して同じ入力で回した結果を、その tick が最初に走った
   ときの `hashAfter` と突き合わせ、`Ok / HashMismatch / Failed` をタイムライン窓に出す。
   C# を使うプロジェクトではここが赤くなるのが正常 (決定台帳 1 の境界そのもの) なので、
   窓に「C# は巻き戻らない」と併記した。**嘘のタイムラインを黙って見せない**のが要点。
   ハッシュを毎 tick 撮るコストはリング有効時 (= Play 中) だけ。

7. **前進シークは復元せずに現在地から再シムする。** `target > 現在 tick` なら
   スナップショットを触らない。これは最適化であると同時に、プローブの主検査
   「T-K へ戻す → **記録入力で T まで進める** → 元の T と一致」そのものの経路。

8. **`--timetravel-selftest [N]` は 2 段構成** (tick 数は省略可、既定 400)。
   ① K = 1/7/30/61/120 で「戻す → 進める」を往復しハッシュ照合
   ② `RequestSeek` を出して**生のフレームループ上で** scrub hold (tick が止まる) と
   branch (再開で未来が消える) を確認。①だけだと 2 の罠が素通りする。
   Editor では `--autoplay` を自動で立てる (Play 中しか sim が進まないため)。
   プローブ中は `dt = kFixedDt` 固定 (M52c の決定的撮影と同じ手) — 実時間だと 400 tick に
   6.7 秒かかる。`tools\replay_verify.bat` の **5 段目**に Debug/Release 両方で恒久追加。

9. **プローブが「落ちること」を実証済み。** 復元直後に `Rng().NextU32()` を 1 回混ぜた
   一時ビルドで実走 → 5 項目すべて HASH MISMATCH、exit 1。
   (M52a 申し送り 5 と同じ流儀 — 検出器は落ちるところまで見ないと信用できない)

10. **実測 (既定デモ 528 体、400 tick 走行後のリング = 14 枚 / 1998 KB)**:

    | 再シム tick 数 | Debug | Release |
    |---|---|---|
    | 1 | 7.1 ms | 0.17 ms |
    | 30 | 112 ms | 1.65 ms |
    | 120 | 426 ms | 6.01 ms |

    ★Release は **約 0.05 ms/tick**。M52i のロールバック上限 8 tick なら Restore (0.06 ms) と
    合わせて **0.5 ms 未満** = 60Hz の予算内に十分収まる。Debug は 70 倍遅い (3.5 ms/tick) ので
    ロールバックの体感評価を Debug でやってはいけない。

11. **スクラブ解除は「Playing である」ではなく「Paused → Playing の遷移」で判定する。**
    状態で見ると `--autoplay` (最初から Playing) の経路でシーク要求が即座に取り消される。
    EditorApp が前フレームの `PlayState` を持って遷移を見ている。
    `PlayModeController` には `Pause()` / `Resume()` / `StepPending()` を追加した
    (`TogglePause` だと呼び側が現状態を知っている必要があり、スクラブのたびに反転する)。

12. **エディタ GUI の実機目視は未** (タイムライン窓のスクラブ / つまみ追従 / 分岐して再開 /
    ツールバーの巻き戻しボタン)。機械検証は `--timetravel-selftest` が全経路を通しているが、
    「掴んで動かしたときの見た目」だけは自動化できていない。M51f/M51i と同じ積み残し。

### M52f の申し送り (計画外の事実・罠)

1. ★**「落ちた tick の入力」をどう .rep に載せるかが、このサブ唯一の設計上の難所だった。**
   落ちるのは `RunOneTick` の**中**なので、tick 末に入力を載せる作りだと
   「まさに落ちた tick」が .rep に残らず、再生してもその tick へ**入れない** =
   再現できない再現用ファイルになる。対策は `OnTickBegin` で先に入力を載せること。
   ただしその tick の期待ハッシュはまだ存在しないので、**`worldHash == 0` を
   「期待値なし (未完了 tick)」の予約値に決めた** (`Replay.h` に明記)。
   - ここに「前 tick のハッシュ」等の嘘を書くと、**再現しなかったときに MISMATCH という
     別の事故に化ける**。値そのもので「照合しない」を表すのが唯一まともな解。
   - v4 のレイアウトは 1 バイトも変わらない = **版は上げていない** (決定台帳 3 を守れた)。
     偶然ハッシュが 0 になる確率は 2^-64 で、その場合も「その 1 tick が未照合になる」
     だけ = 安全側に倒れる。
   - 検証側は `unverifiedTicks` に数え、**「落ちずに通り抜けた = 今回は再現しなかった」**を
     WARN で明示する。VERIFY PASS の行にも未完了 tick がある旨を出す。

2. **バンドラは 2 層に割った** (計画は `CrashHandler.h/.cpp` 1 本だった)。
   `Platform\CrashHandler.*` … OS 側 (4 ハンドラ / minidump / crash.txt / 固定バッファ書式化)
   `Engine\Replay\CrashRing.*` … sim 側 (.rep イメージの常時維持 + scene.json のコピー)
   理由は層規則そのもの: リングは `SimSnapshot` を要るので Platform には置けない。
   両者は `CrashPayloadFn` (関数ポインタ + `void*`) 1 本だけで繋いである。

3. ★**リングは「.rep のバイト列そのもの」を常時維持する。** ハンドラ内で
   `std::vector` を舐めてファイルを組み立てる余裕は無い (確保も走査もできない前提)。
   `[header][snapshot][records...]` を 1 本の `image_` に持ち、
   - 入力の追記 = レコード書き込み → **`header.tickCount` の 1 ストアで発行**
     (書きかけのレコードは常に範囲外に居るので、どこで落ちてもイメージは整合する)
   - ハッシュの確定 = 8 バイト整列の単一ストア (破れない)
   - 撮り直し中だけ `ready_ = false` にして、ハンドラは**中途半端なイメージを書かない**
   実測 (既定デモ 528 体): スナップショット 130,797 B + 600 tick 分 43,200 B ≒ **170KB**。
   17 体の `ui_probe` シーンなら crash.rep は **12KB**。計画の「数 MB に収める」に対して桁 2 つ余裕。

4. ★**Debug CRT の不正パラメータは、ハンドラより先にアサートを報告する。**
   既定の報告先は**モーダルダイアログ**なので、そこで止まって
   `_set_invalid_parameter_handler` が永久に呼ばれない = Debug ではこの経路が実質死んでいた
   (実測: `Runtime.exe --crash-test invalidparam` が固まってバンドルが出ず、
   タイムアウトで気づいた)。`IsDebuggerPresent()` が偽のときだけ
   `_CrtSetReportMode(_CRT_ASSERT/_CRT_ERROR, _CRTDBG_MODE_DEBUG|_CRTDBG_MODE_FILE)` で
   報告先を落としてハンドラまで到達させる。
   **Release では `crtdbg.h` がこの 2 本を no-op マクロにするので `#ifdef _DEBUG` は不要**
   (= 規則 1 に触れずに済む。`check_rules.ps1` の許可リストを増やさなかった)。

5. ★**バッチの `if errorlevel 1` は SEH で落ちた exit code を拾えない。**
   AV の終了コードは `0xC0000005` = 符号付きだと負なので「1 以上か」の判定が**偽になる**。
   `crash_verify.bat` は全部 `if !ERRORLEVEL! EQU 0` / `NEQ 0` の数値比較で書いてある。
   実測の終了コード: av = `-1073741819`、purecall / terminate / invalidparam = `3`
   (非 SEH 経路はバンドルを書いてから `TerminateProcess(.., 3)`)。

6. **合成クラッシュの引き金はフラグ側にあるので、再現コマンドにも同じ引数を渡す。**
   `--crash-test` は「tick N で落とす」でしかないので、`crash.rep` だけでは再現しない
   (実バグは sim 状態が引き金なので素の再生で再現する)。`crash_verify.bat` は
   **2 段構え**で検証している:
   ① 素の `--replay-verify crash.rep` … 落ちる直前 tick まで**ハッシュ一致で再生**
      = 「同じ世界を復元できた」の機械的な証拠 (これが本命)
   ② `--crash-test` を足した `--replay-verify` … **同じ tick でまた落ちて 2 個目の
      バンドルを書く** = tick 番号ごと復元されている証拠
   実走: 5 種 × 3 検査すべて PASS (Debug / Release)。
   ★`crash_verify.bat` 自体は CI に入れない (プロセスを故意に落とすので、赤が本物か
   仕込みか区別しづらい) が、**CI の失敗アーティファクトに `bin/x64/*/crash/**` を足した** —
   ランナー上で*予定外に*落ちたときは、exit code ではなく再生可能なバンドルが手に入る。
   ★さらに強い実証として、**Debug の Editor で落ちて出た crash.rep が、Debug Runtime でも
   Release Runtime でも 25 tick ハッシュ一致で再生できた** (exe も構成も跨いで再現する)。
   埋め込みスナップショットがあるので `--scene` すら要らない。

7. **ハンドラ内の作業領域はスタックに置かない。** スタックオーバーフローで飛んできたときに
   残っているスタックは 1 ページ程度しか無く、数 KB のローカル配列を積んだ瞬間に
   二重フォルトして報告ごと消える。パス組み立ても書式化もファイル単位の `static` バッファ
   (再入ガードで単一スレッド化済み)。`dbghelp.dll` も **Install 時に**解決する
   (ハンドラ内の `LoadLibrary` はローダロック)。
   障害モジュールの特定も `GetModuleHandleEx` ではなく `VirtualQuery` の `AllocationBase`
   (素性の分からないポインタは `IsReadable` = `VirtualQuery` の `MEM_COMMIT` + 保護属性を
   確かめてから触る。**ハンドラの中で AV を起こすと再入ガードに弾かれて報告ごと消える**)。

8. ★**この方針を実地で検証するために `--crash-test stackoverflow` を足した** (計画は 3 種、
   結果 5 種)。**最悪ケースを試さないと「スタックを使わない設計」が効いているか分からない**。
   実測でそのまま 2 つ分かった:
   - **crash.txt と crash.rep は問題なく出た** = 事前確保方針は効いている。
   - **`MiniDumpWriteDump` は落ちた** — 残りスタックが足りず、0 バイトのダンプを残したまま
     入れ子フォルトでプロセスが即死し、後始末すら走らなかった。
   → **minidump だけ `CreateThread` で新品のスタックへ逃がした** (`MINIDUMP_EXCEPTION_INFORMATION`
   には**落ちたスレッドの id** を渡す。呼び出し元は有限待ち 20 秒、失敗なら空ファイルを消す)。
   結果、スタックオーバーフローでも **2.1 MB の正常な minidump** が出るようになった。
   ★minidump をバンドルの**最後**に書く順序 (txt → rep → dump) もここで効いている —
   一番重い処理が転んでも、本体の 2 つは既にディスクにある。

9. **hashdump はバンドルに入れていない** (決定台帳 6 は 4 点セットと書いていた)。
   `HashWorldDump` は 7500 行規模の文字列を作る = ハンドラ内では絶対に踏めない。
   ただし**受け取り側が crash.rep から生成できる**ので情報は失われない
   (`--replay-verify crash.rep --hash-dump-tick N`)。
   M52i の desync バンドラは**ハンドラの外**で走るのでそちらには入れられる。

10. **`crash.txt` には起動コマンドラインを丸ごと入れた。** 再現手順が
   「同じコミットの同じ構成 + このコマンドライン + `--replay-verify crash.rep`」で閉じる。
   `scene` 行はシーンの**元ファイル**で、コードから組んだデモは
   `(built in memory - no source file)` と正直に出る (既定デモがまさにこれ —
   `assets\scenes\main.scene.json` は存在しないので Runtime は `BuildDemoScene` に落ちている)。
   `scene.json` のコピーもその場合は出ない。**crash.rep は埋め込みスナップショット付きなので
   scene.json 無しでも再現できる** = 添付は人間向けの補助でしかない。

11. **リングは既定 on・記録/検証中も動かす。代償は tick 末の `HashWorld` が常時 1 回増えること。**
    ★実測 (Release / WARP / 既定デモ 528 体 / 600 tick の verify を 2 回ずつ):
    ring ON = 4512, 4458 ms / ring OFF = 4380, 4351 ms → 差 107〜132 ms / 600 tick
    = **約 0.2 ms/tick**。60Hz なら 1 フレーム 16.6ms の **約 1.2%**。
    「落ちたら再現可能な報告が必ず残る」の対価としては安いと判断して既定 on にしたが、
    タダではない — 外したいときは `--no-crash-handler`。
    なお TimeTravel が有効なとき (エディタの Play / `--timetravel-selftest`) は TimeTravel も
    自前でハッシュを撮るので**同じ tick で 2 回**走る。**M52i でロールバックが毎 tick ハッシュを
    要求したら、そこで 3 者を 1 本に畳む** (`TickServices` に「この tick のハッシュ」を持たせて
    RunOneTick / TimeTravel / CrashRing で共有する) のが自然。今は畳んでいない。

12. **設置は `app.OnStart` の前、解除は RAII。** シーンロードやスクリプト初期化で落ちるのは
    最もありふれた壊れ方なので、そこを取りこぼさない (その時点ではリングが空 =
    crash.rep 無しだが minidump と crash.txt は出る)。
    ★ハンドラが掴むのは `Run` のローカル (`ctx` / `crashRing` / `crashPayload`) なので、
    Install 以降の **early return が何本もある** (埋め込みスナップショットの復元失敗など)。
    経路を数える代わりに `CrashHandlerScope` の RAII に任せてある
    (`crashRing` より**後**に宣言 = 必ず先に走る)。

13. **`--crash-test` の綴り違いは exit 2 で弾く。** 黙って無視すると
    「落とすつもりで走らせたのに何も起きない」を延々追いかけることになる。
    同じ理由で、`TriggerTestCrash` から**戻ってきてしまった**場合も
    `[crash] ... did NOT crash` を出して exit 2 で止める (4 の罠はこれで気づけるようにした)。

14. **ビルド情報は生成ヘッダ経由** (`obj\generated\<Config>\MyeBuildInfo.h`)。
    `ClCompile` の `PreprocessorDefinitions` へ直接入れると、git ハッシュが変わるたびに
    全 .cpp のコマンドラインが変わって**毎回フルリビルド**になる。
    書き手は Engine プロジェクトだけ (`/m` で 4 プロジェクトが同じファイルを書くと競る)。
    `git describe --always --dirty --abbrev=12 --exclude=*` の出力を正規表現で検証してから
    採用し、git が無ければ `unknown` に落とす。
    ★`.props` の **XML コメントに `--` を書くと MSBuild が読めない** (`--dirty` と書いて
    `MSB4024` で全ビルドが即死した)。

15. **バンドルの出力先は `save\` / `cache\cooked\` と同じ二経路規則** —
    `projectRoot` があればその下、無ければ exe の隣。つまり配布 dist を
    `C:\Program Files\` 配下へ置くと**書けない** (バンドルが出ない)。
    M52 の範囲では既存の規則に揃えることを優先した。配布形態を詰めるとき
    (`%LOCALAPPDATA%` へ逃がす等) に見直す論点として残す。

16. **未検証**: ①ワーカースレッド (JobSystem) で落ちたときの挙動 — リングが撮影中なら
    crash.rep は出ない設計だが、実走で踏んではいない ②エディタ GUI を人が触っている最中の
    クラッシュ (実機目視は未。`--crash-test` 経由の機械検証は Editor でも通っている)
    ③同一秒に 2 回落ちたときの連番 (`_1`.. のフォールバック経路)。

### M52g の申し送り (計画外の事実・罠)

1. **計画が書いていなかった最大の論点は「レーンを足しても replay_verify は何も証明しない」**。
   ヘッドレス実行の実入力は全レーン恒常ゼロなので、「レーン 1 がレーン 0 を読んでいる」型の
   配線ミスは**記録側と検証側で対称に**起き、600 tick のハッシュが素直に一致してしまう。
   4 ペア目を試験として成立させるには 2 つとも要る:
   - `--synth-input` … `SynthLaneInput(tick, lane)` (レーンごとにブロック長を変えて撹拌する
     純関数) をライブ入力の代わりに流す。**verify の入力置換と同じ場所**に置くのが肝で、
     こうすると合成値がそのまま .rep に載り、検証側は普通の記録入力として再生する
     (= 検証コマンドに `--synth-input` は要らない。要る作りにすると「合成入力が無いと
     再生できない .rep」という嘘の仕様になる)。
   - `PlayerInputComponent` … レーンの評価結果を ECS へミラーしてワールドハッシュに載せる。
   実際に「全エンティティがレーン 0 を読む」変異を注入して **tick 0 で MISMATCH**、
   `--hash-diff` が `Player_2` の `PlayerInput.axes / heldBits / pressedBits` を名指しすることまで
   実走確認した (M52a と M52g の統合実証)。

2. **ミラーのフィールドに `kFieldNoSerialize` を付けてはいけない** — `WorldHasher.cpp` は
   NoSerialize フィールドをハッシュから**除外する**。派生値だからと反射的に付けると
   「毎 tick 書いているのにハッシュに 1 ビットも出ない」= 被覆ゼロになり、上の 1 が丸ごと
   無意味になる。代償はシーン JSON に tick 限りの入力値が載ることだけ (次 tick で上書き)。

3. **ABI は本当に 1 本も増やさずに済んだ** (計画どおり)。C++ スクリプトは v11 の
   `GetComponentField` (名前ハッシュの汎用スロット) でミラーを読む — `LocalPlayerDemo.cpp` が
   その実例。M50d で「型ごとのスロットを増やさない」ために入れた 2 本が、1 マイルストーン
   跨いでそのまま効いた形。レーン別の専用スロットは M52i の v13 へ束ねる。

4. **キーボードは分割しない**と決めた。レーン 0 = キーボード + マウス + パッド 0、
   レーン n>0 = パッド n。理由はアクションマップがプロジェクト共有の 1 本しか無く、
   レーン別マップは M52 の範囲外だから。つまり**実機のローカル 2P には物理パッドが 2 本要る**。
   パッド無しでレーンを動かす手段は検証用の合成入力側に寄せてある。
   ★副作用として、`--local-players N` は毎フレーム空きスロットへ `XInputGetState` を撃つ
   (未接続スロットの問い合わせは XInput の既知の重い経路)。**既定は playerCount=1 なので
   従来と 1 命令も変わらない**が、N を上げたときのフレーム時間はここを疑うこと。
   M52h でネット越しのレーンを足すときはポーリング自体が不要になる (入力はパケット由来)。

5. **未接続レーンは「評価をスキップ」ではなく「毎 tick ゼロで潰す」**。スキップにすると
   パッドを抜いた瞬間の値が残り、押しっぱなしのまま固まる。`InputActions` の状態配列は
   `定義数 * kMaxPlayers` を常に確保し、playerCount で伸縮させない (伸縮させると
   「レーンを増やした tick だけ添字がずれる」種類の事故が入る)。

6. **検証時は .rep の `playerCount` が `--local-players` に勝つ**。tick レコード長は
   ファイル側で決まっているので、ここで食い違うと入力が 1 レーンぶんずつずれて読まれ、
   全 tick MISMATCH という原因の見えない壊れ方になる。ログに「.rep 側を採用した」と出す。

7. **リング系の 1 エントリ長も固定にした**。`TimeTravelEntry` は常に kMaxPlayers 本
   (64B × 4 = 256B/tick)、`SimSnapshot` の LOP 節も常に kMaxPlayers 本 (blob v2 → **v3**)。
   実効レーン数で伸縮させると blob のレイアウトが起動オプション依存になり、
   `--local-players 2` で撮った crash.rep が 1 レーンの実行で復元できなくなる。
   一方 `CrashRing` のレコード長だけは playerCount で決まる (= .rep そのもののレイアウト)
   ので、`CrashRingConfig::playerCount` を **Begin より前に**確定させる必要がある。

8. **既存 3 ペアのハッシュは 1 ビットも動いていない**。M52a/M52d と同じ手順
   (変更前ビルドで 3 本の .rep を録っておき、変更後ビルドで verify) で実走確認した。
   `PlayerInput` は TypeId 末尾 append (=32) で既存シーンに存在しないため、
   ReplayFile の bump も不要 (v4 のまま)。

9. **`--local-demo` のシーンはファイルを作らない** (コードから毎回組む)。`scenePath_` は
   `cache\local_players.scene.json` に振ってあるだけで、保存はしない — 万一 Ctrl+S されても
   既定デモシーン (= golden.rep の入力) を潰さないための逃がし先。replay_verify は
   記録の前にこのファイルを消す (残っているとロード経路に落ちてコード側の正解と食い違う)。
   ★人が遊んで確かめるなら **`Runtime.exe --local-demo --local-players 2`**。
   `Editor.exe --local-demo` を単体で叩くと `automation` 判定に入らずプロジェクトマネージャが
   出てフラグごと無視される (`--rt-demo` / `--parts-demo` / `--flow-demo` と同じ既存挙動。
   `--frames N` や `--autoplay` を添えればレガシー起動になる)。

10. **未検証**: ①実機のパッド 2 本での動作 (`--local-demo --local-players 2` の目視。
    XInput スロット割り当ての実地確認はパッドが 2 台要る) ②`--synth-input` を付けたまま
    エディタ GUI を触ったときの体感 (マウスは合成しない設計にしてあるので UI は死なないはずだが
    目視は未) ③レーン 3/4 (`--local-players 4`) の実走はハッシュ上のゼロ確認のみ。

### M52h の申し送り (計画外の事実・罠)

1. **計画の合格条件「2 本の .rep がバイト一致」は片側しか見ていない**。両 peer が
   **対称に**間違えると (例: どちらも自レーンをレーン 0 として合成入力を作る) 2 本は
   仲良く一致してしまう — M52g で踏んだ「配線ミスが記録側と検証側で対称に起きる」の
   ネット版。そこで `net_verify.bat` は照合を **3 本**にした:
   host.rep == join.rep == **ローカル 1 プロセスの `--local-players 2 --synth-input` 参照 .rep**。
   合成入力は (tick, lane) の純関数なので、host がレーン 0・joiner がレーン 1 を作れば
   合成結果はローカル 2P 実行と 1 バイトも変わらないはず、という等式になる。
   実際に変異を 2 種類注入して**両方の検査に独立した歯があること**を実走確認した:
   - 変異 M1 (確定入力で自レーンを上書きしない) → `tick 0: input lane 0 differs at keys[8]`
     で **peers agree が FAIL**
   - 変異 M2 (自レーンの合成をいつもレーン 0 で作る) → **peers agree は PASS のまま**、
     `tick 0: input lane 1 differs at keys[8]` で **ローカル参照との照合だけが FAIL**
   M2 が計画どおりの 2 本照合では素通りする型。参照 .rep を足していなければ見逃していた。

2. **ネットの入口照合が既存バグを 1 本掘り当てた: シャドウ DLL の棚がプロセス間で衝突する**。
   `DllReloader` のコピー先は `<repo>\cache\hot` (Debug/Release も Editor/Runtime も**共有**) の
   `v1` 固定で、`Init` が `remove_all` を撃つ。2 プロセスを同時に起動すると
   後発の CopyFile が共有違反 (32) で弾かれ、**片方だけ C++ スクリプトが 1 本も
   登録されない世界**で走り出す。ネット対戦は 2 プロセス同時が常態なので日常的に踏む。
   → 見つけたのは**ハンドシェイクの開始ワールドハッシュ照合**
   (`rejecting ...: starting world hash`)。「desync の最大要因を入口で潰す」の実例が
   いきなり出た形。修理は 2 点:
   - 棚を `cache\hot\p<pid><n>` に分ける。
   - 掃除は「**もう居ない PID の棚だけ**」(`OpenProcess`+`WaitForSingleObject`)。
     ★ロックの有無で代用してはいけない — コピー直後・`LoadLibrary` 直前の一瞬はロックが
     無く、そこを相手の掃除に踏まれると自分の棚が消えて `LoadLibrary` が
     ERROR_PATH_NOT_FOUND (3) で落ちる (最初の修理案で実際に踏んだ)。
   - `LoadInitial()` の失敗を WARN から **ERROR** へ格上げ。「スクリプト 0 本でも動く」は
     リプレイもネットも別物になる状態で、黙って続けてよい失敗ではない。

3. **接続に失敗したときに `Run` の途中で `return` してはいけない**。Shutdown 群
   (ジョブの join / CoreCLR / D3D) を飛ばすとデストラクタ順で abort し、**exit code 3 +
   `crash\<stamp>\` にクラッシュバンドル**が出る (実測)。「相手に断られた」「ポートが
   埋まっていた」は日常的に起きるので、異常終了として報告してはいけない。
   `netFailed` フラグでフレームループを 0 周にし、通常の後始末へ落として exit 1 にした。
   ★同じ形の早期 return は埋め込みスナップショット復元失敗の経路にも残っている (M52d 由来)。
   そちらは滅多に起きないので今回は触っていないが、同じ罠であることは記録しておく。

4. **キープアライブは飾りではなく相互デッドロックの防止**。stall 中は
   `SubmitLocalInput` が呼ばれない = 1 パケットも出ない。相手が待っている tick を載せた
   パケットが落ちていた場合、再送機構が無いので**誰も送り直さないまま双方が固まる**。
   50ms ごとに直近パケットを撃ち直すことで初めて「冗長送信だけでロスに耐える」が成立する。
   終了時も同じ理由で最後の入力を 16 回撃ってから Bye を送る (黙って抜けると相手だけが
   stall タイムアウトで落ちる)。

5. **Windows の UDP は `SIO_UDP_CONNRESET` を切らないと受信が確率的に死ぬ**。相手がまだ
   起動していないと ICMP port unreachable が次の `recvfrom` に WSAECONNRESET として返る。
   参加側が先に立って JOIN を撃つのは正常な流れなので、ここを潰さないと「ハンドシェイクが
   ときどき失敗する」形で出る。★この環境の SDK では `mstcpip.h` から見えなかったので
   `#ifndef SIO_UDP_CONNRESET` で自前定義した (値は公開の固定 ioctl コード)。

6. **自レーンの値は「tick ごとにちょうど 1 回」確定させる**。フレーム頭で送る作りにすると、
   tick が 1 本も回らないフレームで同じ target tick を**違う値で送り直して**しまい、
   相手が先に消費した値と食い違って即 desync する。`tick t を回す直前に t + delay を送る`
   位置に置くと 1 回きりが構造的に保証される。最初の `delay` tick 分は開始時に先出しする
   (これが無いと tick 0 で自レーンが空 = 双方が永久に stall)。

7. **ライブ入力はフレーム頭で退避する**。tick ループ内で `ctx.inputs` はネットの確定入力に
   丸ごと上書きされるので、そこから自レーンを読むと「相手から返ってきた自分の入力」を
   送り直す循環になる。

8. **オーディオは止めない — 計画からの意図的な逸脱**。計画は「ネット中は record/verify と
   同じく C# レーン停止 + オーディオ suspend」と書いていたが、record/verify で音を止めて
   いるのは**1 フレームに 64 tick 回る早送り実行だから**であって決定論のためではない
   (音は tick 末のハッシュより後の出力レーン)。ネットは実時間で回るので、止めると理由なく
   無音になる。**C# レーン停止と `LoadGame` 禁止は計画どおり実施** — こちらは本当に
   決定論の要求 (C# は 2 台で同じ列を回す保証が無く、スナップショットにも入っていない)。

9. **ping はピギーバック方式なので「相手が次に送るまでの待ち」を含む**。ループバックで
   139ms と出る (Debug + WARP のフレーム時間が支配項)。ネットワーク遅延として読むと
   誤診するので、高いときはまず相手のフレームレートを疑うこと。

10. **構成を跨いだロックステップが成立する**。`net_verify.bat` のケース B は
    **Debug host ↔ Release joiner + ロス 20%** で 300 tick 完全一致。開始ワールドハッシュの
    照合も通る = 「Debug/Release がビット一致する」(replay_verify の担保) がネットにも
    そのまま効いている。ケース A (Debug ↔ Debug / ロス 0%) と 2 本立てにしてある。

11. **ABI 追加ゼロ / `.rep` は v4 のまま / SimSnapshot blob は v3 のまま**。ネット状態を
    スクリプトへ見せるスロット (`NetLocalPlayer` 等) は計画どおり **M52i の v13 へ束ねる**。
    `--rep-diff A B` は新規だが CLI であって ABI ではない。

12. **`net_verify.bat` は CI 対象外** (crash_verify と同じ扱い)。2 プロセス同時起動 +
    UDP 待受 + 実時間タイムアウトは、赤くなったときに「本物の失敗か runner の都合か」を
    切り分けづらい。ロジックの回帰は `--selftest` の **Net session スイート**が押さえる —
    こちらは **1 プロセス内で host/join をループバックで実際に繋ぐ** (待受ポートは 0 =
    任意なのでポート衝突が原理的に起きない)。パケット構造体の往復だけを見るテストに
    しなかったのは、ハンドシェイクと冗長送信という「実際に壊れる場所」が素通りするため。

13. **実測** (Debug + WARP、300 tick、ループバック): パケット送信 381 / 受信 363、
    ロス 0% では stall 4 回 (19ms)。`--net-loss 20` では捨てた 88 / stall 14 回 (88ms) でも
    .rep は完全一致。`--local-demo` 528 体ではなく 7 エンティティの軽いシーンなので、
    負荷側の一次データとしては使えない (M52i のロールバック予算は M52d の実測を使うこと)。

14. **未検証**: ①実 LAN / 物理 2 台 (ここまで全部ループバック) ②NAT 越え・パケット並べ替え
    (ループバックでは順序が入れ替わらない — 冗長送信で吸収できるはずだが実地確認は未)
    ③長時間セッション (最長 300 tick = 5 秒) ④実パッドでのネット対戦の目視
    ⑤エディタ (`Editor.exe --net-host`) でのネット対戦は起動経路の確認のみ。

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

- ~~`World` の private メンバへスナップショットが触る手段 (メンバ関数追加 vs friend) — レイヤ規約との相性~~
  → **M52d で解決**: `World::SnapshotWrite/SnapshotRead` をメンバ関数で追加 (friend は不採用)。
  バイト列ヘルパは `Engine/Core/ByteIo.h` に置く — SimSnapshot 本体は Engine 層なので、
  Core の World から参照するとレイヤ規約に反するため
- `AudioSystem::Init` が音声デバイス無し環境で失敗したときの挙動 (CI runner。`--no-audio` で回避できるはずだが未確認)
- GitHub Actions windows-2022 での D3D11 WARP デバイス生成 + ウィンドウ生成の実地確認 (**M52b 唯一の未知数**)
- ~~`EngineLoop.cpp` tick 本体のローカル変数依存の全量 (M52d の `TickServices` 構造体サイズ = 抽出コスト)~~
  → **M52d で解決**: 41 フィールド。抽出後の `TickRunner.cpp` は 535 行で、本体は元コードの
  純粋な移動 (差分は `recorder`/`player`/`app`/`prevWorld` の null 許容化だけ)
