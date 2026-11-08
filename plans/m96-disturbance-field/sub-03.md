# sub-03: フロクセルの自動有効 (場だけのモード) と、霧に太陽光を当てる bool

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること

spec §2 #14 / #15、§4.3 の「霧」の太陽光と自動有効。ユーザー回答 (2026-11-08): 太陽光は「当てる / 当てないを bool で選べる」、
フロクセルは「場を置いたら自動で有効にし、OFF にもできる」。どちらも sub-01 で入れた見た目の欄 (`fogSunLight` / `autoFroxel`) を使う。

1. 太陽光 (`fogSunLight`、場ごと、既定 false):
   - true の場の媒質にだけ、平行光 (`type == 0`) の散乱を足す: `inscatter += σ_s(その場) × 光の放射輝度 × HG(cosθ, g)`。g はフロクセルの既存の `anisotropy` (カメラの `froxelAnisotropy` か既定)。`froxel_inject.cs.hlsl` のライトのループで今 `continue` している平行光 (`:105-107`) を、場の σ が 0 より大きいセルだけで評価する
   - **影なし** (フロクセルの注入にはカスケード影が束縛されていない)。木陰でも霧が日に照る。制限として Inspector の説明と ADR に書く
   - グローバルの媒質 (`froxelDensity` × 高さ) には今までどおり平行光を足さない = 大気散乱 (`deferred_light.hlsl:249-275` の解析フォグの太陽寄せ) との二重計上は起きない (場の媒質は解析フォグが知らない別の媒質)
   - false の場の見た目は sub-02 と 1 ビットも変わらない
2. 自動有効 (`autoFroxel`、場ごと、既定 true):
   - ビューのフロクセルが off (カメラの `CameraPostFx.froxelOn` が false、または Scene View でグローバル設定が off) で、`autoFroxel` が true かつ霧を出す (2D は濃さチャネルあり、`fogDensity > 0`) 有効な場が 1 つでもあれば、そのビューでフロクセルを**「場だけのモード」**で回す。判定は `RenderSystem.cpp:935-949`
   - 場だけのモードの約束 (= 場の霧が足される以外、絵は従来の「フロクセル off」と同じ):
     - グローバルの媒質は 0 (`froxelDensity` と高さの霧を注入しない)
     - 解析フォグ (`FogComponent`) は全区間に掛ける (フロクセルの奥行きでの受け渡し `FroxelFogOrigin` を使わない = off と同じ起点)
     - ゴッドレイを自動で降ろさない (`RenderSystem.cpp:2364-2374` の条件から外す)
     - 合成は `color × T + inscatter`。場の外のセルは σ = 0 なので T = 1、inscatter = 0 = off と同じ値になる
     - 実装の口 (CB の値を 1 つ増やすか、`gFroxelEnabled` に値を足すか) は coder が選ぶ。合成箇所は `grep -rn gFroxelEnabled assets\shaders` の 10 ファイル。フロクセル off と従来のフル (カメラの `froxelOn`) は**ビット不変**
   - カメラが `froxelOn = true` のとき (フル) は従来どおり (グローバルの媒質・受け渡し・ゴッドレイ off) で、場の媒質が足されるだけ
   - 黙って変わらないようにする明示:
     - Inspector (DisturbanceField): `autoFroxel` の説明に「カメラのフロクセルが off なら、場の霧を描くためにフロクセルを自動で回す (場の霧だけ。解析フォグ・ゴッドレイは変えない。GPU に約 7 MB と注入パスの時間)。止めるには off」。文字列は `Tr()` の両言語
     - ログ: ビュー (viewKey) が場だけのモードに入った / 出たときに INFO を 1 行ずつ (フレームごとには出さない)
     - Profiler: フロクセルのパスの時間は既存の表示に出る (場だけのモードでも同じ名前で出ることを確認)
   - `autoFroxel = false` の場だけなら、フロクセルを入れない限り霧は描かれない (Inspector の説明に書く)
3. 3D (sub-07) にも同じ規則を当てられるよう、判定は「霧を出す有効な場」の一覧で書く (2D 専用にしない)
4. デモ: `--field-demo` のカメラの `froxelOn` を外し (sub-02 で true にしていた)、自動有効で霧が出る形にする。太陽 (平行光) を 1 つ足し、2D 層の `fogSunLight` は true にする (霧の広場の絵の基本形)

## やらないこと (このサブでは)

- 太陽光の影 (カスケード影をフロクセルの注入へ束縛する) — 後回し
- グローバルの媒質への平行光の注入 (既存の設計を変えない)
- `froxelOn` / ゴッドレイのユーザー設定の書き換え (実効値だけを決める。既存の M57d と同じ流儀)

## 触る場所 (planner の見立て)

- `assets\shaders\froxel_inject.cs.hlsl:88-120`、sub-02 の共有ヘッダ
- `src\Engine\Engine\Rendering\RenderSystem.cpp:935-949` (有効判定)、`:2364-2374` (ゴッドレイ)
- フロクセルを合成するシェーダ 10 本 (`deferred_light.hlsl:257-275`、`forward_lit.hlsl:173-`、`MyEngineSurface.hlsli:151-170`、`froxel_common.hlsli:106-140` の `FroxelFogOrigin` / `FroxelComposite` など)
- Inspector の説明 (`Components.cpp` の `MYE_FIELD_TIP`)、`LocalizationTable.inl`
- `src\Engine\Engine\Demo\DemoContentField.cpp`

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 31, 32。

- 31 (自動有効):
  - (a) 場をカメラの視錐台の外に置いた場面 (`autoFroxel = true`、霧あり、`FogComponent` の解析フォグとゴッドレイも on) と、同じ場面の `autoFroxel = false` の絵が `--img-diff` で差 0 (場だけのモードが場の外を 1 ビットも変えない)
  - (b) 場を視界に入れると、カメラの `froxelOn = false` のまま霧が写る (スクショ)
  - (c) `autoFroxel = false` で霧が描かれず、フロクセルのパスが走らない (Profiler かログ)
  - (d) フルモードの既存デモ (`--fog-demo`) とフロクセルを使わない既存デモ 1 本の絵が着手前と差 0
  - (e) モードに入った / 出たときの INFO がそれぞれ 1 行だけ (60 フレーム回して数える)
  - (f) Editor の Scene View (グローバルのフロクセル off) で、Play 前の霧が見える (スクショ)
- 32 (太陽光): `fogSunLight = false` の絵が sub-02 の同じ場面と差 0。true にすると、太陽の側を向いた霧が明るくなる (HG の前方散乱。太陽の方向と逆の方向の 2 枚の平均輝度を報告)。場を視界外に置き `fogSunLight = true` にしても絵が変わらない (グローバルの媒質に足していない)
- 12: replay_verify `field` PASS (描画だけの変更なので sim は不変)

## 検証コマンド

> selftest の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) を**実行しない**。ビルドと check_rules (と、selftest 以外の検証) までを行い、selftest で確かめる条件は「未実行 (sub-07 後 / sub-14 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='render,disturbance,localization'; bin\x64\Debug\Editor.exe --selftest`
- スクショ一式 (Runtime.exe / Editor.exe の Scene View)、`--img-diff`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
