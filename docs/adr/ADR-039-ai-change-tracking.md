# ADR-039: AI による変更の追跡を、独立した追跡サービスと Editor 専用の表示・復元に分けて作る

- 状態: 設計確定・未実装 (2026-11-13、M104)
- 出所: 要求の正本は [docs\ai-change-tracking\spec-v1.1.md](../ai-change-tracking/spec-v1.1.md)。
  設計は [architecture.md](../ai-change-tracking/architecture.md) / [ipc-contract.md](../ai-change-tracking/ipc-contract.md) /
  [db-schema.md](../ai-change-tracking/db-schema.md) / [attribution-rules.md](../ai-change-tracking/attribution-rules.md) /
  [restore-safety.md](../ai-change-tracking/restore-safety.md)。進め方は [plans\m104-ai-change-tracking.md](../../plans/m104-ai-change-tracking.md)

## 背景

AI クライアントがコード・シーン・アセットを書き換えるようになり、「どこを、誰が、どう変えたか」を開発者が後から確かめる手段が git の差分しか無い。
git の差分には AI 以前の未コミットの変更が混ざり、シーンの変更は JSON の行としてしか見えず、変更の一部だけを戻す手段も無い。

仕様書は既存機能の再利用を原則にしているが、照合すると前提の半分は成り立たなかった
(行 diff も Hunk も無い、`FileWatcher` は削除を落とす、Named Pipe と SQLite の前例が無い、シーンの JSON 化は World を書き換える、
外部のシーン変更は確認なしで適用される)。差の一覧は [architecture.md](../ai-change-tracking/architecture.md) §1。

## 決定

