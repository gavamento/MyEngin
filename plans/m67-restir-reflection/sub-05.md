# sub-05: temporal reuse (クラス別 M 上限)

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## やること

spec §4.2 の統合と §4.3 の temporal を `rt_refl.cs.hlsl` の `gRsOn != 0` 経路に足す。元計画 S3。

1. `rt_refl.cs.hlsl`: 初期 reservoir を作った後、`gRsHistValid != 0` なら
   `RtHistoryUv(gRsUseVelocity, uv, vel, mul(float4(P,1), gRsPrevViewProj), prevUv)` → `hp` →
   A の `geom` を読んで `RtReprojectValid(length(P − gRsPrevCameraPos), geom.w, N, geom.xyz, ...)` →
   妥当なら A の reservoir を unpack + A の `rpos` から `P_prev` → `p̂_q(y')` を**現フレームの V / N / α** で評価
   (`L' = normalize(xs' − P)`、スカイは `L' = xs'`、`dot(L', N) ≤ 0` なら棄却) →
   **`J = RtRestirJacobian(xs', ns', P_prev, P)`** (ユーザー判断 U4 = 厳密。spatial と同じ関数・同じ
   `[1/gRsJacobianMax, gRsJacobianMax]` 棄却。スカイは 1) → `RtReservoirMerge(r, wSum, r', p̂, mCap[cls'], J, rnd)`。
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

## フィードバック履歴
