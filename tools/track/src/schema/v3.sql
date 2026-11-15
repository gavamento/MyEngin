-- スキーマ版 3。文書の引き継ぎ (後から .meta が付いた仮の ID の文書 → guid の文書) を表す列と、パスで引く索引 (M104l)。
ALTER TABLE documents ADD COLUMN successor_id TEXT REFERENCES documents(document_id);
CREATE INDEX idx_documents_current_path ON documents(current_path);
CREATE INDEX idx_scene_changes_revision ON scene_changes(revision_id);
