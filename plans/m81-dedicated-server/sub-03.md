# sub-03: 出自情報 (engine/game/content) と NetIdentity の統合

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
spec D5 / D6 / D7 / D8 / 4.2 の SimProvenance を実値で埋め、照合を 1 関数にまとめ、P2P の NetIdentity もそこから作る。

1. `engineVersion`: 既存の `MYE_GIT_HASH` (`build\Common.props:81-96` が生成する `obj\generated\<Config>\MyeBuildInfo.h`) の文字列の 64bit ハッシュ。**新しい生成の仕組みを作らない**。`"unknown"` は 0。`-dirty` を含むときは起動ログに WARN。
2. `gameVersion`: ScriptHost がロードした GameLogic.dll (ホットリロードのシャドウコピーではなく、ロード対象の実ファイル) のバイト列の 64bit ハッシュ。C# アセンブリは含めない (D7)。DLL 無しは 0。
3. `contentHash`:
   - 対象 = assets\ 配下の全ファイル − 除外拡張子 (spec D5 の一覧、大文字小文字無視)。パスは assets ルート相対・`/` 区切り・小文字化で正規化し昇順。ファイルごとに (正規化パス, サイズ, 内容の 64bit ハッシュ) を畳む。
   - `content_manifest.json` の書出し CLI `--write-content-manifest PATH` (Editor / Runtime / Server 共通の EngineCli) と、起動時の読込 (assets ルート直下にあれば読む)。無ければ同じ関数で計算し、所要時間をログ。
   - Build Settings のパッケージ処理 (Editor) が配布物へ manifest を焼くように 1 段追加する (既存の段の並びに合わせる)。
4. `schemaVersion` = kSimSnapshotVersion と `sizeof(InputSnapshot)` と kReplayFileVersion を畳んだ値、`protocolVersion`、`apiVersion`。`initialSnapshotHash` = 開始スナップショット blob のバイト列ハッシュ (.rep に埋め込むとき)。
5. `CompareProvenance(a, b) → ProvenanceMismatch` (最初に食い違った項目を返す、名前関数つき)。0 を「不明」とする項目の規則 (engineVersion / contentHash: 双方 0 なら WARN 付き一致、片方だけ 0 は不一致)。
6. `--allow-game-mismatch`: gameVersion の不一致だけを WARN に落とす。指定は SessionConfig.configBits の新ビットに載り、.rep に残る。
7. P2P: `NetIdentity` を SimProvenance + 既存の P2P 項目 (canvas、referenceW/H、fontMetricsHash、startWorldHash 等) から作る形へ変え、`kNetProtoVersion` 6、`NetReject` に末尾追加 (`EngineVersion`, `GameVersion`, `ContentHash`)。`CompareNetIdentity` は内部で `CompareProvenance` を呼ぶ (照合項目を二重管理しない)。
8. .rep v9 のヘッダの SimProvenance を実値で書く。起動ログに 1 行で出す (`[provenance] engine=... game=... content=...`)。

10. (sub-02 VERDICT should) check_rules 規則 13-a の許可リストのうち `EngineApiTable.cpp` を「`Engine/Engine/Net/NetRuntime.h` の include だけ許可」に絞る。許可リストの他の行 (EngineLoop.cpp / HeadlessSim.cpp) はファイル単位のままでよい (ネット組立の持ち主)。
9. (sub-02 VERDICT から) `DiffReplayFiles` は SessionConfig.configBits と SimProvenance の一部を比較する (sub-02 の実装)。configBits と provenance を実値で埋めると、net_verify の「ローカル参照 (offline) ↔ host」の `--rep-diff` が起動オプションの差で割れうる。**比較する項目を「tick 列の意味に効くもの」に限る**方針 (sub-02 で role / inputDelay / 版番号を外したのと同じ) を保ち、net_verify (C7) が PASS することで確認する。`--allow-game-mismatch` のビットは比較から外す。

## やらないこと (このサブでは)
- サーバ/クライアントのプロトコル (sub-04)
- manifest の署名・改ざん検知 (対象外)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Session\` (Provenance の算出と照合)
- `src\Engine\Engine\Net\NetSession.h/.cpp` (NetIdentity / proto v6 / NetReject)
- `src\Engine\Engine\Script\ScriptHost.*` (ロードした DLL のパス取得)
- `src\Engine\Engine\App\EngineCli.cpp` (`--write-content-manifest`、`--allow-game-mismatch`)
- `src\Editor\` の Build Settings パッケージ処理 (manifest を焼く段)
- `src\Engine\Engine\Loop\EngineLoop.cpp` (NetIdentity の組み立て箇所、.rep ヘッダ)

## 受け入れ条件 (このサブ)
spec 5. の **P1, P2, P3, P4** と **C1, C2, C3, C5, C6, C7**。

## 検証コマンド
- `tools\replay_verify.bat`、`tools\net_verify.bat`
- `pwsh -File tools\check_rules.ps1`
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- P1: Debug / Release の起動ログの `[provenance]` 行を並べて貼る。1 行変更して dirty にしたビルドで値が変わることも貼る (変更は戻す)
- P3: `bin\x64\Release\Runtime.exe --write-content-manifest cache\m81c_manifest.json` の出力と所要時間

## 実装メモ (coder が追記)

## フィードバック履歴
