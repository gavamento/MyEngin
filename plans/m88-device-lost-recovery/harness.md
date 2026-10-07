# harness 台帳: m88-device-lost-recovery

- 依頼原文: A5の復旧まで実装
- 開始: 2026-10-07 / 基点コミット: 218297cfbd462f278fd6beeeb003c07de6ecd7a3
- フェーズ: 完了 (2026-10-07、review round 2 PASS)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | e5d58dd | 検出・疑似消失・安全停止 (①) |
| sub-02 | OK | 1 | b92a391 | 復旧の骨格と旧デバイス参照数ゲート |
| sub-03 | OK | 1 | fb421a1 | アセットの GPU 再アップロード |
| sub-04 | OK | 1 | cb2a04f | エンジン層の残り + Runtime 復旧完成 |
| sub-05 | OK | 2 | 30347ad + 4a05f7b | エディタ側の復旧、偽装メニュー、ADR-026。round 2 で review-1 #1・#2 を修正 |
| sub-06 | OK | 2 | 7131e8c | 復旧中もクライアントの送受信を回す (U5)。round 2: server_verify の check_late_subst に第 6 引数 (止めたレーンの cannot keep up だけ「許容:」表示で許す)。ABCDE 全体 exit 0。混ざったログ行 (tick 3004) は目視で誤判定なし |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 3/3/5/4 | #1 major (Editor の復旧後致命停止で ImGui Win32 二重 Shutdown)、#2 minor (ADR-026 の実体欄)、#3 minor planner (ネットのタイムアウト) |
| 2 | PASS | 5/5/5/5 | なし (#1〜#3 消込済み) |

## ユーザー判断
- 2026-10-07 U1: プロセス内で復旧する (planner 裁定どおり)
- 2026-10-07 U2: Play 中に復旧できず終了するときは Play 開始前の編集状態だけ退避保存、元ファイルは上書きしない (裁定どおり)
- 2026-10-07 復旧時間 (既定デモで Release 約 4 s / Debug 約 13 s): 許容する (planner 裁定どおり)。順次差し替えは後回し
- 2026-10-07 U5 (専用サーバ / クライアントのセッション中の消失で約 4.5 s 止まりタイムアウト 3 s を超える): 裁定 (記録のみ) と逆の「今回対応する」→ sub-06 を新設
- 2026-10-07 review-1 の検証で C:\HAL\三校\crash\ に作られた device_lost_20261007-100443 / -100552 (main.scene.json 各 1 本) はユーザー承認のうえ削除済み
- 2026-10-07 U3: デバイス再作成に失敗しても WARP で続けない。10 回 × 500ms 試して①へ (裁定どおり)

## 申し送り (セッション跨ぎ)
- (別件) K1: `--render-demo --deferred --probe-bake-all` で同じ実行内に BakeAll を 2 回走らせると deferred の絵が変わる (復旧後の再ベイクで maxDiff 17〜21)。ベイカー側の既存の性質と推定、原因未特定。M88 の範囲外 (spec 7.)。
- (nit) ShaderManager の ReleaseGpu/RecreateAll が programs_ を順不同で走査 (rule 7 警告)。次に触るとき ID 順へ。
- (nit) 実シーンの復旧スクショ照合 (sub-04) とエディタ復旧の exit 6 は手動・実走のみで、自動回帰にしていない (spec 3. 後回し)。
- (nit) 再作成失敗 / 再構築失敗の分岐は --simulate-device-lost-stale のゲート不合格経路で代表しており、個別には通していない。
- 未検証 (ユーザー手動確認待ち、docs/test_checklists.md の M88 節): 本物の TDR (dxcap -forcetdr)、メニュー偽装のクリック後の選択・ギズモ・Play/Stop・ドッキング、退避ファイルの再読込 (三校で reviewer が 1 回確認済み)、ImGui の別窓を出した状態での復旧。
- 未検証: 復旧が約 17 s (履歴 1024 tick) を超えたときにネットが resync へ入る経路。
- 検証用の一時プロジェクトが scratchpad\tmpproj にある (リポジトリ外、セッション限り)。
- マイルストーン番号: M86 (Smart Objects) と M87 (エンジン MCP サーバ) は予約済みのため M88 を使う。
- ユーザーの Claude Code 週次利用枠が残り約 16% (2026-10-06 時点) → 2026-10-07 にリセットされ予算制約は解除。
- (planner) 安全な停止点: sub-01 で①が単独完了。sub-02〜04 は旧デバイス参照数ゲートが未対応の所有者を検出して①へ落とすため、どこで止めても壊れた描画で続行しない。sub-04 で Runtime 復旧、sub-05 でエディタ復旧。
- (planner) sub-02 の最初の作業は「Shutdown 後の旧デバイス参照数」の実測 (spec R1)。値が安定しなければ planner へ戻す。
