# harness 台帳: m105-embedded-browser

- 依頼原文: エンジン上で任意のブラウザをセットし起動・普通に使えるようにしたい
- 開始: 2026-11-18 / 基点コミット: da0c13e53041529404238a405ef987ef477ef126
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 2 | 9b373892 | 検出と埋め込みの中核 (実現性試作、撤退条件 E1〜E3)。round 1 REWORK: 親の喪失 (Opera GX) の検出、SendProbeInput 削除、SetLastError(0)。O4/O8 (キー・IME 入力) は人の手の確認へ = sub-02 の着手条件 → **2026-11-19 ユーザー確認 OK (Chrome / Edge とも英字・日本語 IME を入力できた)** |
| sub-02 | OK | 2 | cb0aa48f | round 1 REWORK: ダミー窓プロセスの残存 (寿命上限 120 s + スイート終端の後始末)、トースト通知を airspace から除外 (Q-7)。手動の受け入れ 2〜5・R4 はユーザー依頼 (コミットは待たない)。 ドック可能なブラウザ窓 (追従・airspace・フォーカス・設定・ローカライズ)。依存 sub-01 |
| sub-03 | OK | 1 | | 後始末・障害時表示・既存窓の取り込み・ADR-040 等。依存 sub-02 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 質問の運用: 推奨で進めて Notion に後回し (2026-11-18 起動時に回答)
  - 質問表: Notion 活動記録「M105 埋め込みブラウザ: ハーネスの保留質問」https://app.notion.com/p/41c02024a4d4810b9598cfe7b934f4d3 (Q-1〜Q-5 = spec 2. の D3/D7/D14/D16/D17、回答待ち)
- 使う場所: エディタのドック窓のみ (Runtime / ゲーム内は対象外)
- 方式: A. 窓の埋め込み (検出したインストール済みブラウザの exe を起動し、SetParent でドック窓の子窓にする)
  - 不採用: WebView2 (ユーザーが「WebView 以外で」と希望)、WebView2 フォールバック、CEF、CDP 画面配信
  - 理由: 既存ブラウザそのもの (拡張・IME・入力が普段どおり) を使いたい
- 検出元: レジストリ StartMenuInternet (HKLM / HKCU / WOW6432Node)。このPCでは Brave / Google Chrome / Microsoft Edge / Opera GX / Vivaldi (+ IEXPLORE.EXE)

## 申し送り (セッション跨ぎ)
- 事前調査: エンジン内に Web 表示は無い (ShellExecuteW で既定ブラウザに URL を渡すだけ: src/Editor/App/EditorApp.cpp:991, src/Editor/Update/UpdateWindow.cpp:64)。ImGui は DockingEnable のみ、ViewportsEnable 無効 (src/Engine/Renderer/ImGui/ImGuiRenderer.cpp:28)
- planner (2026-11-18): spec.md / sub-01〜03 を作成 (ユーザーへの直接質問なし、裁定と `[ユーザーに聞ける]` は spec 2. の D3/D7/D14/D16/D17)。sub-01 は撤退条件 E1〜E3 付きの実現性試作 — 当たったら sub-02 へ進まず planner へ戻す。番号: ADR-040、check_rules 規則 20 を使う予定 (ABI / SimSnapshot / TypeId は変更なし)
