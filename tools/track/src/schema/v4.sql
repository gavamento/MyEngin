-- スキーマ版 4。セッションを開いた MyEngine 統合ターミナルの ID (MYE_TERMINAL_ID)。統合ターミナルの外から始まったセッション・版 3 までの行は NULL (M106f)。
ALTER TABLE sessions ADD COLUMN host_terminal TEXT;