1. **履歴の正本を持つ追跡サービスを、エンジンの外の独立した exe にする。** `tools\track\` の単独の Rust crate (`mye_track.exe`)。
   1 プロジェクトに 1 つだけ動き、DB・Blob・ファイル監視・差分生成・帰属判定を所有する唯一の書き手になる。
   Editor にも MCP にも従属せず、どちらかが居なくても記録を続ける (ユーザー判断、2026-11-13)。
2. **通信は Windows の名前付きパイプ、1 行 1 JSON。** 現在のユーザーに限り、リモートを拒否する。
   メッセージの形は collab と同じ `{id, op, args}` / `{id, ok, result | error}`。プロトコル版は C++ と Rust の定数を規則 9 で突き合わせる。
3. **保存は SQLite (WAL) と内容アドレスの Blob、置き場所は `<project>\.mye\ai_changes\`。** 依存 (`rusqlite` bundled、`sha2`、`notify`) は
   `tools\track` だけに入れる。`tools\mcp` の依存は増やさない (ADR-032 決定 1 を保つ)。
4. **MCP には道具を 5 つ足すが、中身は pipe への取り次ぎだけ。** `change_session_begin` / `change_session_end` / `change_list` /
   `change_describe` / `change_report`。承認と復元の道具は足さない。規則 15-c / 15-d の許可表に pipe クライアントの 1 ファイルを足す。
5. **サービスは作業ツリーを書かない。復元は Editor が行う。** ゲート (`ComputeBlockers`)、一括適用 (`ReloadHub::BeginBatch` / `EndBatch`)、
   Undo (`UndoStack::Record`) がすべて Editor にあるため。復元は git を 1 つも起動せず、Index に触らない。
6. **差分の基準は保存したスナップショット。`git diff` を正本にしない。** 行 diff と Hunk はサービスに新規実装する。
   シーンは JSON を `fileId` で対応付けて構造比較し、数値は字句のまま比べる。
7. **検出と帰属を分け、確かさを 4 段階で持つ。** Verified にするのは、申告された編集の内容を Before に当てた結果が観測と一致したとき、
   または専用の作業ツリーの中の変更だけ。申告だけなら Claimed、時刻だけなら Inferred、根拠が無ければ Unknown。
   取り逃した区間と中間状態は、欠落として記録する。
8. **シーン上の同一性は (projectId, documentId, fileId)。** `documentId` は `.meta` の guid。エディタ外のリネームで guid が変わったら、
   同じ文書とは見なさず、追跡が切れたことを記録する。
9. **追跡の状態を World に入れない。** ECS にコンポーネントを足さず、ハイライトは既存の `EditorLinePass` と `GhostMeshPass` で描く。
   `src\Editor\ChangeTracking\` を Engine / Runtime / GameLogic / Shared / Server から include させない (新しい規則 19)。
10. **開いているシーンが未保存のとき、外部の変更を自動で適用しない。** `ReloadHub` に「今適用してよいか」を問う口を足し、
    Editor が未保存なら保留して確認を出す。口が未設定なら今までどおり (Runtime と既存テストは変わらない)。
    既存挙動の変更 (ユーザー判断、2026-11-13)。
11. **レビュー状態と復元状態を別に持つ。** レビューは Revision と範囲に結び付く追記の記録で、新しい Revision は必ず Pending から始まる。
    承認は git の操作を何も起こさず、却下は変更を消さない。

## 却下した案

- **追跡のコアを Rust のライブラリにして、`mye_mcp.exe` と Editor 用 DLL の両方へリンクする (常駐なし)。**
  collab が ADR-015 で in-process へ反転した前例に沿い、IPC を作らずに済む。却下の理由は、監視役が 2 つになること。
  MCP と Editor が同時に動くと同じ変更を二重に記録するので、どちらが書くかの調停が要り、結局それが「1 つの所有者」を作り直すことになる。
  collab は Editor だけが使うので in-process で足りたが、追跡は Editor が居ないときにも記録が要る。
- **追跡を `mye_mcp.exe` に内蔵する。** MCP のプロセスはクライアントのセッションと同じ寿命で、セッションの外の変更を見られない。
  複数のクライアントが同時に動けば DB の書き手も複数になる。ADR-032 が閉じた「外へ手を伸ばす経路」を大きく広げることにもなる。
- **追跡を Editor に内蔵する。** Editor を起動していない間の記録が取れない (仕様書 §16.2)。ADR-032 が「MCP をエディタに内蔵する」を却下したのと同じ理由。
- **`git diff` と `git stash` / `git apply` で差分と復元を作る。** HEAD との差には AI 以前の未コミットの変更が混ざる。
  Index の状態で結果が変わり、復元が Index を動かす。Hunk の適用を git に任せると、競合の判定を自分で制御できない。
- **既存の `FileWatcher` を広げて使う。** 削除とリネーム前の名前を落とす作りで、`ReloadHub` はそれを前提にしている。
  追跡のために挙動を変えるとホットリロード側に波及する。サービスは Editor の外にあるので、そもそも Engine 層の部品を使えない。
- **ハイライト用の Editor 専用コンポーネントを ECS に足す。** WorldHash からは外せるが、SimSnapshot の World 節は全アーキタイプのカラムを持つので、
  版を上げることになる。表示の切り替えで World が変わる。
- **`fileId` だけでエンティティを識別する。** `fileId` はシーンの中でしか一意でない。別のシーンの同じ番号と混ざる。
- **外部のシーン変更を、常に確認してから適用する。** AI がシーンを書くたびに確認が出る。未保存のときだけ止めれば、失われるものは無い。

## 影響

- ABI、SimSnapshot の版、TypeId、sim の挙動は変えない。`Runtime.exe` と `Server.exe` は追跡に関わらない。
- 新しい crate `tools\track\` と、新しい依存 (`rusqlite` bundled、`sha2`、`notify`)。C コンパイラが要る。
- 新しい `check_rules` の規則 19 (追跡の封じ込め)。規則 9 に版定数の組を 1 つ、規則 15-c / 15-d の許可表に 1 ファイルを足す。
- ADR-032 の「道具は 5 つ」が 10 になる。実装時に ADR-032 と [docs\mcp-server.md](../mcp-server.md) を直す。
- Engine 層の変更は `ReloadHub` の口 1 つ。未設定なら挙動は今と同じ。
- Editor の既存挙動の変更は 1 つ: 未保存のシーンに外部の変更が来たとき、確認が出る。

## 未決のまま残したもの

実装の前に試作で確かめる項目と、ユーザーの判断が要る項目は [plans\m104-ai-change-tracking.md](../../plans/m104-ai-change-tracking.md) にまとめた。
この ADR の決定のうち、試作の結果で変わりうるのは 2 と 3 の実現手段 (Win32 API の呼び方、SQLite の組み込み方) で、責務の分け方は変わらない。
