# Round 6 レビュー観点と検出結果の記録

## 1. 監査の目的と対象範囲
Round 6 では、レンダラーの詳細パス、シャドウマッピング、ボリュメトリック、ポストプロセス、テンポラルフィルタなど、最新のグラフィックスパイプラインを深層監査した。

- **Pass & Path 基盤**: `DeferredPath.cpp`, `DeferredPath.h`, `ForwardPath.cpp`, `ForwardPath.h`
- **Shadows & Atlas**: `ShadowPass.cpp`, `ShadowPass.h`, `ShadowAtlas.cpp`, `ShadowAtlas.h`, `FrustumCull.h`
- **Froxel & Volumetrics**: `FroxelPass.cpp`, `FroxelPass.h`, `VolumeTexture.cpp`, `SkyboxPass.cpp`
- **PostProcess & Temporal**: `PostProcess.cpp`, `PostProcess.h`, `TaaPass.cpp`, `TaaPass.h`, `HzbPass.cpp`, `postfx_taa.hlsl`, `hzb_reduce.cs.hlsl`, `ssr_trace.hlsl`, `postfx_hist_reduce.cs.hlsl`

---

## 2. 意識した監査観点

### 2.1 テンポラルフィルタと IEEE 754 NaN 伝播（Temporal Explosions）
- TAA（`TaaPass`, `postfx_taa.hlsl`）における 3x3 近傍クランピングの NaN 透過性。
- 入力 HDR シーンに 1 ピクセルでも NaN が混入した場合の履歴バッファへの永続感染と画面全体への拡散。

### 2.2 幾何計算・視錐台とシャドウスプリットの特異点
- CSM（カスケードシャドウ）のスプリット計算（`ComputeCascadeSplits`）での `nearZ <= 0` や `farZ <= nearZ` におけるゼロ除算および負数累乗による NaN 混入。
- 視錐台カリング（`FrustumCull.h`）における法線未正規化時の縮退平面。

### 2.3 GPU コンピュートとアンダーフロー・TDR
- HZB リダクション（`hzb_reduce.cs.hlsl`）におけるテクスチャ寸法 0 での `uint` アンダーフローおよび 42 億回ループによる GPU TDR。
- 自動露出縮約（`postfx_hist_reduce.cs.hlsl`）における `gAeSpeed` 異常値による指数関数オーバーフローと `gExposure[0]` の永久 NaN 化。

### 2.4 ポストプロセスターゲットキャッシュと状態分離
- `PostProcess::Acquire` における解像度変更時の大量 VRAM 破棄・再生成ストール。
- 自動露出バッファ（`exposureBuf`）がビュー単位でなく解像度キーに紐付いていることによるリサイズ時の露出フリッカー。

---

## 3. 検出結果サマリー

| 重大度 | パス・シャドウ | ボリュメトリック | ポストプロセス・TAA | 合計 |
|---|---|---|---|---|
| **P0** | 1 (`ComputeCascadeSplits`) | 0 | 1 (`postfx_taa.hlsl`) | **2** |
| **P1** | 0 | 1 (`FroxelPass`) | 3 (`postfx_hist_reduce`, `PostProcess`, `hzb_reduce`) | **4** |
| **P2** | 1 (`DeferredPath`) | 0 | 1 (`ssr_trace.hlsl`) | **2** |
| **P3** | 0 | 0 | 1 (`PostProcess` 定数) | **1** |
| **合計** | 2 | 1 | 6 | **9** |
