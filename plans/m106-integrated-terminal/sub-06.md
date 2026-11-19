# sub-06: AI 変更追跡との紐付け

- 依存: sub-02 (track 側 Rust 変更は sub-03〜05 と並列可。AI Changes 窓の表示は sub-04 OK 後に入れる)
- 状態: 未着手
- 往復: 0

## やること
spec 2・4.2。
- track スキーマ v4: `/home/user/MyEngin/tools/track/src/schema/v4.sql` で `sessions.host_terminal TEXT` を追加、`/home/user/MyEngin/tools/track/src/schema/mod.rs:15` の Migration 配列に登録。
- IPC `session_begin` に任意 `hostTerminal` を受け付けて保存 (欠落 = NULL)。
- フック `/home/user/MyEngin/tools/track/src/hook.rs:422-430` の begin_session で環境変数 `MYE_TERMINAL_ID` があれば `hostTerminal` に載せる。`source` は "terminal" のまま。
- エディタ側: AI Changes 窓 (`/home/user/MyEngin/src/Editor/Windows/Project/AiChangesWindow.h` 一式) でセッション行に「統合ターミナル」表示 (host_terminal が非NULL)。DB 読み取り経路を確認して列を追加。ローカライズ両言語。
- 文書: `/home/user/MyEngin/docs/ai-change-tracking/` の ipc-contract.md (:111 付近)、db-schema.md (:47 付近)、hooks-setup.md (:128 付近)、spec-v1.1.md (:131, :229) と engine_spec §9.2 を更新。

## やらないこと (このサブでは)
- source 値の変更、過去セッションの遡及付与。

## 受け入れ条件 (このサブ)
1. spec 5-9。
2. Linux: `cargo test` (tools/track) 成功。新テスト: v3 DB → v4 移行で既存行の host_terminal が NULL、hostTerminal 付き/無し session_begin の保存、フックが環境変数を載せる。
3. 文書の列・フィールド名がコードと一致 (grep)。
4. Windows (要確認): 統合ターミナルで claude を起動しファイル編集 → AI Changes 窓で区別表示 (スクショ)。

## 検証コマンド
- Linux: `cd /home/user/MyEngin/tools/track && cargo test` (依存クレート取得可否を最初に確認、不可なら理由を報告)。
- Windows: エディタビルド、手動。
