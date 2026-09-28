# コードレビュー検出事項の再確認・実コード走査検証報告

- 検証日時: 2026-09-28
- 対象ファイル: エンジン全域（Core, Platform, Physics, Particles, UI, Renderer, PostProcess）
- 格納場所: `C:\HAL\MyEngin\docs\rev\review_verification.md`

---

## 1. 走査・再検証の目的
Round 1〜6 で検出された計 178 件（P0: 34件、P1: 51件など）の指摘について、実コードの行番号、前後の文脈、ガード節の有無、他の場所での対策の有無を直接照合し、**「指摘の妥当性」「誤検知（False Positive）の有無」「真の危険度」** を厳密に再確認・走査した。

---

## 2. 主要 P0 / P1 欠陥の実コード直接照合結果

| 対象 | ファイル・行番号 | 指摘内容 | 実コード照合・再検証結果 | 判定 |
|---|---|---|---|---|
| **Core/ECS** | `World.cpp:716-722` | `rec.row` 上限検査欠落 | `rec.archIndex` のみ検査し `rec.row < count` の検査が完全にゼロ。存在しない行番号で境界外アクセス確定。 | **妥当 (P0確定)** |
| **Core/ECS** | `World.cpp:705` | `freeIndices` 範囲未検証 | `r.PodVector<uint32_t>()` で読み取った後、上限検査なしでそのままメンバへ格納。二重アロケーション確定。 | **妥当 (P0確定)** |
| **Platform** | `CrashHandler.cpp:319` | ハンドラ内 `CreateThread` のローダロックハング | `MiniDumpWriteDump` のスタック枯渇対策として動的生成。ローダロック保持クラッシュ時に 20 秒間完全ハング・二次クラッシュ確定。 | **妥当 (P0確定)** |
| **Platform** | `Win32Window.cpp:177-182` | `HandleMsg` 再帰によるイテレータ無効化 | ハンドラコールバック内で同期メッセージが飛ぶと再入が発生し、イテレータ無効化クラッシュ確定。 | **妥当 (P0確定)** |
| **Platform** | `PathUtil.cpp:133-138` | `std::filesystem::rename` の上書き失敗 | Windows の `MoveFileExW` は `MOVEFILE_REPLACE_EXISTING` が無いと既存ファイル上書きに失敗し `false` を返す。 | **妥当 (P1確定)** |
| **Platform** | `Input.cpp:56-65` | `MOUSE_MOVE_ABSOLUTE` の 0..65535 スケール誤認 | RawInput 絶対座標をピクセル差分と同一視。FHD で 34 倍の速度暴走およびカーソルロック振動ループ確定。 | **妥当 (P1確定)** |
| **Physics** | `ConvexCollision.cpp:110, 551` | 頂点 64 固定配列のオーバーフロー | `out.vertCount` は 64 にクランプするが `t.faceVerts` はクランプされず、65 頂点以上の凸包で `b.vx[i0]` 境界外アクセス確定。 | **妥当 (P0確定)** |
| **Physics** | `PhysicsSystem.cpp:4286` | XPBD 粒子プール空時の `size_t` アンダーフロー | `pool.px.size() - 1` が `SIZE_MAX` になり、`XpbdSolver.cpp:99` で境界チェックなしに `pool.px[SIZE_MAX]` を読んで即死クラッシュ確定。 | **妥当 (P0確定)** |
| **Physics** | `Shapes.cpp:1435-1442` | `BoxTriSat` 重心判定による法線 180 度反転 | ボックス中心が原点で三角形重心との内積で判定しているため、ボックスが床をわずかに貫通した瞬間に法線が反転し床内へ吸い込まれる。 | **妥当 (P1確定)** |
| **Particles** | `ParticleCurves.h:994` | `tiles == 0` での剰余算ゼロ除算 | `frame % tiles` で CPU ゼロ除算例外（`0xC0000094`）発生、プロセス即死確定。 | **妥当 (P0確定)** |
| **Particles** | `VfxRenderer.cpp:98` | `TrailStore` 巻き戻し時の `uint64_t` アンダーフロー | 過去へのシーク・巻き戻しで `tick < pts.tick` となり天文学的数値にアンダーフローして全点即死消滅確定。 | **妥当 (P0確定)** |
| **UI** | `UILayout.cpp:165-175` | `ResolveWorldBase` での $cw = 0$ ゼロ除算 | `clamp == true` のとき背面ガードを素通りし、`cw = 0` で `cx / -cw` により ±Inf/NaN が画面全体に伝播確定。 | **妥当 (P0確定)** |
| **Renderer** | `postfx_taa.hlsl:57-78` | 3x3 カラークランプの NaN 透過・感染爆発 | IEEE 754 の `min/max` が NaN を透過し、次フレームの履歴バッファに書き込まれて再投影で画面全体へ無限拡散確定。 | **妥当 (P0確定)** |
| **Renderer** | `FrustumCull.h:14-22` | `ComputeCascadeSplits` の `nearZ <= 0` ゼロ除算 | 対数分割式で `nearZ <= 0` のとき NaN が発生し、CSM ディレクショナルシャドウが全画面で全消滅確定。 | **妥当 (P0確定)** |

---

## 3. 再走査によって得られた深層の洞察

1. **「コメントの意図」と「実コードのガード境界」の乖離**:
   - `UILayout.cpp:165` では「ゼロ除算防止」とコメントに明記されながら、その直下の `if (behind && !clamp)` により、クランプ有効時に肝心の除算ガードがバイパスされていた。
   - `CrashHandler.cpp:311` では「スタック枯渇防止のため別スレッド化」と意図が書かれながら、Windows の `CreateThread` がローダロック（`LdrpLoaderLock`）を要求するという OS 低レベル制約の考慮が欠落していた。
2. **符号なし整数（`size_t`, `uint64_t`, `uint32_t`）の減算トラップ**:
   - `size - 1`（`PhysicsSystem.cpp`）や `tick - pts.tick`（`VfxRenderer.cpp`）、`srcExtent - 1`（`hzb_reduce.cs.hlsl`）など、サイズ 0 や時間逆行という「境界条件・動的シナリオ」において、符号なし整数が `SIZE_MAX` や `UINT_MAX` に跳ね上がり、即死クラッシュや全要素消失を引き起こすパターンが複数箇所に共通して存在していた。
3. **誤検知（False Positive）の不在**:
   - 走査した主要 P0 / P1 項目において、他の箇所で暗黙にガードされているような誤検知は一切存在せず、すべて「極限入力・境界値・動的操作」によって確実に発現する本質的な欠陥であることを確認した。
