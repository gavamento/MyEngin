# ADR-014: CI とピクセル回帰 (WARP 固定 + 内蔵フォント)

## 決定

- CI (`.github\workflows\ci.yml`) は **windows-2022 の単一 job** で、`tools\*.bat` を
  そのまま呼ぶ。**CI 専用の検証ロジックを書かない。**
- CI 固有の事情は環境変数 3 本だけで注入する:
  `MYE_EXTRA_ARGS` (`--warp --no-audio`) / `MYE_MSBUILD_ARGS` (`/p:MyeWarnAsError=true`) /
  `MYE_DOTNET_ARGS` (`/p:TreatWarningsAsErrors=true`)。
- `GraphicsDevice::Init` は HARDWARE 失敗時に **WARP へ自動フォールバック**し、
  `--warp` で明示指定もできる。採用アダプタはログに出す。
- golden スクリーンショットは **`--warp` + `--font-embedded` 固定**で撮る。
  既定許容差は `maxDiff <= 2` (`MYE_SHOT_TOL` で上書き可)。
- `--screenshot` 指定時 (連番 `--shot-every` を除く) は**決定的撮影モード**が自動 on:
  dt を固定 tick 長に固定 (= frame 番号 == tick 番号) し、非同期テクスチャを撮影前に
  drain する。解除は `--shot-realtime`。
- `crash_verify.bat` と `net_verify.bat` は **CI 対象外**。

## 理由

### なぜ「bat をそのまま呼ぶ」なのか

CI 専用の検証手順を書くと、**手元で緑・CI で赤 (またはその逆)** が起きたときに
「本物の差か、検証ロジックの差か」の切り分けから始めることになる。手元と CI が
同じ 1 本を呼んでいれば、その問いが最初から存在しない。

### なぜ `/p:TreatWarningAsError=true` ではだめだったか

C++ の `TreatWarningAsError` は **ClCompile の項目メタデータ**なので、グローバル
プロパティとして渡しても誰も読まない。**警告 0 で緑になったが実は何も見ていない**
という最悪の形で気づかず通っていた。`Common.props` の `ItemDefinitionGroup` に
`MyeWarnAsError` の橋渡しを置いて初めて効く。

### なぜスクリーンショットを `--warp` と内蔵フォントで固定するのか

実測で:

- Debug と Release は **WARP 同士でビット一致**
- WARP と実 GPU は **maxDiff = 2** (518400 画素中 376856 画素が非一致)
- Forward と Deferred は maxDiff = 84

つまりラスタライザを固定しない限り、ピクセル回帰は「機種が違う」というノイズを
毎回踏む。フォントも同じ問題で、英語版 Windows Server に日本語 TTF は無い —
探索させると別の絵になる。代償として **CI のスクショは日本語グリフ焼成を被覆しない**
(OFL フォント同梱は M53 候補)。

### なぜ `--img-diff` は「比較不能」を別の終了コードにするのか

一致 0 / 差あり 1 / **比較不能 2** の 3 値にしてある。寸法違いを PASS に混ぜると、
撮影そのものが壊れた日に**静かに緑**になる。回帰テストが一番やってはいけない壊れ方。

### なぜ crash_verify / net_verify を CI から外すのか

前者は**自分のプロセスを意図的に落とす**、後者は **2 プロセス同時起動 + UDP 待受 +
実時間タイムアウト**。どちらも赤くなったときに「本物の失敗か runner の都合か」を
切り分けづらい。ロジックの回帰は `--selftest` 側で押さえる:
`CrashRing self test` と `Net session self test` (1 プロセス内でループバック接続、
待受ポート 0 なのでポート衝突が原理的に起きない) が CI で毎回走る。

## 結果

- CI ステップは `shell: cmd`。`Editor.exe` / `Runtime.exe` は Windows サブシステム
  なので、PowerShell から起動すると**待たずに戻り終了コードが取れない**。
- CookedCache の flaky はここで顕在化して修理した。真因は時計運で、NTFS の mtime は
  約 14ms 刻み — 同サイズ書き換えが同じ刻みに入ると高速路が正当に hit する。
  テスト側で `fs::last_write_time` を秒単位でずらす。
- `.gitattributes` に `*.png binary` を明示している。golden が改行変換されると
  「ピクセル回帰が理由不明で赤い」形で出る。

## 追記: Performance Regression CI (2026-09-29)

- Release の `Editor.exe --perf-bench <json> --perf-commit <40桁SHA>` はヘッドレスで
  8 項目を固定入力、3 回ウォームアップ、9 回測定の中央値で記録する。対象は
  ECS 更新、Transform 伝播、broadphase、XPBD、D3D11 WARP の CPU Draw 投入、
  sim snapshot、world hash、AssetDatabase の 10k アセット走査。Draw は GPU 完了待ちを
  含めず、AssetDatabase は `.meta` 準備後の再走査を測る。
- `tools/perf_verify.ps1` は指定した対象と基準のソースを同じ runner に展開し、
  それぞれ Release ビルドして JSON を比較する。基準の既定は対象の第一親。
  目標値はレポートに表示するだけで、性能値では CI を失敗にしない。
  コミット・ビルド・計測・JSON の欠落や不正値は失敗とする。
- 初回導入時に限り基準コミットにベンチが存在しないため、対象だけ測り、
  基準を `unavailable` と明記する。次のコミットからコミット間比較を行う。
- Editor の専用「性能比較検証」ウィンドウはエンジンリポジトリの分岐線付き Gitline を表示する。
  選択したコミットを基準にし、未選択なら現在の HEAD の第一親を使う。
  `tools/perf_dispatch.ps1` が GitHub CLI の既存認証で workflow_dispatch を起動する。
  対象と基準の両方が origin に push 済みであることを確認する。
  認証が無い場合はログイン案内を表示し、エディタ内にトークンを保存しない。
- 専用ウィンドウで起動時の HEAD と基準 SHA を固定表示する。成果物 `mye-performance`
  を待って取得し、8 項目の現在値・基準値・増減率・目標比を同じ窓に表示する。
  GitHub の実行 URL と失敗理由も同じ窓に表示する。
