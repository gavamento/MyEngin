# harness 台帳: m80-destruction

- 依頼原文: m80-destruction: UE の Chaos Destruction 相当の破壊物理を実装する。要件と事前調査は plans/m80-destruction/design-draft.md (要件節はユーザー確定済み)。運用: 質問は AskUserQuestion を使わず推奨で裁定し、Notion (https://app.notion.com/p/3e502024a4d481df8d01cbe69c013018) の表へ記録して先へ進む。作業ツリーの既存 WIP には触らない。
- 開始: 2026-09-25 / 基点コミット: 73c8d76277fae380161f706b6015747f1ef68166
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 2 | 54c8f1f | 閉じ判定と平面切断 + 蓋 |
| sub-02 | OK | 3 | e2175f9 | Voronoi 分割・凸包・接着グラフ。トーラス分は sub-13 へ移管 |
| sub-03 | OK | 1 | 8c9d984 | 破片資産 .mfrac と FractureLibrary |
| sub-04 | OK | 2 | 42a8dfe | ボクセル化 + surface nets。既定解像度 32。開いた箱 48/64 は sub-14 へ移管 |
| sub-05 | OK | 1 | dd46d99 | 複合の上限撤廃・形状単位インパルス |
| sub-06 | OK | 1 | 68cf2d4 | Destructible/FracturePiece・事前生成・root proxy |
| sub-07 | OK | 2 | 7b09398 | 接着の破断と塊の剛体化 |
| sub-08 | OK | 2 | 5e04bcd | 割れた後の 6 挙動 |
| sub-09 | OK | 1 | c9764c4 | Inspector・非同期焼き・Undo |
| sub-10 | OK | 3 | 5236868 | スキンメッシュの破壊 |
| sub-11 | OK | 1 | 5b33f5f | 計測・ベンチ・上限 |
| sub-12 | OK | 1 | (本コミット) | ABI v22・デモ・spec/ADR |
| sub-14 | OK | 1 | 14d8775 | 断面の三角形分割を libtess2 に置き換え (依存 sub-04、sub-09 が依存) |
| sub-15 | 未着手 | 0 | | 拡張点の確認と整理 + 位相的な閉じの再調査 (Notion 回答の反映、依存 sub-12) |
| sub-13 | OK | 1 | d873eac | 凸包生成の無限ループ修正 + トーラス焼き (依存 sub-02、sub-06 が依存) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
| ID | 論点 | 決定 | 状態 |
|---|---|---|---|
| Q-1 | 断面の描き方 | 破片ごとに `_cap` 子エンティティ + 内部マテリアル | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-2 | スキンの割れた後 | 全部剛体破片で描く (骨に剛体追従) | ユーザー回答 (Notion、2026-09-25): 推奨で確定 + 「今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-3 | 古い順に消す範囲 | Destructible ごとの maxDebris | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-4 | 荷重モデル | 接触は tick 単位判定、蓄積はスクリプトの ApplyDamage のみ | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-5 | 固定壁 | ルート kinematic、体積最大の塊が残る | ユーザー回答 (Notion、2026-09-25): 推奨で確定 + 「今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-6 | .mfrac の置き場所 | assets 側 (.meta 付き、git 共有) | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-7 | 凹んだ破片 | 凸包 1 個で近似 (非連結は別破片) | ユーザー回答 (Notion、2026-09-25): 推奨で確定 + 「今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-8 | 破壊物の単位 | MeshRenderer のメッシュ 1 つ | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-9 | ABI 範囲 | ApplyFractureDamage 1 本 + onBreak (v22 = 126) | ユーザー回答 (Notion、2026-09-25): 推奨で確定 + 「今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-10 | 破片の位相的な閉じ | 求めない (幾何的な閉じ = 体積保存 + ベクトル面積≈0)。B で継ぎ目の閉じを足す | ユーザー回答 (Notion、2026-09-25): 要再検討: 「実装は延期し再度検討/走査しそれでも無理だったら今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-11 | ボクセル化の見た目と既定解像度 | surface nets (滑らか)、既定は Release 10 秒以内の最大 | ユーザー回答 (Notion、2026-09-25): 確定 (「これでいい」) |
| Q-12 | 外部ライブラリ libtess2 の取り込み | 取り込む (external/、SGI FSL B 2.0) | ユーザー回答 (Notion、2026-09-25): 推奨で確定 + 「今後拡張する可能性があるから拡張性を意識して設計して」 |
| Q-13 | shot_verify の golden 4 枚 (parts/joints/acoustic_forward/acoustic_deferred) が M80 着手前からずれている | M80 では更新せず除外。原因調査と更新は M80 の外 | 推奨で仮決定 (Notion Q-13、回答待ち。planner の #12) |
| Q-14 | 固定の壁を撃ったときの打ち抜き | v1 は打ち抜かない (弾はその tick で止まり、破片は静止から落ちる)。打ち抜きは後回し | 推奨で仮決定 (Notion Q-14、回答待ち。planner の #13) |

## 申し送り (セッション跨ぎ)
- **運用 (2026-09-25 ユーザー指示)**: ハーネス中の質問は推奨案で仮決定して進める。質問は Notion「活動記録」のページ
  https://app.notion.com/p/3e502024a4d481df8d01cbe69c013018 の表 (Q-n) に追記し、回答は後で受け取る。
  planner は AskUserQuestion を使わず、未決事項に `[ユーザーに聞ける]` を付けて返す。司会は Notion へ転記し、
  「ユーザー判断」表に「推奨で仮決定 (Notion Q-n、回答待ち)」と書く。resume 時は Notion の回答欄を読み、推奨と違う回答は planner へ補足として送る。
- 上限 (差し戻し 3 / レビュー 3) 到達時も Notion へ記録。後続が依存しなければ当該サブを「保留」にして先へ、依存すれば中断して報告。
- **作業ツリー**: 開始時点の未コミット WIP (deepmodal 関連、`src/Engine/Renderer/WaterPass.cpp`、`tools/deepmodal/train.py`、`assets/deepmodal/*`、ルート直下の一時ファイル群) には触らない。コミットは SELF_EVAL の「触ったファイル」だけ。WIP と同じファイルを触る必要が出たらそのサブで止めて Notion に記録。
- coder は `model: "sonnet"` で起動する。
- ABI の現状は v21 / 125 スロット (メモの v16/110 は古い)。
- ルート直下の *.log (_build_round2.log 等) は 9/13〜9/23 付けで今回のハーネス以前のもの、.gitignore 済み。触らない。coder の作業ファイルはスクラッチパッドへ出す。
- **shot_verify の切り分け (2026-09-25 司会)**: 4 枚 (parts / joints / acoustic_forward / acoustic_deferred) は M80 着手前 73c8d76 と 14d8775 のクリーン worktree で、数値・worst 座標・actual PNG の MD5 まで完全一致で割れる。M80 の差分と WIP (WaterPass.cpp) は無関係。以後のサブはこの 4 枚を除外して判定する (spec §5 受け入れ条件 10)。
- **M80 外の既存問題 (ユーザーへ報告する)**: src/Engine/Engine/Asset/CookedCacheSelfTest.cpp:144-147 が固定名の一時ディレクトリ (mye_cook_selftest) を開始時に remove_all しており、Debug と Release の --selftest を同時に走らせると互いに消し合って不安定になり得る (sub-10 round 2 で同型の不安定を発見)。
- **ユーザーへ報告する**: ABI v22 に上がったので、外部プロジェクト (三校 / HAL Collector) の GameLogic.dll はエンジン更新後に再ビルドが必要 (v21 の DLL は版検証でロード拒否)。
