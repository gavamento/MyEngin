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
