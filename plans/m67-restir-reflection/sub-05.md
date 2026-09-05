# sub-05: temporal reuse (クラス別 M 上限)

- 依存: sub-04
- 状態: OK (commit be8b4ce)
- 往復: 1

## やること

spec §4.2 の統合と §4.3 の temporal を `rt_refl.cs.hlsl` の `gRsOn != 0` 経路に足す。元計画 S3。

1. `rt_refl.cs.hlsl`: 初期 reservoir を作った後、`gRsHistValid != 0` なら
   `RtHistoryUv(gRsUseVelocity, uv, vel, mul(float4(P,1), gRsPrevViewProj), prevUv)` → `hp` →
   A の `geom` を読んで `RtReprojectValid(length(P − gRsPrevCameraPos), geom.w, N, geom.xyz, ...)` →
   妥当なら A の reservoir を unpack + A の `rpos` から `P_prev` → `p̂_q(y')` を**現フレームの V / N / α** で評価
   (`L' = normalize(xs' − P)`、スカイは `L' = xs'`、`dot(L', N) ≤ 0` なら棄却) →
   **`J = RtRestirJacobian(xs', ns', P_prev, P)`** (ユーザー判断 U4 = 厳密。spatial と同じ関数・同じ
   `[1/gRsJacobianMax, gRsJacobianMax]` 棄却。スカイは 1) → `RtReservoirMerge(r, wSum, r', p̂, mCap[cls'], J, gRsJacobianMax, rnd)`
   (spec §4.2「M を数える規則」: p̂ = 0 / J 範囲外 / 空は候補外で M 不加算 = 履歴が使えないフレームは M が 1 に戻る)。
   書き戻し前に `RtRestirClampM(r, wSum, mCap[r.cls])`。`geom` (u4) には受け側 N と `length(P − cameraPos)`、
   `rpos` (u5) には P を書く (sub-04 と同じ)。
2. `RtPasses::RenderReflection`: `RtRestirCB` に `histValid` (`slot.hasLast && slot.lastSerial + 1 == rtViewSerial &&
   prevViewProjValid`)、`useVelocity` (`gbVelocity && histValid`)、`prevViewProj` (転置)、`prevCameraPos` を詰める。
   spatial パスの後に `lastSerial = rtViewSerial; hasLast = true`。`rtReflRestir == 0` のフレームで `hasLast = false`。
   velocity SRV を rt_refl の `t16` に張る (t7-t10 = G-Buffer、t11-t15 = 前フレーム reservoir 5 枚)。
