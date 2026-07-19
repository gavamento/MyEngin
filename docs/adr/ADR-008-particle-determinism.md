# ADR-008: パーティクル二重実装と決定論の分担

## 決定
- CPU 実装 (SoA + SSE) と GPU 実装 (Compute) は同一の `ParticleEmitterComponent` を解釈する
- 乱数はどちらもエンジン側の決定論 RNG (PCG32、エミッタ別ストリーム) が生成する。
  GPU では乱数を生成せず、CPU が作った放出データバッファを消費する (spec 7.3)
- ワールドハッシュ (spec 11.3) には CPU パーティクルの全状態 (SoA + RNG + 放出累積) を含め、
  GPU パーティクルは描画出力として除外する。GPU 側は比較モード (同一シード並走) で目視検証する

## 理由
- 「同じ定義データ・同じシード → 論理的に同等の結果」という spec 7.5 の要求を、
  乱数生成を CPU 側に一本化することで構成非依存に満たす
- GPU の alive 数や浮動小数の並列リダクション順はリードバックなしでは検証コストが高く、
  リプレイ検証の対象を CPU 実装に限定するのが費用対効果の均衡点

## 実装メモ
- CPU の SIMD 経路はスカラー参照実装と演算列 (mul → add の順) を一致させ、
  端数レーンもスカラーで処理 — SIMD on/off でビット一致を保つ
- GPU は dead list (append/consume) + alive list A/B 圧縮 + CopyStructureCount →
  DrawInstancedIndirect で CPU リードバックゼロ。カウンタはリソース生成時に事前初期化
- 隠しカウンタのコピー先は structured buffer 不可 (D3D 制約) — typed Buffer<uint> を使う

## トレードオフ
- 放出データの CPU 生成コスト (放出数に比例) — 放出はシミュレーションよりずっと少ないので許容
- GPU 側の厳密なハッシュ検証は非対応 (比較モードでの目視 + 更新時間計測で代替)
