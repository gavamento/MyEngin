# harness 台帳: m107-gamelift-editor

- 依頼原文: AWSとつなげてオンラインできる機能をもっと簡単に使えるように死体
- 開始: 2026-11-27 / 基点コミット: 276f8613af9e7981cb125ee2d1173b4e51b00fa1
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 45131630 | Server.exe: gamelift-managed / 停止一本化 / ログ。nit 2 件 (StopRequest.h の Windows.h、--log-dir 時のエラー出先は sub-06 文書へ) |
| sub-02 | OK | 1 | 0c391a7c | サーバ版の組み立て + 局所起動。nit 2 件 (install.bat の start /wait = R9、BuildServerPackage が長い)。**手動チェックポイント待ち (ユーザー)** |
| sub-03 | OK | 1 | (本コミット) | aws CLI 層 + check_rules 規則 22。should 1 件 (コンピュート名・ロケーションの許可文字が AWS より広い) は spec D14 を直して sub-04 の 9 項へ持ち越し |
| sub-04 | 実装中 | 0 | | GameLift 窓 共通部 + Anywhere |
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
- 質問の運用: ~~その場で聞く~~ → **推奨で進めて Notion に後回し** (2026-11-27、ユーザーが就寝するため変更。sub-01 の判定待ちの時点)。質問表: Notion 活動記録「M107 GameLift 窓: ハーネスの保留質問」https://app.notion.com/p/42402024a4d481cbbf8dd556d2ff9c45
- 2026-11-27 planner の未決 (spec 7.) を司会が AskUserQuestion で確認。**6 件とも planner の裁定どおり**:
  - Q1 EC2 の実機確認: sub-02 の直後に手動チェックポイント (費用あり、ユーザーが CLI を貼る)
  - Q2 参加ボタンのクライアント: Runtime.exe (Anywhere = Editor.exe の隣 + `--project`、EC2 = 書き出しフォルダ、`--project` なし)
  - Q3 VC++ ランタイム: サーバ版に vc_redist.x64.exe + install.bat を同梱
  - Q4 課金中の確認: 起動時に 1 回 (保存済みフリート ID があるときだけ) + 窓で常時。1 台以上なら WARN + 窓を開いて赤帯
  - Q5 窓: 新しい GameLift 窓 (Window メニューの Network の隣)。NetWindow の接続欄は変えない
  - 確定確認: 受け入れ条件 1〜29 をこのまま確定

## 申し送り (セッション跨ぎ)
- 事前調査の結果は `plans/m107-gamelift-editor/design-draft.md`
- (司会 2026-11-27) **サブが OK になるたび、技術解説を Notion 活動記録に 1 ページ書く (ユーザー指示。人が読む前提で分かりやすく、詳しく。「なぜ」だけでなく**実際にどんなコードを書いたか** (起動〜停止の流れ、ファイルごとの変更、実際の差分からのコード抜粋と読み方、テストの仕組み) も書く。抜粋はコミットの差分から取り、創作しない)**。sub-01 = https://app.notion.com/p/42402024a4d48193bb6df9776c2fc320 / sub-02 = https://app.notion.com/p/42402024a4d48134bc47f95116d24f2d
- (司会 2026-11-27) **M107 完了時に hooks を戻す (ユーザー指示)**。止まらないよう `C:\Users\akita\.claude\settings.json` の `hooks` 節を `C:\Users\akita\.claude\hooks-disabled-20261127.json` へ退避中 (全体のバックアップは `settings.json.bak-20261127-hooks`)。戻すときはバックアップで上書きせず、退避ファイルの `hooks` を現在の settings.json へ書き戻す (途中で変わった他の設定を消さないため)。戻したらユーザーに報告する
- (planner 2026-11-27) spec.md は「仮確定」。AskUserQuestion が使えなかったため Q1〜Q5 と確定確認は司会がユーザーに聞く (spec 7.)。
- (planner) **sub-02 の OK 後に手動チェックポイント** (sub-02.md 末尾の CLI 手順、EC2 c5.large を 1 時間未満)。結果を planner へ渡してから sub-05 に着手する。ERROR なら sub-02 差し戻しの可能性。
- (planner) sub-01 と sub-03 は依存なしで並列可。sub-02 と sub-03 はどちらも `src/Editor/Tools/GameLift/` と `SelfTestSuites.cpp` を触るので、同時に走らせるなら合流時の競合に注意。
- (planner) 見つけた既存バグ: `--hosting gamelift` は Ctrl+C のハンドラを持たず ProcessEnding が送られない (`LocalHosting.cpp:36` だけが登録)。`docs/gamelift-anywhere.md` 8 章の記述と食い違う。sub-01 で直し、sub-06 で文書を直す。
- (司会 2026-11-27) 別件 (M107 と無関係、直さない): `modalsynth` の 1 項目 FAIL (`transient burst increases initial impact energy in [0, 15ms]`)。src/Engine は基点から差分なし、最終変更は 7ee637a3 (2026-11-19)。master の CI は 2026-11-24 以降 failure (基点では Rust の `pipe_is_owner_only_and_refuses_remote_clients`)。完了報告で別件として挙げる
- (司会 2026-11-27) Notion 後回し M107-N1 (Defender 検疫 R8) を質問表に記載。sub-02 OK の時点で、Release Editor.exe が一度 `Behavior:Win32/DefenseEvasion.A!ml` で検疫された (02:01:45)
- (planner 2026-11-27) 手動チェックポイント手順 4 で upload-build の**実際の出力 (進捗の行)** をユーザーに残してもらう (sub-03 の進捗解析の fixture が推測のため)。司会がユーザーに頼むときに一言添える
