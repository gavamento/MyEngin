-- スキーマ版 2。`editor_saved` の知らせを観測の時刻で引くための索引 (events の kind と at)。表と列は変えない。
CREATE INDEX idx_events_kind_at ON events(kind, at);
