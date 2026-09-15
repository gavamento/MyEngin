# harness 台帳: m76-deepmodal

- 依頼原文: "C:\HAL\MyEngin\plans\DeepModal" これを参考にDeepmodalを使った音作成をエンジンに実装する計画を立てて
- 開始: 2026-09-16 / 基点コミット: 99803eb
- フェーズ: 実装

## ユーザー判断 (策定前に確定済み)
- 再生方式: 衝突ごとに ≤2 s のクリップを合成し回転プールへ RegisterClip → 既存の Play / 3D / 遮蔽 / リバーブ経路 (ストリーミングの新レーンは作らない)
- 推論: バックエンドを抽象化。最初は C++ CPU 実装、将来 GPU Compute Shader 実装へ差し替えられる構造
- データセット: Primitive → 小規模自前 → ModelNet10 → ModelNet40 の順で拡張。**小規模 Dataset への overfit が成功するまで大規模生成を開始しない**
- 作業ツリー: 無関係の WIP を先にコミット (b4a35c0 / 1bc3c46 / a0b8802) してから master 上でハーネスを回す
- 設計案 (司会が調査して作成、planner の出発点): `plans/m76-deepmodal/design-draft.md`
- **2026-09-16 策定後の [ユーザーに聞ける] 3 件の回答**:
  - poissonRatio: 「FEM / 教師データ生成と reference material metadata 用。現行 Deep-Modal の runtime material scaling には使用しない。将来 Poisson 比を考慮するモデルへ拡張可能な形で保持する」 (planner 裁定「PhysMat に足さない」とは異なる → planner へ補足送付、spec / sub-01 / sub-07 を修正)
  - M76h (sub-08) の範囲: stage1 で .dmnet コミットまで (planner 裁定どおり)。ModelNet10 は README の手順でユーザーが回す
  - 計画の確定: 確定 (受け入れ条件 20 件 / サブ 8 本)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | a3a536d | M76a モーダル合成器 + PhysMat 音響材質 4 フィールド (依存なし) |
| sub-02 | 実装中 | 1 | | M76b ボクセライザ (.mvox) + OFF/OBJ + --modal-voxelize (依存なし) |
| sub-03 | 未着手 | 0 | | M76c Python データセット生成 + pytest + constGroups (依存 sub-02) |
| sub-04 | 未着手 | 0 | | M76d モデル / 学習 / export、overfit の門 (依存 sub-03) |
| sub-05 | 未着手 | 0 | | M76e .dmnet ローダ + CPU バックエンド + .msfm + ModalSoundLibrary (依存 sub-02, sub-04) |
| sub-06 | 未着手 | 0 | | M76f ModalSound (61) + 接触→合成 + wave 口封じ + CLI (依存 sub-01, sub-05) |
| sub-07 | 未着手 | 0 | | M76g Inspector プレビュー + PhysMat 欄 (依存 sub-06) |
| sub-08 | 未着手 | 0 | | M76h stage1 学習 + .dmnet + 文書 (依存 sub-06, sub-07) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- sub-01 nit: `ImpactSynth.h:27` のコメントが `engine_spec §10.7` を先取りで参照。sub-08 で節番号が変わったら合わせる
- 環境: `replay_verify.bat` を既定の並列 12 で回すとホストのメモリ不足でバックグラウンドごと kill されることがある → `MYE_REPLAY_JOBS=3` で回す (エンジン非依存、M75b と同じ症状)
- planner / coder のエージェント ID はこのセッション限り。再開時は `spec.md` / `sub-NN.md` / 台帳から文脈を渡して新規起動
