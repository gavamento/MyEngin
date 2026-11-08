# sub-08: フロクセルの自動有効 (場だけのモード) と、Volume の霧に太陽光を当てる bool

- 依存: sub-07, sub-16 (波 W3。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W3)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): 合成箇所のシェーダ全部 (10 本 + **sub-16 の背景の PS**)、RenderSystem のフロクセル判定とゴッドレイ。コンポーネントの欄は変えない (sub-01 の `autoFroxel` / `fogSunLight` を使う)
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §2 #14 / #15、§4.3 のフロクセルのモード。ユーザー回答 (2026-11-08): 太陽光は「当てる / 当てないを bool で選べる」、
フロクセルは「場を置いたら自動で有効にし、OFF にもできる」。sub-03 で GroundLayer の霧は専用描画になったので、**このサブの対象は
フロクセルで描く Volume (3D) の霧だけ**。欄は sub-01 で入れた `fogSunLight` / `autoFroxel`。

1. 太陽光 (`fogSunLight`、場ごと、既定 false) を Volume の媒質にも:
   - true の Volume の媒質にだけ、平行光 (`type == 0`) の散乱 `σ_s(その場) × 放射輝度 × HG(cosθ, g)` を足す。g はフロクセルの既存の `anisotropy`。`froxel_inject.cs.hlsl` のライトのループで今 `continue` している平行光 (`:105-107`) を、Volume の σ が 0 より大きいセルだけで評価する
   - 影なし。グローバルの媒質には今までどおり足さない (解析フォグの太陽寄せとの二重計上は起きない。spec §2 #14)
   - false の Volume の見た目は sub-07 と 1 ビットも変わらない
2. 自動有効 (`autoFroxel`、場ごと、既定 true):
   - ビューのフロクセルが off (カメラの `CameraPostFx.froxelOn` が false、または Scene View でグローバル設定が off) で、`autoFroxel` が true かつ霧を出す (`fogDensity > 0`) 有効な **Volume** があれば、そのビューでフロクセルを**「場だけのモード」**で回す。判定は `RenderSystem.cpp:935-949`。GroundLayer はフロクセルを使わないので判定に入れない
   - 場だけのモードの約束 (= Volume の霧が足される以外、絵は従来の「フロクセル off」と同じ):
     - グローバルの媒質は 0 (`froxelDensity` と高さの霧を注入しない)
     - 解析フォグ (`FogComponent`) は全区間に掛ける (`FroxelFogOrigin` の受け渡しをしない = off と同じ起点)
     - ゴッドレイを自動で降ろさない (`RenderSystem.cpp:2364-2374` の条件から外す)
     - 合成は `color × T + inscatter`。Volume の外のセルは σ = 0 なので T = 1、inscatter = 0
     - 実装の口 (CB の値を 1 つ足すか `gFroxelEnabled` に値を足すか) は coder が選ぶ。合成箇所は 10 本 (sub-03 で地面の霧を入れたのと同じ箇所)。フロクセル off と従来のフルは**ビット不変**
   - カメラが `froxelOn = true` のとき (フル) は従来どおりで、Volume の媒質が足されるだけ
   - 黙って変わらないようにする明示:
     - Inspector (DisturbanceField) の `autoFroxel` の説明: 「Volume の霧を描くために、カメラのフロクセルが off ならフロクセルを自動で回す (場の霧だけ。解析フォグ・ゴッドレイは変えない。GPU に約 7 MB と注入パスの時間)。止めるには off。GroundLayer には関係しない」。`Tr()` の両言語
     - ログ: ビュー (viewKey) が場だけのモードに入った / 出たときに INFO を 1 行ずつ
     - Profiler: フロクセルのパスの時間が場だけのモードでも同じ名前で出る
3. デモ: sub-07 で煙の柱のために立てたカメラの `froxelOn` を外し、自動有効で煙が写る形にする。煙の柱の `fogSunLight` を true にする

## やらないこと (このサブでは)

- 太陽光の影、グローバルの媒質への平行光の注入
- `froxelOn` / ゴッドレイのユーザー設定の書き換え (実効値だけを決める。M57d と同じ流儀)
- GroundLayer の描画の変更 (sub-03 で済み)

## 触る場所 (planner の見立て)

- `assets\shaders\froxel_inject.cs.hlsl:88-120`、`disturbance_common.hlsli`
- `src\Engine\Engine\Rendering\RenderSystem.cpp:935-949` (有効判定)、`:2364-2374` (ゴッドレイ)
- フロクセルを合成するシェーダ 10 本と、sub-16 が足した Forward の背景の PS (`assets\shaders\forward_background_fog.hlsl`、`SkyboxPass::NeedsBackgroundFog` の条件にも場だけのモードを入れる。`froxel_common.hlsli:106-140` の `FroxelFogOrigin` / `FroxelComposite` など)
- Inspector の説明 (`Components.cpp` の `MYE_FIELD_TIP`)、`LocalizationTable.inl`
- `src\Engine\Engine\Demo\DemoContentField.cpp`

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 31, 32 (Volume の分)。

- 31 (自動有効):
  - (a) Volume を視錐台の外に置いた場面 (`autoFroxel = true`、霧あり、`FogComponent` の解析フォグとゴッドレイも on) と、同じ場面の `autoFroxel = false` の絵が `--img-diff` で差 0
  - (b) Volume を視界に入れると、カメラの `froxelOn = false` のまま煙が写る
  - (c) `autoFroxel = false` で煙が描かれず、フロクセルのパスが走らない
  - (d) フルモードの既存デモ (`--fog-demo`) と、フロクセルを使わない既存デモ 1 本の絵が着手前と差 0
  - (e) モードに入った / 出たときの INFO がそれぞれ 1 行だけ (60 フレーム回して数える)
  - (f) Editor の Scene View (グローバルのフロクセル off) で、Play 前の煙が見える
  - (g) GroundLayer だけのシーン (`--field-demo` の煙の柱を除いた状態か、selftest 相当の場面) ではフロクセルが走らない
- 32 (Volume の分): `fogSunLight = false` の絵が sub-07 と差 0。true で太陽側を向いた煙が明るい。Volume を視界外に置いて true にしても絵が変わらない
- 12: replay_verify `field` (運用によりまとめて実行)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- スクショ一式 (Runtime.exe / Editor.exe の Scene View)、`--img-diff`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
