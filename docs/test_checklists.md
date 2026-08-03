# 手動テスト手順

自動化済みの検証 (`--selftest`, `tools\replay_verify.bat`, `tools\check_rules.ps1`) に加えて、
対話的な機能はこの手順で確認する。ウォッチャー/ローダ周りを変更したら必ず再実施すること。

## M3: シェーダ / アセット / シーンのホットリロード

- [ ] 実行中に `assets/shaders/forward_lit.hlsl` を編集 → 1 秒以内に見た目へ反映
- [ ] `common.hlsli` を編集 → forward_lit / deferred 系など依存シェーダ全部が再コンパイル
- [ ] 構文エラーを保存 → Console に赤エラー (ファイル/行)、旧シェーダで動作継続 → 修正で復帰
- [ ] `assets/textures/test.png` を上書き → 地面のテクスチャが変わる
- [ ] `assets/models/BoxTextured.glb` を上書き → メッシュ/テクスチャが変わる (エンティティは不変)
- [ ] シーンを保存 → VS Code で JSON の position を編集 → 実行中のオブジェクトが動く
- [ ] JSON を壊して保存 → 警告のみでエンジン継続 → 直すと反映

## M4: GameLogic.dll のホットリロード

- [ ] Play 中に `Rotator.cpp` の定数を変更 → GameLogic のみビルド → 1-2 秒で挙動変化
- [ ] 状態保持: `angleDeg` が飛ばない / 「Rotator started」が再ログされない
- [ ] フィールド追加 → 「layout migrated」ログ + Inspector に新フィールド (既存値は保持)
- [ ] フィールド削除 → クリーンにリロード
- [ ] VS デバッガをアタッチしたままリロード → 新コードのブレークポイントが効く
- [ ] 5 回以上連続リロード → クラッシュ/リークなし (タスクマネージャでワーキングセット確認)
- [ ] ビルドエラーの DLL (リンク失敗) → 旧ロジックのまま継続

## M5/M6.5: 切替系

- [ ] Particle Settings: CPU ⇔ GPU 切替でクラッシュなし (パーティクル再スタート)
- [ ] Compare mode: 2 つの雲が同じ動き / ms 表示 / SIMD トグルで CPU 時間変化
- [ ] View > Render Path: Forward ⇔ Deferred 切替で見た目一致、繰り返してもリークなし
  (Debug 実行で終了時の D3D レポート確認)

## M46: ハイブリッド・パストレーシング (RT GI)

前提: **Deferred パスのみ**効く。Forward / AssetPreview では自動的に無効。
CLI は Editor / Runtime 共通で `--rt-gi` `--rt-debug N` `--rt-no-temporal` `--rt-no-svgf`
`--rt-freeze-seed` `--rt-anim-seed`。

### 非干渉 (既定 off) — サブごとに必須

- [ ] 何も触らずに `Runtime.exe --deferred --replay-verify cache\golden.rep --shot-frame 3
      --screenshot a.png` を**変更前後のバイナリ**で撮り `fc /b` でバイト一致
- [ ] 同じことを `--deferred` 無し (Forward) でも実施 — `common.hlsli` を触ったら必ず
- [ ] RT off のまま `--selftest` / `tools\replay_verify.bat` / `tools\check_rules.ps1` が全 PASS

### RT GI の合成 (M46f)

- [ ] View > Rendering > **RT GI (Deferred)** を on/off → SceneView が即座に切り替わる
      (off で BVH の構築も走らない = Profiler の `rt bvh` 行が消える)
- [ ] **明るさの段差が無いこと**: スカイ (= IBL) のあるシーンで on/off し、遮蔽の無い
      開けた床の輝度がほぼ変わらない (GI は IBL 拡散項と同次元の入射放射輝度で置換される)
- [ ] 色移り: 有色の床の上に置いた白い箱の**影側の面**に床の色が回り込む
- [ ] SSAO 併用: GI on では拡散環境項に AO が掛からない (二重遮蔽にならない)。
      IBL スペキュラ項には従来どおり掛かる
- [ ] Unlit / Wireframe 表示モードでは GI が掛からない (環境項が定数のまま)
- [ ] SceneView と GameView を同時に開いても互いのノイズ/履歴が混線しない
- [ ] Debug 実行で D3D デバッグレイヤの警告 0 (GBuffer を CS の SRV で読むため
      RTV のアンバインド漏れがあるとここに出る)

### 品質 A/B (計測時の注意)

- [ ] `--rt-no-temporal` で 1spp の生ノイズ / 外すと均される
- [ ] `--rt-no-svgf` で蓄積のみ / 外すとエッジを保ったまま平滑化
- [ ] **スクリーンショットは自動でシード凍結される** — デノイズの効きを写したいときは
      `--rt-anim-seed` を併用する (付けても `--replay-verify` ならフレーム決定的)
