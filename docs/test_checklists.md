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
