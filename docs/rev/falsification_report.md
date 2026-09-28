# レビュー指摘事項の厳密な反証（Falsification）検証報告書

- 検証日時: 2026-09-28
- 対象ファイル: エンジン全域（Core, Platform, Physics, Particles, UI, Renderer, PostProcess）
- 格納場所: `C:\HAL\MyEngin\docs\rev\falsification_report.md`

---

## 1. 反証の目的と方法
AGENTS.md 第6章「6. 自分の結論を反証する。」「7. 実際の経路で確認する。」に基づき、これまでのレビューで検出された重大欠陥（P0/P1）に対し、以下の3つの観点から徹底的な反証（反例・救済の探索）を試みた。

1. **先行ガードの有無**: 局所的なコードで未防護に見えても、関数の呼び出し前や上位ループで既に値がクランプ・検証されていないか？
2. **実際の呼び出し経路（Callsite）の追跡**: その関数を実際に呼んでいる全コードパスを調査し、危険な値が本当に渡り得るか？
3. **OS / コンパイラ / API 仕様の検証**: OS API（Win32, DirectX 11, MSVC STL）の実際の挙動やコンパイラ出力（DXBC アセンブリ、実機バイナリ実行）と照合し、仕様の誤解がないか？

---

## 2. 反証結果まとめ一覧

| 対象コード | 当初の指摘 | 反証結果 | 最終判定 |
|---|---|---|---|
| `PathUtil.cpp:133` | `std::filesystem::rename` が Windows で既存ファイル上書きに失敗する（P1） | **完全反証**: MSVC C++20 STL は内部で `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` を呼んでおり、実機テストでも既存ファイルを完全に上書きできることを実証。古い C 言語 `rename()` の仕様との混同。 | **誤検知 (False Positive) に修正** |
| `PhysicsSystem.cpp:4286` | XPBD 粒子プール空時の `size_t` アンダーフロー即死（P0） | **完全反証**: 同関数内の先行アタッチループ（2342行目）で `if (... || pool.px.empty() || ...) { pool.attachValid = 0; continue; }` により、空プールは事前に除外され `xpbdAttachBody = -1` となるため、4286行目には絶対に到達しない。 | **誤検知 (到達不能) に修正** |
| `ParticleCurves.h:989` | `ParticleFlipTilePos` で `tiles == 0` の剰余算ゼロ除算即死（P0） | **完全反証**: C++ 側呼び出し元はユニットテストのみ。HLSL 側呼び出し元も C++ 描画側（`CpuParticleBackend.cpp:857`, `GpuParticleBackend.cpp:1032`）で `std::max(1, flipTiles)` の先行ガードが完璧に敷かれており、実動パイプラインでは 0 は絶対に渡らない。 | **実動上は誤検知 (P3 改善提案に修正)** |
| `FrustumCull.h:14` | `ComputeCascadeSplits` の `nearZ <= 0` で NaN 発生（P0） | **一部反証**: 唯一のエンジン呼び出し元（`RenderSystem.cpp:393-396`）において、射影行列から復元された `nearZ > 0` および `farZ >= nearZ + 1.0f` が渡されるため、通常レンダリングでは踏まない。関数単体の境界値脆弱性。 | **P2（境界値防御欠落）に格下げ** |
| `ConvexCollision.cpp:110` | 凸包の頂点数 64 超でバッファオーバーラン（P0） | **一部反証**: 内部の `BuildConvexHull`（Quickhull）は 64 頂点で打ち切るため正常データでは踏まない。ただし `DeserializeConvexHull` は $10^6$ 頂点まで許容するため、破損ファイルや外部 Blob ロード時に発現する。 | **P1（ファイル入力セキュリティ脆弱性）に修正** |
| `UILayout.cpp:165` | `ResolveWorldBase` での $cw = 0$ ゼロ除算（P0） | **反証失敗 (真の欠陥)**: `clampToScreen == true` のとき背面判定を素通りし、カメラ視点平面上（$cw = 0$）を通過する瞬間に `cx / -0.0f` でゼロ除算が発生。先行・後続ガードは一切なく、画面全体へ NaN/Inf が伝播する。 | **真正の P0 (True Positive 確定)** |
| `CrashHandler.cpp:319` | ハンドラ内 `CreateThread` のローダロックハング（P0） | **反証失敗 (真の欠陥)**: Windows OS 仕様として、`CreateThread` はスレッド初期化時に必ず `LdrpLoaderLock` を取得し全 DLL の `DllMain(DLL_THREAD_ATTACH)` を呼ぶ。ローダロック保持中クラッシュ時に 20 秒間完全ハング・二次クラッシュする。 | **真正の P0 (True Positive 確定)** |
| `Win32Window.cpp:177` | `HandleMsg` 再帰によるイテレータ無効化（P0） | **反証失敗 (真の欠陥)**: ハンドラコールバック内で同期 Windows メッセージが飛ぶと、同一スレッドで `WndProc` が再入実行され、走査中の `handlers_` に対する再確保・イテレータ無効化クラッシュが発生する。 | **真正の P0 (True Positive 確定)** |
| `Input.cpp:56` | `MOUSE_MOVE_ABSOLUTE` の 0..65535 スケール誤認（P1） | **反証失敗 (真の欠陥)**: Windows RawInput の絶対座標モードは 0..65535 の正規化座標であるため、1px 移動で約 34 カウントが加算され、カメラが超高速暴走・カーソルロック振動を起こす。 | **真正の P1 (True Positive 確定)** |
| `VfxRenderer.cpp:98` | `TrailStore` 時間巻き戻し時の符号なしアンダーフロー（P0） | **反証失敗 (真の欠陥)**: ロールバックやエディタのシークで過去の tick に戻った際、`tick < pts.tick` となり、符号なし 64 ビット整数の減算で天文学的数値へアンダーフローし、全点が即死消滅する。 | **真正の P0 (True Positive 確定)** |

---

## 3. 反証を通じて得られた教訓

1. **「局所的なスニペット読解」の危険性**:
   `PhysicsSystem.cpp:4286` のように、ある 1 つのブロックだけを見ると「境界チェックがない」ように見えても、関数冒頭のループで事前に完全に除外されているケースが存在する。**必ず関数全体の制御フローとデータ依存関係を上流から下流までトレースしなければならない**。
2. **「API / 言語仕様の思い込み」の是正**:
   `std::filesystem::rename` のように、「Windows の rename は上書きできない」という古い C 言語知識に引きずられ、現代の MSVC STL（C++20）の内部実装（`MoveFileExW` + `MOVEFILE_REPLACE_EXISTING`）を実機検証せずに誤検知を生み出していた。**API の仕様を語る際は、ドキュメントと実機テストによる証拠確認が不可欠である**。
3. **「純関数単体の堅牢性」と「システム全体の欠陥」の峻別**:
   `ComputeCascadeSplits` や `ParticleFlipTilePos` のように、ヘルパー関数単体としてはゼロ除算ガードが欠落していても、呼び出し元のシステム側で既にガードされている場合、重大度は P0 ではなく P2/P3（防御的プログラミングの推奨）に位置づけるべきである。
