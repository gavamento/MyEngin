# sub-01: Recast の vendor と決定論・復元方式の試作

- 依存: なし
- 状態: OK (コミット待ち)
- 往復: 1

## やること
M82 全体で最も荷重のかかる未知 (spec 2. #1, #2 / 4.4 N1〜N4) を潰す。コンポーネントはまだ作らない。

1. `external\recastnavigation\` に Recast Navigation を vendor する。使うのは `Recast` / `Detour` / `DetourTileCache` / `DetourCrowd` / `DebugUtils` の `Include` と `Source` とライセンスだけ (RecastDemo / Tests / CMake 類は入れない)。最新リリースタグ (またはその時点の main) のコミットを固定し、`external\VERSIONS.md` に 1 行足す (Zlib)。
2. `build\Engine.vcxproj` (+ `.filters`) に libtess2 と同じ形 (`WarningLevel=Level3`、`TreatWarningAsError=false`) で手書き追加。インクルードパスは `build\Common.props` か Engine.vcxproj に足す (Runtime / Server も Engine.lib 経由で同じものを使う)。
3. 無圧縮の `dtTileCacheCompressor` と、`dtTileCacheMeshProcess` の最小実装を `src\Engine\Engine\Navigation\` に置く (後続サブがそのまま使う)。
4. 決定論の試作 `NavDeterminismSelfTest` (`--selftest` に載せる、近くに `*SelfTest.cpp`):
   - 手続き生成の固定ジオメトリ (床 + 段差 + 坂 + 柱数本、Recast 入力の三角形配列を直書き) を TileCache 方式でベイク (層を作る → TileCache に入れる → 全タイルを dtNavMesh へ)。
   - 経路クエリ (findPath / findStraightPath / findNearestPoly / raycast / findRandomPoint を Pcg32 の frand で) を固定の問い合わせ列で実行。
   - 障害物の追加・削除を tick 境界で入れ、`dtTileCache::update` を upToDate まで回す。
   - dtCrowd に 8 体を固定順で入れ、目的地を与えて 100 tick (dt = 1/60) 進める。
   - 各段階の結果を FNV 等でハッシュ (ポリゴン・頂点・経路・エージェント位置/速度のビット列) し、ログへ出す + 期待値 (定数) と比較。期待値は Debug で採取 → Release で同値を確認してから焼く。
5. 復元方式の試作 (spec 4.4 の候補 a / b / c): tick 50 の状態を保存 → 新しい dtNavMesh / dtTileCache / dtCrowd へ復元 → 50 tick 進めて、連続実行の tick 100 とハッシュ一致することを SelfTest で確かめる。採った方式で、状態の保存に掛かる時間 (8 体・128 体) を計測する。
   - **将来の再ベイクへの備え (spec 4.4 F1 / F3 / F4 / F5)**: tick 30 で 1 枚のタイルの層を「別ジオメトリ (柱を 1 本足した入力) からベイクした層」に差し替えてから tick 50 で保存 → 復元 → 連続実行一致を、上の試験とは別の項目で確かめる。差し替えは後続サブの NavSystem がそのまま使える「タイル単位の差し替え口」の形で書く (TileCache の obstacle 再構築も同じ口を通す前提)。層の所有者は呼び出し側 (アセットのバッファを借りない)。`dtNavMeshParams` の `maxTiles` / `maxPolys` と salt の桁配分、巡回までの差し替え回数を計算して ADR 下書きに書く。
6. 不一致が出たら Recast にパッチし `external\recastnavigation\PATCHES.md` に (ファイル・行・理由・確認方法) を記録。
7. `docs\adr\ADR-023-navmesh.md` を下書きする: 採用した復元方式・却下案と理由・計測値・パッチ一覧・タイル差し替えと再ベイクの差し込み方・salt の桁配分・AcousticNav との役割分担 (sub-09 で仕上げる)。

## やらないこと (このサブでは)
- コンポーネント、TickRunner への組み込み、SimSnapshot の節の実装 (方式を決めるだけ。実装は sub-03)
- エディタ UI、`.mnav`

## 触る場所 (planner の見立て)
- 新規: `external\recastnavigation\**`、`external\recastnavigation\PATCHES.md` (パッチがあれば)、`src\Engine\Engine\Navigation\NavTileCacheSupport.{h,cpp}` (名前は任意)、`src\Engine\Engine\Navigation\NavDeterminismSelfTest.{h,cpp}`、`docs\adr\ADR-023-navmesh.md`
- 変更: `external\VERSIONS.md`、`build\Engine.vcxproj` + `.filters`、`build\Common.props` (include)、selftest の登録箇所 (既存 `*SelfTest` の呼び出し元)
- 参考: `build\Engine.vcxproj:575-633` (libtess2 の書き方)、`src\Engine\Core\Util\Random.h` (Pcg32)、`src\Engine\Engine\Physics\Fracture\FractureSelfTest.cpp:2276-2324` (新しいシステムへ restore して連続実行と照合する前例)
- Recast の `rand()` / `time` 系の使用箇所を grep して、エンジンから呼ぶ経路に無いことを確かめる (`check_rules.ps1` は `src\` しか見ない)

## 受け入れ条件 (このサブ)
1. (spec 1) vendor 済み、VERSIONS.md 記載、Debug / Release が `/p:MyeWarnAsError=true` で 0 警告。
2. (spec 2) NavDeterminism のハッシュが Debug / Release で一致し、焼いた期待値と一致。
3. (spec 3) 復元の SelfTest (tick 50 で保存 → 新インスタンスへ復元 → tick 100 で連続実行と一致) が両構成で PASS。ADR-023 下書きに方式・却下案・計測値。
4. (spec 18 (a)) タイル差し替え後の保存 → 復元 → 連続実行一致の SelfTest が両構成で PASS。ADR 下書きに `maxTiles` / `maxPolys` / salt の桁配分と巡回までの回数。
5. 既存の `--selftest` に新しい FAIL を増やさない (既知の Source control 2 件は除く)。`check_rules.ps1` 0。

## 検証コマンド
- `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` と Release
- `bin\x64\Debug\Editor.exe --selftest` / `bin\x64\Release\Editor.exe --selftest` (NavDeterminism のハッシュ行を両方から抜いて比較)
- `bin\x64\Release\Server.exe` の selftest 経路があれば同じハッシュを確認 (無ければ未実施と書く)
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - external\recastnavigation\** — Recast Navigation v1.6.0 (6dc1667) の Recast / Detour / DetourTileCache / DetourCrowd / DebugUtils の Include / Source / License.txt。RecastDump.cpp はビルド対象外 (FILE 使用)
  - external\VERSIONS.md — 1 行追加 (Zlib)
  - external\recastnavigation\PATCHES.md — パッチ 2 件 (ファイル・行・理由・確認方法)。rand() / time 系は Recast に無いことを grep で確認
  - external\recastnavigation\DetourCrowd\Source\DetourCrowd.cpp:48-50 — MAX_ITERS_PER_UPDATE 100 -> 0x3fffffff (経路要求を同じ update で完走させ、tick 境界の途中状態を無くす)
  - external\recastnavigation\DetourCrowd\Include\DetourLocalBoundary.h:27-29 — friend struct dtLocalBoundaryAccess; (挙動は変えない)
  - build\Engine.vcxproj / .filters / Common.props — 手書き ClCompile 追加 (Level3 / TreatWarningAsError=false)、5 つの Include を追加
  - tools\gen_project_files.ps1 — filters 生成の対象に external\recastnavigation を追加
  - src\Engine\Engine\Navigation\NavTileCacheSupport.{h,cpp} — NavRawCompressor / NavFixedAllocator / NavMeshProcess (63 -> 歩行可、再構築したタイルの記録) / NavTileStore (層の所有、障害物、ReplaceTileLayers = 差し替え口、Commit = 同期確定 + 全タイルの正規化、SaveState / LoadState、HashNavMesh) / NavBakeTileLayers (Engine 層の純関数) / NavSaveCrowd・NavLoadCrowd・NavHashCrowd
  - src\Engine\Engine\Navigation\NavDeterminismSelfTest.{h,cpp} — 固定ジオメトリのベイク -> 経路クエリ -> 障害物 -> dtCrowd 100 tick、復元 2 シナリオ (障害物のみ / タイル差し替え込み)、毎 tick の保存 -> 復元 -> 再保存、履歴依存の実証 (正規化なしで割れる)、70 障害物のバッチ、保存・復元・Commit の時間計測。ハッシュ 16 個を kExpected に焼いた
  - src\Editor\App\EditorMain.cpp — --selftest の末尾へ RunNavDeterminismSelfTest を追加
  - src\Server\ServerSelfTest.cpp — Server.exe --selftest でも同じテストを走らせ、ハッシュが Editor と一致することを確認できるようにした
  - docs\adr\ADR-023-navmesh.md — 下書き (採用方式・却下案・計測値・パッチ・差し込み口・salt 桁配分・Detour の制約・AcousticNav との分担)
仕様との差分:
  - [逸脱] 復元方式は spec 4.4 の候補 (a)(b)(c) のどれでもなく (b') = 層 + 障害物 + スロット表 + salt を保存し、Commit が全タイルをキー順・スロット順に入れ直して履歴を消す方式 — (b) の素朴版 (salt の表だけ) は実測で割れた (リンク順 0x270640B3D9D88ECA 対 0xE2BAE9CBE3175BDA)。dtNavMesh のリンク順・検索連鎖順が addTile / removeTile の履歴に依存するため
  - [追加] Recast パッチ 2 件はビット一致のためではなく復元のため (不一致は出ていない)。MAX_ITERS_PER_UPDATE (dtPathQueue の途中状態は private)、dtLocalBoundary の friend (境界キャッシュの保存)
  - [追加] Server.exe の --selftest へ NavDeterminism を接続 (ServerSelfTest.cpp) — spec は「selftest 経路があれば」。N1 の Server.exe 側の根拠として 2 行足した
  - [追加] tools\gen_project_files.ps1 の改修 (filters の外部ソース対象に recastnavigation) — 生成スクリプトが filters を毎回作り直すため必要
  - [追加] NavTileStore は障害物要求を 8 件ごとに処理する (dtTileCache の MAX_UPDATE 64 / DT_MAX_TOUCHED_TILES 8 の黙った取りこぼし対策。70 個一括の試験あり)
  - [逸脱] 受け入れ条件 1「Debug / Release が /p:MyeWarnAsError=true で 0 警告」は未達。src\Engine\Renderer\Compute\ProjectComputeRunnerSelfTest.cpp (未変更、HEAD のまま) の C4127 が両構成で C2220 になる (VS 18 のコンパイラ)。フラグ無しでは Debug / Release とも exit 0。新規ファイルと Recast に警告は出ていない (ビルドログで確認)
  - [未実装] tools\replay_verify.bat / shot_verify.bat は未実行 (sub-01 の検証コマンドに無く、sim へ未接続)
検証:
  - msbuild MyEngine.sln Debug / Release (/p:MyeWarnAsError=true 無し) → 両方 exit 0。警告は libtess2 (既存) と ProjectComputeRunnerSelfTest.cpp (既存) のみ
  - msbuild ... /p:MyeWarnAsError=true → Debug / Release とも ProjectComputeRunnerSelfTest.cpp(139) の C2220 で失敗 (上記。HEAD 由来)
  - Editor.exe --selftest (Debug 347 s / Release 62 s) → NavDeterminism PASS (0 failures)。他の FAIL は既知の Source control 2 件 (external cherry-pick / external revert) だけ
  - Server.exe --selftest (Debug / Release) → exit 0、NavDeterminism PASS
  - ハッシュ照合: Debug Editor / Release Editor / Debug Server / Release Server の NavDeterminism ハッシュ 16 行が完全一致 (Compare-Object で差分 0)。期待値は Debug で採取し Release / Server で一致を確認してから焼いた
  - 変異試験 (Debug): 復元で境界を捨てる -> 毎 tick のバイト比較が FAIL / salt の表を戻さない -> 復元後の全項目が FAIL / 差し替え後の障害物を付け直さない -> シナリオ B が FAIL (元へ戻して全 PASS)
  - MAX_ITERS_PER_UPDATE を 1 にした試験 (Debug Server) → 経路要求が途中の tick で保存が失敗し 21 項目 FAIL = パッチの必要性を確認 (元へ戻した)
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 2 / 3 / 4 / 5 は満たす。1 の MyeWarnAsError は HEAD 由来の別ファイルで未達、復元方式は spec の候補外の (b') に着地 (差分欄に根拠)
  正しさ: 4 — 4 構成のハッシュ一致、変異 3 種を検出、70 障害物のバッチ一致、履歴依存を負例で実証。dtLocalBoundary の保存が軌道を変えることは試験ジオメトリでは示せていない (バイト比較でのみ守られる)。Off-Mesh / agentAnims は未対応
  コード品質: 4 — 責務は Engine\Navigation に閉じ、所有・寿命・失敗の扱いを明示。LoadState は失敗時に途中状態を残す (ADR に注記)。ReplaceGroup が長い
  テスト: 4 — 16 ハッシュ + 40 項目超の照合。replay_verify / 実シーン接続は未 (sub-03 以降)
不安・質問:
  1. spec 4.4 を確定してほしい: Nav 節は「NavTileStore::SaveState の blob (層の差し替え分 + 障害物 + スロット表 + salt、通常 0.7 KB) + NavSaveCrowd の blob (8 体で約 3.2〜3.9 KB、128 体で 66.5 KB)」。128 体は毎 tick capture (約 148 KB) に +45%。差分化は sub-03 で実機の数字を見て判断でよいか
  2. 受け入れ条件 1 の MyeWarnAsError: ProjectComputeRunnerSelfTest.cpp の C4127 を直すサブを別に立てるか、このサブで直すか (隣のバグなので手を付けていない)
  3. Commit は変更のたびに全タイルを入れ直す (O(全タイル))。13 層で Release 12.5 us (変更なし)。数百タイルの実マップで障害物を毎 tick 動かすと効くかもしれない。sub-05 で計測してから最適化するでよいか
  4. dtCrowd の経路要求を無制限にしたので、重い経路が 1 tick に集中しうる (node pool 4096 まで)。sub-03 の実機計測で問題なら要求数を絞る方向で良いか
  5. maxTiles / maxPolys の決め方 (ADR の目安: 層数 x 1.5 / 最大ポリゴン数 x 2、salt 10 ビット以上) は sub-02 のベイク結果を見て確定する
触ったファイル:
  - external/recastnavigation/** (新規 56 ファイル)、external/VERSIONS.md
  - build/Common.props、build/Engine.vcxproj、tools/gen_project_files.ps1
  - src/Engine/Engine/Navigation/NavTileCacheSupport.h / .cpp、NavDeterminismSelfTest.h / .cpp
  - src/Editor/App/EditorMain.cpp、src/Server/ServerSelfTest.cpp
  - docs/adr/ADR-023-navmesh.md
  - (生成物だがコミットが要る) build/Engine.vcxproj.filters (gen_project_files.ps1 の出力)
申し送り:
  - reviewer: ProjectComputeRunnerSelfTest.cpp の MyeWarnAsError 失敗は HEAD のまま (git diff 0)。このサブとは無関係
  - sub-02: NavBakeTileLayers は Engine 層の純関数の入口 (入力は三角形配列 + 設定)。World -> 三角形の収集 (F2) をここへ足す。ベイク設定の cellSize 等と NavMakeStoreConfig の maxTiles / maxPolys はアセットに焼く
  - sub-03: NavTileStore の Commit / SaveState / LoadState と NavSaveCrowd / NavLoadCrowd をそのまま NavSystem と SimSnapshot の Nav 節に使える。agent <-> crowd スロットの対応 (エンティティキー順) は NavSystem 側で決める。エリア ID は層では 0 = 通行不可 / 63 = 歩行可 (NavMeshProcess が 63 -> 0 に写す)。frand は引数なしの関数ポインタなので World::Rng() への静的ポインタ経由
  - sub-07: dtCrowd::m_agentAnims (Off-Mesh を dtCrowd 自身が渡る状態) は保存していない。アプリ側で渡るなら不要、dtCrowd に渡らせるなら friend が要る
  - Detour の制約 (MAX_UPDATE 64 / DT_MAX_TOUCHED_TILES 8 / 要求キュー 64) は ADR-023 に記載

## フィードバック履歴
- round 1: VERDICT OK (planner)。復元方式 (b') を spec 4.4 に確定。受け入れ条件 1 は HEAD 由来の C4127 を除外する形に spec 側を修正 (R9)。質問 1〜5 は spec / sub-02・03・05 に反映。
