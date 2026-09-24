# harness 台帳: m80-destruction

- 依頼原文: m80-destruction: UE の Chaos Destruction 相当の破壊物理を実装する。要件と事前調査は plans/m80-destruction/design-draft.md (要件節はユーザー確定済み)。運用: 質問は AskUserQuestion を使わず推奨で裁定し、Notion (https://app.notion.com/p/3e502024a4d481df8d01cbe69c013018) の表へ記録して先へ進む。作業ツリーの既存 WIP には触らない。
- 開始: 2026-09-25 / 基点コミット: 73c8d76277fae380161f706b6015747f1ef68166
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | REWORK | 1 | | 閉じ判定と平面切断 + 蓋 |
| sub-02 | 未着手 | 0 | | Voronoi 分割・凸包・接着グラフ |
| sub-03 | 未着手 | 0 | | 破片資産 .mfrac と FractureLibrary |
| sub-04 | 未着手 | 0 | | ボクセル化 + surface nets |
| sub-05 | 未着手 | 0 | | 複合の上限撤廃・形状単位インパルス |
| sub-06 | 未着手 | 0 | | Destructible/FracturePiece・事前生成・root proxy |
| sub-07 | 未着手 | 0 | | 接着の破断と塊の剛体化 |
| sub-08 | 未着手 | 0 | | 割れた後の 6 挙動 |
| sub-09 | 未着手 | 0 | | Inspector・非同期焼き・Undo |
| sub-10 | 未着手 | 0 | | スキンメッシュの破壊 |
| sub-11 | 未着手 | 0 | | 計測・ベンチ・上限 |
| sub-12 | 未着手 | 0 | | ABI v22・デモ・spec/ADR |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
| ID | 論点 | 決定 | 状態 |
|---|---|---|---|
| Q-1 | 断面の描き方 | 破片ごとに `_cap` 子エンティティ + 内部マテリアル | 推奨で仮決定 (Notion Q-1、回答待ち) |
| Q-2 | スキンの割れた後 | 全部剛体破片で描く (骨に剛体追従) | 推奨で仮決定 (Notion Q-2、回答待ち) |
| Q-3 | 古い順に消す範囲 | Destructible ごとの maxDebris | 推奨で仮決定 (Notion Q-3、回答待ち) |
| Q-4 | 荷重モデル | 接触は tick 単位判定、蓄積はスクリプトの ApplyDamage のみ | 推奨で仮決定 (Notion Q-4、回答待ち) |
| Q-5 | 固定壁 | ルート kinematic、体積最大の塊が残る | 推奨で仮決定 (Notion Q-5、回答待ち) |
| Q-6 | .mfrac の置き場所 | assets 側 (.meta 付き、git 共有) | 推奨で仮決定 (Notion Q-6、回答待ち) |
| Q-7 | 凹んだ破片 | 凸包 1 個で近似 (非連結は別破片) | 推奨で仮決定 (Notion Q-7、回答待ち) |
| Q-8 | 破壊物の単位 | MeshRenderer のメッシュ 1 つ | 推奨で仮決定 (Notion Q-8、回答待ち) |
| Q-9 | ABI 範囲 | ApplyFractureDamage 1 本 + onBreak (v22 = 126) | 推奨で仮決定 (Notion Q-9、回答待ち) |

## 申し送り (セッション跨ぎ)
- **運用 (2026-09-25 ユーザー指示)**: ハーネス中の質問は推奨案で仮決定して進める。質問は Notion「活動記録」のページ
  https://app.notion.com/p/3e502024a4d481df8d01cbe69c013018 の表 (Q-n) に追記し、回答は後で受け取る。
  planner は AskUserQuestion を使わず、未決事項に `[ユーザーに聞ける]` を付けて返す。司会は Notion へ転記し、
  「ユーザー判断」表に「推奨で仮決定 (Notion Q-n、回答待ち)」と書く。resume 時は Notion の回答欄を読み、推奨と違う回答は planner へ補足として送る。
- 上限 (差し戻し 3 / レビュー 3) 到達時も Notion へ記録。後続が依存しなければ当該サブを「保留」にして先へ、依存すれば中断して報告。
- **作業ツリー**: 開始時点の未コミット WIP (deepmodal 関連、`src/Engine/Renderer/WaterPass.cpp`、`tools/deepmodal/train.py`、`assets/deepmodal/*`、ルート直下の一時ファイル群) には触らない。コミットは SELF_EVAL の「触ったファイル」だけ。WIP と同じファイルを触る必要が出たらそのサブで止めて Notion に記録。
- coder は `model: "sonnet"` で起動する。
- ABI の現状は v21 / 125 スロット (メモの v16/110 は古い)。
