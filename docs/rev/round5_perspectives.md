# Round 5 レビュー観点と検出結果の記録

## 1. 監査の目的と対象範囲
Round 5 では、エンジンの基盤およびリアルタイム動的要素でありながら、ヘッドレス自動テスト（CI や `--synth-input`）の網から外れやすい 4 つの領域を深層監査した。

- **Platform & OS**: `Win32Window.cpp`, `Input.cpp`, `CrashHandler.cpp`, `PathUtil.cpp`
- **Physics & XPBD**: `Shapes.cpp`, `ConvexCollision.cpp`, `ConvexHull.cpp`, `XpbdSolver.cpp`, `PhysicsSystem.cpp`, `CharacterController.cpp`
- **Particles & VFX**: `CpuParticleBackend.cpp`, `GpuParticleBackend.cpp`, `VfxRenderer.cpp`, `ParticleCurves.h`, `TrailStore.cpp`, `SimSnapshot.cpp`
- **UI & Font**: `UILayout.cpp`, `UIWidgets.cpp`, `UILayoutGroup.cpp`, `UIInteraction.cpp`, `FontAtlas.cpp`, `FontAtlas.h`, `FontFiles.cpp`, `FontGeometry.h`

---

## 2. 意識した監査観点

### 2.1 Platform & OS
- **OS メッセージループと再入・競合**:
  - `WndProc` ディスパッチ中のハンドラ追加・削除によるイテレータ無効化。
  - `AdjustWindowRect` と Per-Monitor V2 DPI の計算齟齬。
  - リモートデスクトップ（RDP）やペンタブレットにおける RawInput（`MOUSE_MOVE_ABSOLUTE`）の 0..65535 正規化座標とピクセル差分の混同。
  - IME 確定文字列のバッファ溢れと UTF-16 サロゲートペア破棄。
- **クラッシュハンドラの非同期シグナル安全（Async-Signal-Safety）**:
  - SEH ハンドラ内での `CreateThread` 呼び出しによる Windows ローダロック（`LdrpLoaderLock`）デッドロック。
  - マルチスレッドクラッシュ時の競合による早期 `TerminateProcess` でのダンプ切断・破損。
  - Windows における `std::filesystem::rename` の既存ファイル上書き失敗仕様。

### 2.2 Physics & XPBD
- **SAT と凸包・マニフォールド幾何**:
  - `Body` 頂点固定バッファ（64要素）に対する `.mcvx` Blob のデシリアライズ上限不整合（バッファオーバーラン）。
  - Sutherland-Hodgman 多角形クリッピングの境界値（$d=0$）での重複頂点二重生成。
  - 接触マニフォールド削減が「深度ソート降順」のみであることによる支持面から 1D 線分への退化。
  - ボックス vs 三角形の重心判定による接触法線 180 度反転（吸い込み現象）。
- **XPBD と連続衝突判定（CCD）**:
  - 粒子プール空時の `size_t` アンダーフローによる配列外アクセス即死クラッシュ。
  - シミュレーション停止時（$h \le 0$）のゼロ除算による NaN 混入と状態汚染。
  - CCD の外接球近似による「移動前接触の誤判定」に起因する CCD スキップ・壁抜け。
  - キャラクターコントローラー（CC）での複合剛体（Compound Collider）子形状走査漏れによるすり抜け。

### 2.3 Particles & VFX
- **メモリ管理と時間逆行**:
  - `ParticleFlipTilePos` での `tiles == 0` によるゼロ除算未定義動作。
  - Replay 巻き戻し・シーク時の `uint64_t` アンダーフローによるトレイル全点消滅・巨大ポリゴン帯生成。
  - `SimSnapshot` での `ParticleEmitterComponent` 生ダンプによる未初期化パディングゴミ混入（デシンク原因）。
  - 生存数 `alive` を超えた死んだ粒子の残骸バッファまるごとシリアライズによるファイル肥大化。
  - 描画側で `nowTick` を全エミッタ最新点から取得することによる静止時フェード停止とエミッタ間時刻混同。

### 2.4 UI & Font
- **幾何変換とゼロ除算・IEEE 754 NaN**:
  - `ResolveWorldBase` でのカメラ面上点（$cw = 0.0f$）におけるゼロ除算と画面全体への NaN/Inf 伝播。
  - `Clamp01` の NaN 透過によるスライダーアンカー破壊。
  - Unity case 1345471 移植部における `cellsPerMainAxis == 1`（1列グリッド）時のゼロ除算。
  - `FindNextFocus` におけるアーキタイプ走査順依存による非決定性。
  - `FontAtlas::Grow` でのパッキング順序変化によるグリフサイレント消失。
  - 旧 `UIElement` 移行ロジックのキー検査不備によるオーサリング座標消失。

---

## 3. 検出結果サマリー

| 重大度 | Platform & OS | Physics & XPBD | Particles & VFX | UI & Font | 合計 |
|---|---|---|---|---|---|
| **P0** | 3 | 3 | 2 | 1 | **9** |
| **P1** | 5 | 4 | 3 | 7 | **19** |
| **P2** | 8 | 6 | 3 | 5 | **22** |
| **P3** | 2 | 2 | 1 | 2 | **7** |
| **合計** | 18 | 15 | 9 | 15 | **57** |
