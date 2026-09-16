# harness 台帳: m76-deepmodal

- 依頼原文: "C:\HAL\MyEngin\plans\DeepModal" これを参考にDeepmodalを使った音作成をエンジンに実装する計画を立てて
- 開始: 2026-09-16 / 基点コミット: 99803eb
- フェーズ: レビュー (round 1 FAIL → sub-10 を実装中)

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
- **2026-09-16 sub-03 の [ユーザーに聞ける] 回答**: 固有値解法の cap + LOBPCG フォールバック (`MAX_OCCUPIED_EXACT=9000`、m=40 / maxiter=60 / spilu fill_factor=8) は「採用 + 解法を記録」= planner 裁定どおり。npz に `method` を記録して LOBPCG 由来を可視化し、sub-04 が除外・重み下げを選べる形にする。しきい値と LOBPCG パラメータの再調整は M76h で実分布を見てから
  - **追加指示 (同日)**: 「fallback の採否は solver 名ではなく固有対の residual と、基準形状での shift-invert との比較結果に基づく。各結果に solver metadata と convergence quality を保存し、学習時に除外/重み付け可能にする。」= 採否の判断材料は method 名ではなく**数値の収束品質** (residual ‖Kx − λMx‖/‖λMx‖ 等) と基準形状での直接法との照合。npz / stats.json に solver metadata + convergence quality を保存する
- **2026-09-16 sub-03 round 2 の [ユーザーに聞ける] 回答 (モード数の予算)**: 「1 で固定のモード数達成を必須条件にせず、100–10000 Hz に対する **Mel-band coverage** を各サンプルで記録する。学習時の採否・重み付けは **mode count ではなく band coverage と residual 品質**で決定する。」
  - 前提の訂正 (planner 再分析): 校正の「LOBPCG が 77% 取りこぼし」は**測定のバグ** — 直接法に 150 本、反復法に 40 本 (剛体除去後 34) を要求していた予算差。規模の違う 2 形状で 34/116 が同一、周波数一致は相対誤差 1e-12 級。両解法とも要求数で頭打ちだった (直接法も k=150 で切れていた)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | a3a536d | M76a モーダル合成器 + PhysMat 音響材質 4 フィールド (依存なし) |
| sub-02 | OK | 2 | ae77b20 | M76b ボクセライザ (.mvox) + OFF/OBJ + --modal-voxelize (依存なし) |
| sub-03 | OK | 3 | 81b5a39 | M76c Python データセット生成 + pytest + constGroups (依存 sub-02) |
| sub-04 | OK | 2 | 4239ce3 | M76d モデル / 学習 / export、overfit の門 (依存 sub-03) |
| sub-05 | OK | 1 | 82f5363 | M76e .dmnet ローダ + CPU バックエンド + .msfm + ModalSoundLibrary (依存 sub-02, sub-04) |
| sub-09 | OK | 2 | bc68463 | M76e2 CPU 推論の AVX2 / マルチスレッド最適化 (依存 sub-05、sub-06 の前) |
| sub-06 | OK | 1 | eac3800 | M76f ModalSound (61) + 接触→合成 + wave 口封じ + CLI (依存 sub-01, sub-05, sub-09) |
| sub-07 | OK | 1 | 34bf95f | M76g Inspector プレビュー + PhysMat 欄 (依存 sub-06) |
| sub-08 | OK | 2 | 5807308 | M76h stage1 学習 + .dmnet + 文書 (依存 sub-06, sub-07) |
| sub-10 | OK | 4 | f08cd95 + 7fc5aa0 | M76i 音量カーブの較正と圧縮 + engine 文書 (依存 sub-08、レビュー round 1 の major 1) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 3 / 4 / 4 / 4 | major 2 (ampScale 未校正 = 実用域で無音 + engine 文書に記録なし / Inspector プレビューが既定値で無反応・無通知)、minor 8 |
| 2 | FAIL | 4 / 4 / **5** / 4 | round 1 の指摘 1〜10 は全て解消。残 major 1 (root README 未更新)、minor 3 (統計量の定義 / アンカーの代表性 / flush の損失窓) |

