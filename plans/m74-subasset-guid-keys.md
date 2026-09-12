# M74: サブアセット ID のチェックアウト非依存化 — 登録名の接頭辞を `.meta` の GUID に

決定と却下理由は [ADR-019](../docs/adr/ADR-019-guid-subasset-keys.md)、仕様は `engine_spec.md` §10.2.1。
このファイルには**進捗と、計画外の事実・申し送りだけ**を書く (進捗の一次情報は git log)。

## 出所

三校 (Shadow_Sound) の 2 台運用。`C:\HAKtokyo\Shadow_Sound` と `C:\HAL\Shadow_Sound` の 2 台が
同じ stage1 を触り、シーンに「こっちでしか見えない屋根なしステージ」と「向こうでしか見えない
屋根付きステージ」が同居した。敵も同じ現象 (2026-09-12 に調査、ユーザーが根本対策を指示)。
M66 の計画 (`quiet-merging-harbor.md` 決定 2) が独立マイルストーンへ送っていた項目の回収。

## サブ

| サブ | 内容 | 状態 |
|---|---|---|
| M74a | 接頭辞 `guid://<16hex>` (`assetkey::SubAssetKeyPrefix`)。FBX / glTF の 6 か所、`ConvexCookSourcePath` の GUID 逆引き、`kCookVersion` 3、`SubAssetKeySelfTest`、ADR-019 / spec / CLAUDE.md / README | 実装・検証済み (未コミット) |
| M74b | `--migrate-subasset-ids [--project DIR] [--legacy-root OLD]... [--dry-run]` (`SubAssetMigration.*`)。エンジン同梱の research_wing_stage01 プレハブを `C:\HAL\MyEngin` 基準から変換 | 実装・検証済み (未コミット) |
| M74c | シーンロード時に未解決のモデル参照を数えて WARN (「黙って消える」の再発防止) | **未着手 (任意)** — 3 か所のロード経路 (Runtime / Editor / TickRunner) に要る。デモの名前キー材質で誤検出しないかの確認が先 |

## 検証 (2026-09-12)

- Debug / Release ビルド (`/p:MyeWarnAsError=true`) exit 0、警告 0 (最終ソースで全ビルドし直して確認)
- `Editor.exe --selftest` Debug / Release 全 PASS (`SubAssetKey` 27 項目を含む)
- `check_rules.ps1` 0 error / 0 warning
- 三校: 別の絶対パス (空白入り) へ複製したプロジェクトと元の場所で、stage1 の見下ろしと敵の寄りが
  `--img-diff --tol 0` で maxDiff=0 (複製側コールドクック / 元ウォーム再生)
- 三校 `tools\verify.bat replay` PASS (敵 2 体 `body bound, parts 5/5`)
- `replay_verify.bat` PASS (12 ジョブ。joints の `.mcvx` は記録で `guid://…#mesh0#prim0` の凸包を焼き、照合 2 回はクックから読んだ)
- `shot_verify.bat` 24 枚 PASS (モデル入りの demo / parts / joints も maxDiff=0 — 描画順のキーは変わったが絵は不変)。
  ★1 回目は `flow_title` が 944 px 落ちた。原因は `bin\x64\Release\MyeScripting.dll` 未ビルド (C# レーン停止で
  操作説明の行が暗くならない) で、`tools\build_managed.bat Release` の後は一致した = M74 とは無関係
- 三校 `tools\verify.bat all` ALL PASS (golden `main.png` は屋根なしの絵へ撮り直し)

## 申し送り

- **もう 1 台 (C:\HAL) はエンジンを更新してビルドし直すまで、移行後のシーンのモデルが見えない。**
  古いエンジンで移行後のシーンを保存すると、その保存で置いたモデルだけ旧形式 (絶対パス由来) になる。
  そうなったら新しいエンジンで `--migrate-subasset-ids --legacy-root C:\HAL\Shadow_Sound` を回す。
- 配布パッケージは `kCookVersion` が動いたので新しい exe で作り直す (M51j の封印キャッシュは版を見る)。
- 移行で対応付けられないもの: 保存後にプロジェクト内で移動したモデルの ID (三校 `main.scene.json` の
  Lab_Sink 10 か所 = 旧 `assets\new folder\` 時代の ID。M74 以前から壊れていた)。
- `tools\stage01\` (research_wing_stage01 プレハブの生成元、gitignore) は旧方式のまま。再生成したら
  `--migrate-subasset-ids` で変換する (COLLISION.md に追記済み)。
- `canonicalRoot` の警告 (M66b) は残した。文言は「別のパスで作成されています」だけで、ID の話はしていない。