3. A6 の観測用に、`tests\actual\` へ画像を残す手順を実装メモに (下の検証コマンド)。
4. フリッカー指標の一時 Python (scratch、コミットしない): PNG 2 枚の同一領域 (鏡面パッチ = `demo_render_*` の
   画面中央付近。座標は `probe_rtdebug14.png` で水色 / 赤が映る矩形を実測して決める) の平均絶対差、および
   debug 12 画像の同領域の平均 R / G。値を実装メモに表で残す。

5. ★spec §4.5: `isfinite()` / `isinf()` を使わない (fxc X3577)。幾何不一致・J 範囲外・w ≤ 0 の棄却は
   `RtReservoirUpdate` を呼ぶ前に return (呼んだ時点で M が増える)。
6. ★sub-04 の申し送り: 入口は全部配線済み (t11-t15 = 組 A、`gRsHistValid` / `gRsUseVelocity` / `gRsPrevViewProj` /
   `gRsPrevCameraPos` / しきい値 / `gRsJacobianMax` は CB に充填済み) だが**一度も実行されていない** — sub-05 が最初の
   実走者。velocity SRV (t16) だけ未配線。p̂ の評価方向は必ず `RtRestirSampleDir(r', P)` (撃った L で評価しない)。
   シルエット際 (受け側 p̂ = 0) は仕様どおり M = 0 のまま = temporal でも積まれず 1spp フォールバック (spec §4.2)。

## やらないこと (このサブでは)

- spatial 統合 (タップ 0 のまま)。可視レイ。UI。`--rt-class-override`。
- temporal 専用の J 範囲定数 (spec §7 のリスク。S5 でユーザーが「動くと荒れる」と言ったら)。

## 触る場所 (planner の見立て)

- `assets\shaders\rt_refl.cs.hlsl` (sub-04 で作った `gRsOn` 経路)
- `src\Engine\Renderer\RayTracing\RtPasses.cpp` (`RenderReflection` の CB 充填、slot の serial 管理 — `Accumulate`
  401-490 行の `histValid` と同じ規則)
- (`rt_reproject.hlsli` は sub-04 で移動済み)

## 受け入れ条件 (このサブ)

1. ビルド緑 / selftest 緑 / check_rules 緑 / `shot_verify.bat` 21 枚緑 (A1)。
2. A14: `--rt-restir` の絵 (frame 3、凍結) を 2 回撮って `--tol 0` PASS。
3. A6-a: `--rt-anim-seed --rt-restir --rt-debug 12` を frame 3 と frame 40 で撮り、鏡面パッチ領域の平均 G が増加
   (赤 → 緑 = M が cap へ伸びる)。凍結 (`--rt-anim-seed` 無し) でも同様に伸びること (同じサンプルでも M は積む)。
4. A6-b: `--rt-anim-seed --rt-no-temporal --rt-no-svgf` で frame 40 / 41 を `--rt-restir` 有 / 無で撮り、
   鏡面パッチ領域の平均絶対差 (フリッカー) が **有 < 無**。4 枚を `tests\actual\flicker_{on,off}_{40,41}.png` に。
5. カメラが動かない静止シーンで M が cap を超えない (debug 12 が緑で飽和、それ以上に変わらない) — 3 の frame 40 と
   frame 80 の平均 G がほぼ同じ (±0.02)。
   ★3 と 5 が **`rpos` の配線の検査を兼ねる**: ヘッドレスの render-demo は受け側 (床) が静止しているので
   `P_prev == P` → J = 1 ちょうど。`rpos` が書けていない / 読む組を取り違えている / 半精度に落ちている と
   J が範囲外で temporal 候補が棄却され、**M が 1 から伸びない** (赤のまま) 形で出る。J ≠ 1 の経路そのものは
   ヘッドレスでは通らない (受け側が動かない) — selftest (A4) とユーザーの実機 (カメラ移動) だけが観測手段。
   実装メモにその旨を書く。
6. `--rt-restir` を off → on → off とフレーム途中で切り替える経路 (エディタで手動) で混線しない — reviewer の実機確認
   項目として実装メモに「`hasLast` を落とす場所」を書く。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
set C=--render-demo --deferred --rt-refl --rt-restir --rt-anim-seed --warp --no-audio --font-embedded --width 960 --height 540 --no-fxaa
cmd /c bin\x64\Release\Runtime.exe %C% --rt-debug 12 --frames 6  --shot-frame 3  --screenshot tests\actual\m12_f3.png
cmd /c bin\x64\Release\Runtime.exe %C% --rt-debug 12 --frames 41 --shot-frame 40 --screenshot tests\actual\m12_f40.png
cmd /c bin\x64\Release\Runtime.exe %C% --rt-debug 12 --frames 81 --shot-frame 80 --screenshot tests\actual\m12_f80.png
set D=--render-demo --deferred --rt-refl --rt-anim-seed --rt-no-temporal --rt-no-svgf --warp --no-audio --font-embedded --width 960 --height 540 --no-fxaa
cmd /c bin\x64\Release\Runtime.exe %D% --rt-restir --frames 41 --shot-frame 40 --screenshot tests\actual\flicker_on_40.png
cmd /c bin\x64\Release\Runtime.exe %D% --rt-restir --frames 42 --shot-frame 41 --screenshot tests\actual\flicker_on_41.png
cmd /c bin\x64\Release\Runtime.exe %D% --frames 41 --shot-frame 40 --screenshot tests\actual\flicker_off_40.png
cmd /c bin\x64\Release\Runtime.exe %D% --frames 42 --shot-frame 41 --screenshot tests\actual\flicker_off_41.png
python <scratch>\flicker.py ...   (領域の平均絶対差と平均 R/G。PYTHONIOENCODING はユーザー設定済み)
```

## 実装メモ (coder が追記)

### round 1

```
SELF_EVAL: sub-05 (round 1)
実装:
  - assets/shaders/rt_refl.cs.hlsl:CSMain — 初期 reservoir の直後に temporal 統合を追加。
    `gRsHistValid != 0` → `gRsGbVelocity.Load(gp)` → `RtHistoryUv(gRsUseVelocity, uv, vel,
    mul(float4(P,1), gRsPrevViewProj), prevUv)` → `hp = int2(prevUv * gRfOutSize)` →
    組 A の `geom` で `RtReprojectValid(length(P - gRsPrevCameraPos), geom.w, N, geom.xyz,
    gRsDepthThreshold, gRsNormalThreshold)` → `RtReservoirUnpack(t11,t12,t13)` +
    `rpos` (t15) の `P_prev` → `pHatPrev = RtRestirTargetPdf(prev.Ls,
    RtRestirSampleDir(prev, P), V, N, alpha)` → `J = RtRestirJacobian(prev.xs, prev.ns,
    P_prev, P)` (U4 = 厳密) → `RtReservoirMerge(..., RtRestirClassParams(prev.cls).z, J,
    gRsJacobianMax, RtNextRand2(seed).x)`。統合後に `RtRestirClampM(r, wSum,
    RtRestirClassParams(r.cls).z)`、その**後**で `pHat` / `r.W` を作る (採用サンプルが
    履歴側に入れ替わっていることがあるため)。
  - assets/shaders/rt_refl.cs.hlsl — `Texture2D<float2> gRsGbVelocity : register(t16)` を追加。
  - assets/shaders/rt_refl_restir_spatial.cs.hlsl:CSMain — **書き戻し前の `r.W` の作り直しを追加**
    (`pSel = RtRestirTargetPdf(r.Ls, RtRestirSampleDir(r, P), V, N, alpha)` →
    `r.W = RtRestirWeight(wSum, r.M, pSel)`)。M67d の欠落。下の [逸脱] 1 を参照。
  - src/Engine/Renderer/RayTracing/RtPasses.cpp:RenderReflection — restirOn の分岐で
    `in.gbVelocity` を t16 に張る (null もそのまま張る)。
  - src/Engine/Renderer/RayTracing/RtPasses.cpp:UnbindCompute — null SRV を 16 → **17** 本に。
  - src/Engine/Engine/RayTracing/RtSelfTest.cpp:TestRestir — **新規テスト 1 ブロック**:
    2 パスの往復 (rt_refl → spatial → 次フレーム) を CPU ミラーで 24 フレーム回し、
    (a) M が 1 フレームに 1 ずつ伸びてクラス上限 (Default=16) で止まる、(b) 推定値が Ls の
    まま (再利用で暗化・明化しない)、(c) 組 A の W を 0 にすると M が 1 から伸びない
    (= 下の [逸脱] 1 のバグを機械で落とす) の 3 点。ログ 1 行
    `restir: temporal loop M = 16 after 24 frames (cap 16)`。
仕様との差分:
  - [逸脱→修正] **spatial パスが組 A へ書き戻す `W` が常に 0 だった** (M67d の欠落)。
    `rt_refl_restir_spatial.cs.hlsl` は `RtReservoirEmpty()` (W=0) から統合を始めて
    `RtReservoirPack` するだけで W を作り直しておらず、resolve は wSum を使うので
    **M67d では 1 画素も絵が変わらず golden も A5 も緑のまま通っていた**。
    M67e の temporal は `w = p̂ · W · min(M', mCap) · J` にこの W を掛けるので、
    全候補の重みが 0 → 候補ごと棄却 → **M が永久に 1**。実測: 修正前はデバッグ 12 の
    frame 3 / 40 / 80 がすべて同じ (231,72,0) = M=1 (sub-04 の実装メモの値と一致)。
    spec §4.2 の保存表「pos の w = W」に合わせる修正なので仕様変更ではないが、
    **sub-04 のファイルを直している**ので明示する。planner の確認が要る → 不安・質問 1。
  - [追加] `RtSelfTest.cpp` に上の 2 パス往復テスト。sub-05 の「やること」には無い。
    このバグは HLSL 側にあったが、CPU ミラー (RtMath.h) で**同じ手順**を回せば落ちる
    (変異テストで確認済み) ため、恒久の防波堤として足した。CLAUDE.md の「テストを足す」
    列挙には該当しないが、GPU でしか出ない不具合を CPU で落とせる形にする価値を優先した。
  - [追加] 触ったファイル 3 本 (rt_refl.cs.hlsl / rt_refl_restir_spatial.cs.hlsl /
    RtSelfTest.cpp) の**改行コードを CRLF へ戻した**。作業ツリーの規約は core.autocrlf=true
    なので CRLF が正で、編集ツールが LF に落としていた (git が「LF will be replaced by CRLF」
    と警告)。`git diff --numstat` は変換の前後で同一 (index の内容は不変) を確認済み。
  - [未実装] W のクランプ (`kRtRestirWMax`、spec §7 のリスク) は**入れなかった**。判断の根拠は
    下の検証欄「firefly 計測」— ヘッドレスで観測できた範囲では ReSTIR on の反射バッファの
    最大輝度はむしろ**下がり** (on 227.5 / off 247.9)、孤立した高輝度画素は on/off とも 0。
    カメラが大きく動くフレームは S5 (ユーザーの実機) でしか通らないので、そこで出たら
    1 行で足せる。今入れると「効いているかどうか分からないクランプ」が残る。
  - (仕様どおり) temporal 専用の J 範囲定数は作っていない (sub-05 の「やらないこと」)。
検証:
  - MSBuild Debug / Release x64 → 両方成功、警告 0
  - `cmd /c bin\x64\Debug\Editor.exe --selftest` → **exit 0 / 全スイート ALL PASS**。
    RT スイートに新規 4 アサート (growOk / unbiasedOk / prev.M == mCap / r2.M == 1.0f) が PASS、
    ログ `restir: temporal loop M = 16 after 24 frames (cap 16)`
  - **変異テスト** (テストが本当に効くことの確認): 上のループの `s.W = RtRestirWeight(...)` を
    `s.W = 0.0f` に差し替えてビルド → `restir: temporal loop M = 1 after 24 frames` +
    **growOk / prev.M == mCap が FAIL** (unbiasedOk は PASS のまま = 「絵は変わらないが
    再利用が死ぬ」という実際の壊れ方をそのまま再現)。確認後に元へ戻して再ビルド・再実行。
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
  - `tools\shot_verify.bat` → **21 枚全 PASS** (受け入れ条件 1 / A1)。`demo_render_rtrefl` /
    `demo_render_rtgi` は tol=0 で maxDiff=0 diffPixels=0 anyDiff=0/518400 = ReSTIR off は
    M67d 以前とビット一致のまま。**最終ビルド + 改行正規化の後にもう一度回して同結果**
  - 受け入れ条件 2 / A14: `--render-demo --rt-refl --rt-restir` frame 3 を 2 回 → tol=0 PASS。
    frame 40 (temporal が cap 近くまで積んだ状態) を別プロセスで 3 回 → tol=0 PASS。
    `--acoustic-demo` frame 120 を 2 回 → tol=0 PASS
  - 受け入れ条件 3 / A6-a (`--rt-anim-seed --rt-restir --rt-debug 12`、鏡面パッチ矩形
    (330,255)-(470,400) の平均 G、0-255):

    | 条件 | frame 3 | frame 40 | frame 80 |
    |---|---|---|---|
    | `--rt-anim-seed` | **78.98** | **130.95** | **131.03** |
    | 凍結 (既定) | **78.94** | **130.31** | (未計測) |

    修正前 (spatial の W が 0 のまま) は **frame 3 / 40 / 80 とも 78.9 で不動**だった。
    画像の色分布でも frame 40 は黄 (231,231,0) 20946 px = M=16 (Default の cap) +
    緑 (0,231,0) 1596 px = M=32 (Prop の cap) が出ており、**クラス別 M 上限が実際に効いている**。
    凍結シードでも同じように伸びる (同じサンプルを積んでも M は進む) ことを確認。
  - 受け入れ条件 4 / A6-b (`--rt-anim-seed --rt-no-temporal --rt-no-svgf`、frame 40 と 41 の
    平均絶対差 = フリッカー指標、0-255):

    | 領域 | ReSTIR on | ReSTIR off | 比 |
    |---|---|---|---|
    | 鏡面パッチ (330,255)-(470,400) | **0.20609** | **0.91335** | 0.226 |
    | ジオメトリ全体 (40,180)-(680,460) | 0.16945 | 0.53451 | 0.317 |
    | 画面全体 | 0.05868 | 0.18720 | 0.313 |

    **有 < 無** (4.4 倍の低減)。4 枚は `tests\actual\flicker_{on,off}_{40,41}.png`
  - 受け入れ条件 5 (M が cap で飽和する = `rpos` の配線検査): frame 40 と frame 80 の平均 G は
    130.946 と 131.028、差 0.082/255 = **0.0003** (許容 ±0.02 の 1/60)。
    ★静止シーンなので `P_prev == P` → `J = 1` ちょうど。`rpos` が書けていない / 読む組を
    取り違えている / 半精度に落ちていると J が `[0.1, 10]` の外に出て temporal 候補が
    棄却され「M が赤のまま」になるので、この 2 つの計測が **rpos の配線検査を兼ねている**。
    **J ≠ 1 の経路 (カメラ・受け側が動く) はヘッドレスでは通らない** — 観測手段は
    selftest (A4 の Jacobian ケース) とユーザーの実機だけ。
  - firefly / W オーバーフロー (spec §7 の判断材料): `--rt-debug 11`
    (`--rt-no-temporal --rt-no-svgf`、frame 40) の on / off を数値比較 →
    平均 7.096 / 6.991 (+1.5% = 「M を数えない側の偏り = わずかに明るい」と整合)、
    **最大輝度 227.5 (on) < 247.9 (off)**、近傍最大より 40 レベル以上明るい孤立画素は
    **on / off とも 0**。合成後 (frame 40/41、4 枚) でも孤立画素 on=2 / off=3。
    `--acoustic-demo` frame 120 でも孤立画素 0。→ クランプは入れない (上の [未実装])
  - GPU 時間 (WARP / Release / **frames 20** の計測 run、`[rt]` ログ):

    | シーン | refl | denoise | restir |
    |---|---|---|---|
    | render-demo `--rt-restir` | 5.407 ms | 29.071 ms | **2.433 ms** |
    | render-demo off | 4.845 ms | 19.944 ms | 0.000 ms |
    | acoustic-demo `--rt-restir` | 10.253 ms | 17.951 ms | **2.034 ms** |
    | acoustic-demo off | 7.027 ms | 14.802 ms | 0.000 ms |

    sub-04 の同条件は render-demo restir 1.498 / acoustic 1.526 ms。spatial パスに
    W の作り直し (VNDF pdf 1 回) が増えたぶんと WARP の run-to-run の幅の合算。
    GpuTimer は M67 で触っていない
  - Debug ビルド (D3D11 デバッグレイヤ on) で `--rt-refl --rt-restir` を実走 →
    **D3D の警告・エラー 0** (t16 の追加と UnbindCompute 17 本で SRV/UAV ハザード無し)
  - fxc の警告: `rt_refl.cs.hlsl` / `rt_refl_restir_spatial.cs.hlsl` とも
    `[INFO] shader compiled:` のみでログに警告なし (X3577 / X4714 は出ていない)
  - `Editor.exe --render-demo --deferred --rt-refl --rt-restir --screenshot` → exit 0、
    restir 0.194 ms (エディタ経路 = viewKey 別スロットでも動く)
  - **未実行**: `replay_verify.bat` (sim 非接触。A10 は sub-07)、`collab_verify` /
    `crash_verify` / `net_verify` (無関係)、受け入れ条件 6 の off → on → off の手動トグル
    (GUI。ヘッドレスで切り替える口が無い = reviewer の実機確認項目。落とす場所は下の申し送り)
自己採点 (1-5):
  仕様適合: 4 — sub-05 の 1〜6 を実装し、受け入れ条件 1〜5 を機械で満たした。
    4 にしたのは (a) sub-04 のファイル (spatial シェーダ) を直している [逸脱→修正] が
    1 件あり planner の確認が要る、(b) spec §7 の「W クランプを入れるか」を
    「入れない」と決めた根拠がヘッドレスの静止カメラ計測に限られる (動くカメラは S5 待ち)、
    (c) 受け入れ条件 6 が GUI で未実行、の 3 点。
  正しさ: 4 — golden 21 枚 tol=0 / selftest 全緑 (新規 4 アサート + 変異テストで有効性を確認) /
    A14 は 3 シーン条件で tol=0 / M の伸びと飽和とフリッカー低減を画素で実測 /
    D3D デバッグレイヤ 0 まで確認した。5 にしないのは **J ≠ 1 の経路 (カメラや受け側が
    動くフレーム) を 1 度も実走していない**ため — CPU ミラーの Jacobian テスト (A4) は
    通っているが、GPU で `rpos` に「動いた P_prev」が載る経路の観測はユーザーの実機のみ。
  コード品質: 4 — 再投影は rt_temporal と同じ `rt_reproject.hlsli` の関数を呼ぶだけにし、
    棄却は全部 `RtReservoirUpdate` を呼ぶ前 (= M を数えない) に置いた。踏んだ罠
    (W の作り直し / ClampM と W の順序 / t16 は on のときだけ張る) を日本語コメントに残した。
    5 にしないのは、履歴 UV の解像度に `gRfOutSize` を使っており CB 側の `gRsOutSize` と
    値は同じだが出所が 2 つある点 (コメントで補ってはいる)。
  テスト: 4 — 新規に恒久テストを 1 ブロック足し、**変異テストで「壊すと落ちる」ことまで確認**
    した。5 にしないのは、このテストが CPU ミラーの往復であって GPU の配線 (t11-t16 /
    組 A・B の取り違え) 自体は固定していないため — そこは sub-07 の ReSTIR on golden が担う。
不安・質問:
  1. **spatial パスの `r.W` 作り直し (sub-04 のファイルへの修正) を承認してほしい。**
     spec §4.2 の保存表は「pos の w = W」なので仕様どおりに直しただけだが、
     sub-04 は VERDICT OK 済みなので明示的に確認したい。これを入れないと sub-05 の
     temporal は**構造的に成立しない** (全候補の重みが 0)。
  2. **W クランプ (`kRtRestirWMax`) を入れない判断でよいか** (spec §7 は「sub-05 の実測で
     決める」)。ヘッドレス (静止カメラ) では firefly の兆候ゼロ。カメラが動くフレームは
     S5 でユーザーが見る領域なので、そこで出たら M67h で足す、が私の推奨。
  3. `--rt-restir` を on にしたまま `--rt-no-temporal` を渡すと **SVGF の履歴だけ**が切れて
     ReSTIR の temporal は生きる (spec §2 S5 の設計どおり) が、A5 の注記
     「チューニング UI で全クラスの M 上限を 1 にすれば sub-04 と同等」は sub-06 の UI が
     入るまで実行できない。sub-05 では「temporal が常に効く」前提で A5 を再検証していない
     (spec §5 A5 の但し書きどおり)。この理解で合っているか。
触ったファイル:
  - assets/shaders/rt_refl.cs.hlsl
  - assets/shaders/rt_refl_restir_spatial.cs.hlsl
  - src/Engine/Renderer/RayTracing/RtPasses.cpp
  - src/Engine/Engine/RayTracing/RtSelfTest.cpp
  - plans/m67-restir-reflection/sub-05.md (この実装メモ)
申し送り:
  - (→ sub-06 / reviewer) **`hasLast` を落とす場所は 3 か所**、どれも
    `RtPasses.cpp::RenderReflection` / `EnsureReservoirs` の中:
    (1) `restirOn` が false のフレーム (トグル off / シェーダ未コンパイル / 確保失敗) →
        `slot.hasLast = false`、(2) 内部解像度が変わった → `EnsureReservoirs` が
        `slot.hasLast = false`、(3) spatial を走らせられなかった (`restirRan == false` かつ
        `restirOn`) → 組 A が更新されていないので `slot.hasLast = false`。
    さらに `rsHistValid` は `slot.lastSerial + 1 == view.rtViewSerial` と
    `view.prevViewProjValid != 0` も要求する (= 1 フレームでも飛んだら履歴を使わない)。
    スロットは viewKey 別 (`HistorySlot(view.rtViewKey, 4)`) なので SceneView と GameView は
    混線しない。**reviewer の実機確認**: エディタで ReSTIR を off → on → off と切り替えて
    デバッグ 12 が「赤に戻ってから伸び直す」ことを見る (混線していれば緑のまま復帰する)。
  - (→ sub-06 / 07、minor) ReSTIR をトグルしても **SVGF 側の履歴 (`reflHist_`) は落ちない**
    ので、切り替え直後の数フレームは「ReSTIR 前の蓄積」と「ReSTIR 後の値」が混ざる。
    どちらも同じ量の推定量なので数フレームで収束する = 実害は無いと判断して触っていない。
    気になるなら `restirRan` が前フレームと変わったときに `reflHist_[...].hasLast = false`
    を足すのが最小 (1 行 + bool 1 個)。M67d からの性質で sub-05 が作ったものではない。
  - (→ sub-06) temporal で M が積まれた状態が spatial の入力になる = **タップを足すと
    M の伸び方が変わる** (`min(M', mCap)` が cap に張り付く速度が上がる)。デバッグ 12 の
    平均 G を sub-06 の前後で比べると「空間タップが実際に効いているか」が読める
    (計測スクリプトは使い捨てなのでコミットしていない。矩形は (330,255)-(470,400)、
    指標は「平均 G」と「連続 2 フレームの平均絶対差」の 2 つだけ)。
  - (→ sub-06) 候補の M 上限に使うクラスは **候補側 (映っている物体) の `cls`**
    (`RtRestirClassParams(prev.cls).z`)。書き戻し前の `RtRestirClampM` だけが
    **採用サンプルの `cls`** を使う。spatial のタップでも同じ使い分けにすること。
  - (→ sub-07) `--rt-restir` の絵は M67d から**変わった** (temporal が効くようになった)。
    ReSTIR on の golden (`demo_render_rtrefl_restir`) を撮るのは sub-07 なので、
    撮る前に「M が cap まで伸びた状態を撮るのか (frame 3 では M ≈ 4 にしかならない)」を
    決めること。frame 3 の既定条件で撮ると temporal の被覆は 4 フレームぶんしかない。
  - (→ sub-07) `[rt]` ログの restir は 1.5 → 2.4 ms (WARP、render-demo、frames 20) に増えた。
    ADR に数字を書くならこちら。reservoir のメモリは sub-04 から変わらない (5 枚 × 2 組)。
  - (→ reviewer) 画像は `tests\actual\` (gitignore、再撮影可): `m12_f3` / `m12_f40` /
    `m12_f80` / `m12frz_f3` / `m12frz_f40` (デバッグ 12) / `flicker_{on,off}_{40,41}` /
    `d11_restir_{on,off}` (反射バッファの on/off) / `m67e_a14_f40_{1,2,3}` (A14) /
    `m67e_ac_{1,2}` (acoustic の A14) / `m67e_editor` (エディタ経路)。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-05)。受け入れ条件 1〜5 を SELF_EVAL の検証欄で確認 (golden 21 枚 tol=0 /
  A14 を frame 3・40・acoustic 120 の 3 条件で tol=0 / A6-a 平均 G 78.98 → 130.95 → 131.03、黄 (M=16) 20946 px + 緑 (M=32)
  1596 px = クラス別上限が効く / A6-b 鏡面パッチ 0.206 vs 0.913 = 0.226 倍 / 飽和差 0.0003)。条件 6 (手動トグル) は
  reviewer の実機へ。planner が `rt_refl_restir_spatial.cs.hlsl:114-118` (pack 前の W 再計算)、`rt_refl.cs.hlsl:174-217`
  (Merge → ClampM → W の順、t16)、`RtPasses.cpp:308 / 734 / 840` (`hasLast` を落とす 3 箇所)、`nullSrvs[17]`、
  selftest 984-1045、3 ファイルの CRLF (bare LF 0) を読んで裏取り。
  [逸脱→修正] sub-04 の W = 0 → **承認** (sub-04 のフィードバック履歴に事後記録。planner の見落とし)。
  [追加] 2 パス往復 selftest → 承認 (変異テストで有効性まで確認)。[追加] CRLF 正規化 → 承認 (規約どおり)。
  [未実装] W クランプ → 承認 (兆候ゼロ。spec §7 を「入れない。出たら S5 → M67h」に更新)。
  不安 3 (A5 は sub-05 で再検証しない) → 理解どおり。
  申し送りの「SVGF 履歴は ReSTIR トグルで落ちない」→ spec §7 に既知の minor として記録 (M67d からの性質、触らない)。