- **2026-09-16 レビュー round 1 後の [ユーザーに聞ける] 回答 (音量校正)**: 「**今校正する**」。**先の『将来に回す』判断を、レビューの新しい実測に基づいて撤回**した — 判断時の情報は「peak −28〜−76 dB、無音や常時歪みは無い」だったが、reviewer の実測では**振幅が力積に線形で、1 kg を 0.5 m 落とす (J ≈ 3 N·s) と PCM が全サンプル 0**、面打ちプローブ (15 N·s) でも −84.3 dBFS = 2 LSB、焼いた 382 枚中 35 枚 (9.2%) はどんな力でも無音。`ampScale` はヘッダの float 1 つで `export.py --amp-scale` も既にあるため**再学習は不要**。「重い衝突がソフトクリップに張り付くか」= 上限圧縮の判断とセットで行う (spec §2 #12 が予見していた通り)

## 申し送り (セッション跨ぎ)
- sub-01 nit: `ImpactSynth.h:27` のコメントが `engine_spec §10.7` を先取りで参照。sub-08 で節番号が変わったら合わせる
- 環境: `replay_verify.bat` を既定の並列 12 で回すとホストのメモリ不足でバックグラウンドごと kill されることがある → `MYE_REPLAY_JOBS=3` で回す (エンジン非依存、M75b と同じ症状)
- **2026-09-16 sub-04 round 1 の [ユーザーに聞ける] 回答 (overfit の門の定義)**: 「説明率 R² + マスク精度」= planner 裁定どおり。絶対 MSE は撤回 (旧閾値 1e-3 は根拠が無く、教師場の表現上の下限 ≈0.002 を下回っていて到達不能だった。1 サンプルでも床が立つことを実測で確認)。具体値はデータのサイズ漏れ修正 + 再生成の後に再計測して planner が spec §8 で確定する
- **sub-04 round 1 で発見した実バグ**: `dataset.py` が FEM の要素寸法にメッシュ実寸を渡していた。ボクセル化は最長辺で正規化するので入力はスケール不変 = 同じ入力に異なる教師値が生まれていた (同一ボクセル列の 3 本で feat が最大 7.0 食い違う実測)。加えてランタイムの `BuildModes` が σ3 をもう一度掛けるので**二重スケール**になる (論文 §5.1 は学習時にサイズ固定・後処理で σ3 が正しい)。修正は `h_ref = L_REF/28` を全メッシュ共通で渡す
- **2026-09-16 sub-05 の [ユーザーに聞ける] 回答 (初回の裏焼き時間)**: 「**今すぐ最適化する**」= planner 裁定 (許容してネットは縮めない) とは異なり、**先に CPU 推論の最適化を入れてから sub-06 へ進む**。sub-06 は起動直後に停止させた。ネットは縮めない (門の測り直しを招く) / 正しさの安全網は fixture (許容 1e-3 が加算順の差を吸収する) / 実効 0.4 GMAC/s に対し SIMD + マルチスレッドで 10-50 倍の余地、が前提
- sub-09 nit (申し送り): `ReluRange` の NaN 時の振る舞いだけ SIMD (MAXPS は NaN なら第 2 オペランドを返す) とスカラー (`std::max(0.0f, NaN)` は 0) で違う = 理屈の上では同型の欠陥。実害なし (NaN が出る時点でモデルが壊れており fixture 照合が先に落ちる) だがコメントを 1 行残すこと / 速度の追加最適化 (cols・padded バッファの永続化、AVX2 の M ブロック幅拡大) は未実装 / `MYE_MODAL_THREADS` と `MYE_MODAL_FORCE_SCALAR` は CLI フラグではないので、M76h の文書化で「計測用の環境変数」として 1 行足す
- **sub-06 → sub-08 へ移管**: 耳確認 (面で音が変わる / 材質で減衰が変わる / 強く当てると大きい) と「絶対音量に上限圧縮を足すか」の判断。実モデル (`assets/deepmodal/deepmodal.dmnet`) が無い段階では乱数重みの fixture の音しか出ず、聴感評価が原理的に成立しないため。sub-08 の受け入れ条件 0 に入れた
- sub-06 nit (次に触るとき): `CollectModalImpacts` の index→EntityID 表が `AcousticField::DrainImpacts` の複製 (include の向きは崩れないので `acoustic::` へ寄せられる) / `AudioSourceSystem::Update` の drain ブロックのネストが深い
- **2026-09-16 sub-08 の [ユーザーに聞ける] 回答**: (1) shot_verify の 2 枚 (acoustic_forward / acoustic_deferred) は「**既存の問題として記録し別件へ**」= M76 の範囲外。M76 開始前の基点 (99803eb) をビルドして同一の失敗 (maxDiff 83/82、差分座標も一致) を再現済み。いつ壊れたかの特定は後日。**golden を撮り直さない** (原因を断定せずに塗り潰さない) (2) `ampScale` の -12 dBFS 校正は「**将来に回す**」= ModelNet10 の本学習でモデルが変われば振幅の分布も変わるため、その後に合わせる
- **sub-04 の未決 (M76h 前に決める)**: L_REF (0.3 m) / fMax (10000 Hz) の再検討。サイズ漏れ修正で全メッシュを参照サイズで解くようになり帯域内モードが減った (mode_count 中央値 30.5 → 14、coverage 0.45 → 0.336)。旧根拠はバグ入りデータの統計なので失効。L_ref はランタイムの σ3 が吸収する自由なパラメータなので「教師データが最も豊かになる値」を選んでよい。変更時は `.dmnet` の refSizeL と stage0 再生成がセット
- sub-04 nit (次に tools/deepmodal を触るとき): lobpcg の seed が solver_params に記録されていない / データセット内の distinct seed が 1 つであることの機械チェックが無い / R² は N をまたいで比較できない (var が標本ごとに違う) 旨を README に / README の実行例を forward slash に統一 (Bash がバックスラッシュを潰す)
- **sub-03 の未決 (M76h 前に決める)**: モード数の予算が足りず大きいメッシュの高域が系統的に欠ける。stage0/stage1 は coverage フィルタ既定 off で全 38 本を使う。ModelNet10 の前に「適応予算」(f_top が f_max に届くまで m/k を上げる) を入れるか決める。今フィルタを有効にすると「lobpcg のメッシュを除く」と数値的に同義になり、method で決めないという指示の趣旨に反する
- sub-03 nit: f_top と coverage はパイプラインの別の段を測っている (f_top = 生の固有解、coverage = 残差フィルタ + 未励起しきい値の後) / `calibrate.py` の budget 既定 34 は LOBPCG の m=40 − 剛体 6 に依存 (m を変えたら追随)
- planner / coder のエージェント ID はこのセッション限り。再開時は `spec.md` / `sub-NN.md` / 台帳から文脈を渡して新規起動
