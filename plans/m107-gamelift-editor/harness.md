# harness 台帳: m107-gamelift-editor

- 依頼原文: AWSとつなげてオンラインできる機能をもっと簡単に使えるように死体
- 開始: 2026-11-27 / 基点コミット: 276f8613af9e7981cb125ee2d1173b4e51b00fa1
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | 未着手 | 0 | | Server.exe: gamelift-managed / 停止一本化 / ログ |
| sub-02 | 未着手 | 0 | | サーバ版の組み立て + 局所起動。OK 後に手動チェックポイント |
| sub-03 | 未着手 | 0 | | aws CLI 層 + check_rules 規則 22 |
| sub-04 | 未着手 | 0 | | GameLift 窓 共通部 + Anywhere |
| sub-05 | 未着手 | 0 | | EC2 + 課金中表示 (チェックポイント通過が前提) |
| sub-06 | 未着手 | 0 | | 文書・ADR-042・版 0.90.4 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 2026-11-27 (司会の事前調査で確認):
  - 簡単にする対象: **エディタの窓から操作** (開発者向け。配布ゲームのプレイヤー向けロビー連携は今回の対象外)
  - サーバの場所: **両方を選べるようにする** (自分の PC = GameLift Anywhere / AWS 上 = マネージド EC2 フリート)
  - AWS の呼び方: **aws CLI を裏で呼ぶ** (AWS SDK for C++ は組み込まない)
  - 進め方: harness で回す
- 質問の運用: **その場で聞く** (2026-11-27)
- 2026-11-27 planner の未決 (spec 7.) を司会が AskUserQuestion で確認。**6 件とも planner の裁定どおり**:
  - Q1 EC2 の実機確認: sub-02 の直後に手動チェックポイント (費用あり、ユーザーが CLI を貼る)
  - Q2 参加ボタンのクライアント: Runtime.exe (Anywhere = Editor.exe の隣 + `--project`、EC2 = 書き出しフォルダ、`--project` なし)
  - Q3 VC++ ランタイム: サーバ版に vc_redist.x64.exe + install.bat を同梱
  - Q4 課金中の確認: 起動時に 1 回 (保存済みフリート ID があるときだけ) + 窓で常時。1 台以上なら WARN + 窓を開いて赤帯
  - Q5 窓: 新しい GameLift 窓 (Window メニューの Network の隣)。NetWindow の接続欄は変えない
  - 確定確認: 受け入れ条件 1〜29 をこのまま確定

## 申し送り (セッション跨ぎ)
- 事前調査の結果は `plans/m107-gamelift-editor/design-draft.md`
- (planner 2026-11-27) spec.md は「仮確定」。AskUserQuestion が使えなかったため Q1〜Q5 と確定確認は司会がユーザーに聞く (spec 7.)。
- (planner) **sub-02 の OK 後に手動チェックポイント** (sub-02.md 末尾の CLI 手順、EC2 c5.large を 1 時間未満)。結果を planner へ渡してから sub-05 に着手する。ERROR なら sub-02 差し戻しの可能性。
- (planner) sub-01 と sub-03 は依存なしで並列可。sub-02 と sub-03 はどちらも `src/Editor/Tools/GameLift/` と `SelfTestSuites.cpp` を触るので、同時に走らせるなら合流時の競合に注意。
- (planner) 見つけた既存バグ: `--hosting gamelift` は Ctrl+C のハンドラを持たず ProcessEnding が送られない (`LocalHosting.cpp:36` だけが登録)。`docs/gamelift-anywhere.md` 8 章の記述と食い違う。sub-01 で直し、sub-06 で文書を直す。
