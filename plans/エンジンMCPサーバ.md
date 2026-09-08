# エンジン MCP サーバ (M71) — 計画

**現在地: 設計確定・未着手。コードは 1 行も書いていない。**
2026-09-08 の会話で「このエンジンに AI 用の MCP を用意する」案を詰め切った結果。
姉妹メモ `plans\エンジン機能の自然言語検索.md` の §7「未決の判断」のうち、
**S0 (索引) と S3 代替 (MCP) をこの計画が決着させる**。S1 (エディタ内検索パレット) と
S2 (埋め込みモデル) は据え置きで、この計画には含めない。

---

## 1. 再開手順

1. この節と §3 の決定表を読む。設計の分岐は**すべて決着済み**なので、再検討しない。
2. §9「却下した案」を読む。同じ案を再提案しないため。
3. §8 の「着手前に実測すること」を先に潰す。3 つとも未確認で、結果次第で §5 の返却形が変わる。
4. §7 のサブ分割の頭 (M71a) から着手する。1 サブ = 1 コミット = 1 セッション。

---

## 2. 何を作るか / 作らないか

**作るもの** — エンジンの機能・ABI・CLI・golden を索引し、それを AI クライアントへ
MCP (JSON-RPC 2.0 / stdio) で出す**開発ツール**。加えて、プロジェクト固有の記録を
LLM が追記できる置き場。

**作らないもの** — エンジンにモデルを載せることは**しない**。ファインチューニングも
しない。姉妹メモ §3 の結論どおり「エンジン特化」の正体は索引の質であって重みではない。

**この投資が効く場所を正直に書いておく。** Claude Code は既にシェルを持っているので、
bat を包むだけの MCP は価値がほぼゼロ (今すでに呼べる)。実際に効くのは 3 箇所だけ:

| | 中身 | シェルで代替できるか |
|---|---|---|
| ① ログの構造化 | `replay_verify.bat` は 8 ビルド + 並列 10 ジョブでログが巨大。落ちた job / 割れた tick / フィールド差分だけを返す | △ **ここが最大の節約** |
| ② 索引検索 | 「霧デモの golden は何枚目」を `engine_spec.md` 258KB の grep 抜きで 1 呼び出し | **✗ 代替不能** |
| ③ 記録の継承 | セッションをまたいでプロジェクトの現在地を LLM が持てる | **✗ 代替不能** |

**エンジンの成果物ではなく外側の開発ツールである**ことは意識しておく。三校企画 /
HAL Collector の審査に効くのは決定論・リプレイ・音響の方で、これは自分の効率への投資。

---

## 3. 確定した設計

| 項目 | 決定 | 根拠 |
|---|---|---|
| 置き場 | `tools\mcp\` **独立 crate** | `tools\collab` は workspace ではなく単独 crate。workspace 化すると `build_collab.bat` と `collab_verify.bat` に波及する。独立なら M66 の資産に一切触らない |
| 言語 | Rust / edition 2021 / `panic = "unwind"` | `tools\collab\Cargo.toml` の判断をそのまま踏襲 (2024 だと `#[unsafe(no_mangle)]` が要る / abort だと `catch_unwind` が無意味になる) |
| 依存 | `serde` `serde_json` のみ | collab の「依存が増えるほど cargo build が通らない同僚が増える」を維持 |
| JSON-RPC | **自前実装** | MCP は JSON-RPC 2.0。stdio なら 200 行規模。`rmcp` は §9 で却下 |
| transport | **stdio 先行 + handler 分離** | `handler.rs` を `fn handle(Request) -> Response` の純関数にし、`stdio.rs` は行単位で回すだけ。HTTP が要るときに handler へ触らずに済む。純関数なので transport 抜きで回帰も取れる |
| 道具 | `search` / `describe` / `verify_job` (start+poll) / `note_read` / `note_append` | §5 |
| 索引の源 | component・field は**実行時ダンプ**、ABI・CLI・ADR・golden は静的パース | §4 |
| 検索 | 識別子の単純スコア + 日英同義語表 | 識別子は語が短いので BM25 より単純マッチが効く。文書本文の全文検索はやらない |
| 鮮度 | `tools\gen_engine_index.ps1` を**人が叩く**。MCP は古ければ `stale` フラグを付けるだけ | `gen_project_files.ps1` と同じ枠。読み取り専用の道具が副作用で exe を起動しない |
| notes | `<projectRoot>\.mye\notes\<topic>.md`、**追記のみ + タイムスタンプ**、**gitignore** | §6 |
| 子プロセス | `CREATE_NO_WINDOW` で起動 | §3.1 |
| CI | **一切入れない。`--selftest` にも入れない** | 無ければ「利用不可」に縮退するだけ (`MyeCollab.dll` と同じ) |
| マイルストーン | M71 (M70 まで消化済み) | |

### 3.1 Rust に置くことで構造的に消える罠が 2 つある (この計画の隠れた利得)

**① `chcp` のコンソール残留。** `replay_verify.bat --job` の子は `chcp 437` (単バイト CP) を
強制する必要がある — 多バイト CP だと cmd のバッチ読取りが `goto` の後にバイト数と文字数の
ずれで読み位置をドリフトさせ、日本語 rem の断片をコマンドとして実行して即死する。
ところが `chcp` は**プロセスではなくコンソールの状態**を変えるので、`run_parallel.ps1` の
ように `-NoNewWindow` で親とコンソールを共有すると呼び出し元シェルごと文字化けする
(2026-08-27 に実際に発生。`run_parallel.ps1` の `finally { chcp 65001 }` はその対策)。

Rust から `CREATE_NO_WINDOW` で起こせば**子が親のコンソールを持たない**ので、
漏れる経路そのものが無くなる。try/finally の規律ではなく構造で殺せる。

**② GUI サブシステムの exit code。** CLAUDE.md の「`& Editor.exe` は待たずに戻り exit code が
取れない」は **PowerShell 固有**の挙動で、`std::process::Command` はプロセスハンドルを待つので
subsystem に関係なく exit code が取れる。`cmd /c` の重ね着が要らない。

ただし **`verify_job` の子の呼び形だけは `run_parallel.ps1:67` とバイト単位で揃える**
(`cmd /c chcp 437 >nul & call <bat> --job <name> > "<log>" 2>&1`)。CI と同じ経路を通ることを
保証するため — 「CI 専用の検証ロジックを書かない」の精神。

---

## 4. 索引の設計

### 4.1 実行時ダンプ (component / field) — 姉妹メモの S0 案からの改善

姉妹メモ §6 は「索引ジェネレータはソース構造に依存するので、`MYE_JP` の書式を変えた瞬間に
静かに壊れる」と自分で懸念を書いていた。**これは回避できる。**

`Reflection.h:67` の `WithJp` は `FieldDesc.displayName` に**実行時メンバとして**書き込んで
いて、`tooltip` も同様。つまり コンポーネント名 / TypeId / フィールド名 / 型 / offset / flags /
日本語表示名 / tooltip は**すべて実行中のエンジンが権威データとして持っている**。
`--selftest` が D3D もウィンドウも作らずに ECS を回している以上、`Editor.exe --dump-index` は
完全にヘッドレスで成立する。

→ 索引の一番おいしい部分 (436 フィールド) からパースの脆さが消える。

### 4.2 静的パースが残る部分

| セクション | 源 | 備考 |
|---|---|---|
| `abi` | `src\Shared\EngineAPI.h` (110 スロット / v16) | `check_rules.ps1` の `$apiVersionSlots` に静的表の前例がある |
| `cli` | `EditorMain.cpp` の argv パーサ + CLAUDE.md の説明文 | **CLAUDE.md に載っているフラグ ⇄ 実際のパーサ**の突き合わせが副産物で取れる |
| `adr` | `docs\adr\*.md` の見出し | 本文は索引しない (見出しへのポインタのみ) |
| `spec` | `engine_spec.md` の見出し | 同上 |
| `golden` | `tools\shot_verify.bat` の撮影表 + `tests\golden\*.png` | 枚数・frame・tol・CI 対象かを持つ |
| `l10n` | `LocalizationTable.inl` (1269 行の `MYE_STR(id, en, ja)`) | **日英同義語表の種**として使う |

### 4.3 壊れたことに気付く仕掛け

`check_rules.ps1` に**件数の下限チェック**を足す (component < 45 / field < 400 / abi != 110 で
fail)。静的パースが静かに壊れるのを殺す唯一の手段 — 姉妹メモ §6 の懸念への回答。

出力は `cache\engine_index.json` (gitignore)。

---

## 5. 道具の仕様

全て `handler.rs` の純関数経由。**引数はシェルに一切近づけない** — `verify_job` の job 名は
enum で受け、文字列を bat へ素通しさせない。

### search

```
{"name":"search","arguments":{"q":"音の遮蔽","kind":["component","cli","golden"],"limit":20}}
→ {"stale":false,"hits":[
     {"kind":"cli","id":"--acoustic-demo","score":100,"jp":"音響伝播のショーケース","src":"engine"},
     {"kind":"component","id":"AcousticAudio","typeId":50,"score":90,"src":"engine"},
     {"kind":"note","id":"sanko-progress","score":40,"src":"notes"}
   ]}
```

スコアは 完全一致 > 前方一致 > 部分一致 > 日本語表示名・tooltip の当たり。
`src` (`engine` / `notes`) を**必ず**付ける。notes は §6 で書ける場所なので、
エンジンの権威情報と混ぜない。

### describe

名前 1 つを引いて関連を全部出す。**`search` で当たりを付けて `describe` で深掘る**のが想定動線。

```
{"name":"describe","arguments":{"id":"AcousticAudio"}}
→ {"kind":"component","typeId":50,"hashed":false,
   "fields":[{"name":"...","type":"Float","jp":"...","flags":["kFieldNoSerialize"],"offset":0}],
   "adr":["ADR-017"],"spec":["§10.6"],
   "cli":["--acoustic-demo","--acoustic-audio-log"],"golden":[16,17,18,19]}
```

### verify_job (start / poll)

対象は `replay_verify.bat` の再入口が持つ **10 個**:
`demo` / `parts` / `flow` / `mp` / `physics` / `joints` / `acoustic` / `ttdebug` / `ttrelease` / `rules`。
**ビルド済み前提** (ビルドはシェルでやる)。`shot_verify.bat` には `--job` 相当が無いので v1 対象外。

```
{"op":"start","job":"acoustic","ticks":600} → {"jobId":"jb_01"}
{"op":"poll","jobId":"jb_01"}
→ {"state":"done","exitCode":1,"summary":{
     "result":"FAIL","stage":"Debug verify with snapshot stress",
     "firstMismatchTick":143,
     "fields":[{"path":"...","expected":"...","actual":"..."}],
     "diagnostic":"plain verify passes = snapshot restore asymmetry"}}
```

stdout はパースせず、**ジョブ終了後に `<LogDir>\<name>.log` を読んで構造化する**
(`run_parallel.ps1` が既に各ジョブのログを隔離している)。
`[FAIL]` / `[diag] first mismatch tick:` / `[diag] field-level diff` が拾う行。

start / poll に割るのは、他社 LLM API 経由だと tool call のタイムアウトを自分で制御できない
ため。`tools\collab\src\worker.rs` のタイマー付き worker が型になる。

### note_read / note_append

```
{"name":"note_append","arguments":{"topic":"sanko-progress","text":"..."}}
→ {"path":"...","bytes":1234}
{"name":"note_read","arguments":{"topic":"sanko-progress","latest":5}}   # topic 省略 = 一覧
```

1 回の `note_append` = `## 2026-09-08T12:34 <見出し>` + 本文。**上書きも削除もできない。**

---

## 6. 安全側の規約

書き込み口は「Claude Code なら承認プロンプトが出るが、**自前ループの他 LLM は無条件に叩く**」
という前提で組む。だから**不可逆な操作を持たせない**。

```
書き先:  <projectRoot>\.mye\notes\<topic>.md   ← --project 指定時
         .mye\notes\<topic>.md                 ← projectRoot 無し (エンジンリポ作業時)
ガード:  topic は [A-Za-z0-9_-]{1,64} のみ (パス区切り・.. を構文で排除)
         拡張子 .md 固定、書き先ディレクトリ外へ解決したら拒否
         src\ / assets\ / docs\ / plans\ には一切書けない
```

**分岐は `config.projectRoot` の有無で判定する** (CLAUDE.md の規約。エンジンの
「プロジェクト起動 / 裸起動」と同じ判定基準を使う)。

**規則 13** を `check_rules.ps1` に足す (規則 12 = Source Control の Editor 層封じ込めと同型。
**後から足すと必ず漏れる**ので先に書く):

1. AI 経路は Editor 層に封じ込め、**sim を 1 バイトも書かない**
2. AI 経路の書き込みは **notes 配下のみ**

`tools\mcp_verify.bat` に「`../` や絶対パスを topic に渡したら拒否される」**固定テスト**を
入れる (`SourceControlSelfTest` の (d3) と同型 — 機械で固定しないと画面に出ない類の違反)。

### 記録場所の住み分け

**既に 3 つある** (`plans\` / `docs\adr\` / Claude Code の memory)。4 つ目を足す以上、
先に規約を決めておかないと「どれにも書かれない」か「全部に重複」のどちらかになる。

| 場所 | 書くもの | 読む主体 |
|---|---|---|
| `docs\adr\` | 覆らない設計判断 | 人 + AI |
| `plans\` | これからやること | 人 + AI |
| Claude Code memory | 作業のクセ・踏んだ罠 | Claude Code のみ |
| **新** `.mye\notes\` | **プロジェクト固有の実測値・現在地** (例: 三校企画の工程 A のどこまで実装済みか) | 全 LLM |

4 つ目に固有の役割は「**エンジンリポにコミットできない / したくないプロジェクト固有の状態**」。
`docs\sanko-implementation-status.md` が今その役目を手作業で担っている。

notes は **gitignore** する。残す価値があるものは**人が `docs\` へ昇格させる** 2 段構え —
AI が書いたものがレビューなしにリポへ溜まるのを防ぐ。

---

## 7. サブ分割 (1 サブ = 1 コミット = 1 セッション)

| | 中身 | 触るもの |
|---|---|---|
| **M71a** | `Editor.exe --dump-index` + `tools\gen_engine_index.ps1` + `check_rules.ps1` の件数下限 | `EditorMain.cpp` (~50 行) / 新規 ps1 / check_rules.ps1 |
| **M71b** | Rust crate 骨格 + stdio JSON-RPC + `search` / `describe` + `tools\mcp_verify.bat` + `tests\mcp\*.ndjson` | 新規 `tools\mcp\` |
| **M71c** | `verify_job` (start / poll、`CREATE_NO_WINDOW`、`<name>.log` の構造化) | `tools\mcp\src\verify.rs` |
| **M71d** | `note_read` / `note_append` + 規則 13 + パストラバーサル拒否の固定テスト | `tools\mcp\src\notes.rs` / check_rules.ps1 |

**M71a は MCP なしで完結し、Claude Code が `cache\engine_index.json` を直接読めるので
単体で効果が出る**。ここで止めても損をしない順序にしてある。

---

## 8. 着手前に実測すること (3 つとも未確認)

1. **`--dump-index` の出力サイズ。** 436 フィールド + 110 スロット + 約 80 の CLI フラグで
   数百 KB のはず。数 MB になるなら `search` の返却を絞る設計が要る。
2. **`CREATE_NO_WINDOW` で `chcp 437` が親コンソールに漏れないこと。** 構造上漏れないはずだが、
   2026-08-27 の事故と同じものなので実測する (Claude Code の TUI が道連れになる)。
3. **MCP の `initialize` が要求する `protocolVersion` の現行値。** 自前実装なので仕様追従は
   自分の責任。

---

## 9. 却下した案とその理由 (再提案しないため)

| 案 | 却下理由 |
|---|---|
| `rmcp` (公式 Rust SDK) を使う | tokio 系の依存が入り、collab の「古い stable でも通る側に倒す」方針と衝突する可能性が高い。MCP は JSON-RPC 2.0 なので stdio なら自前で足りる |
| 最初から Streamable HTTP | OpenAI Responses API のリモート MCP は**向こうのサーバが接続しに来る**ので `127.0.0.1` では届かず、自宅マシンの外部公開が要る。`verify_job` は自 PC でビルドと exe 実行が走り、`search` は未公開のソース構造と ABI 名を外へ出す。**本当のコストは実装ではなく運用と判断**で、「他社 API を試したい」段階では払う必要がない。日常の Claude Code 利用には一切寄与しない |
| `tools\` を cargo workspace 化 | `build_collab.bat` / `collab_verify.bat` に波及する。独立 crate なら M66 の資産が無傷 |
| 文書本文の BM25 検索 | 識別子は語が短くて BM25 が効きにくい。本文検索が本当に要るのは「なぜこうなっているか」を引くときで、それは ADR 見出しへのポインタで足りる。トークナイザとスコア調整で 1 日増える |
| 7 本フル (`img_diff` / `selftest` / `rules` / `hash_diff` も包む) | `--img-diff` は既に `[img-diff] FAIL: WxH maxDiff=N diffPixels=N` の 1 行 + exit 0/1/2 で機械可読。`selftest` / `rules` はログが短く旨みが薄い。包むほど保守コストだけ増える |
| 索引をサーバが自動再生成 | 読み取り専用のはずの道具が副作用で exe を起動しファイルを書く。`stale` フラグを返して人に判断させる方が予測可能 |
| エンジンに小型 LLM を載せる | 姉妹メモ §1・§3。1B 級は `kNetMaxSpeculation` のような固有語彙を捏造し、`Interop.cs` が位置ベースのミラーで実行時の版検証を持たないこのリポでは害が一般より大きい |
| notes の上書きを許す | 他社 LLM が承認なしに叩ける口に不可逆な操作を置かない。追記のみなら「現在地」は `note_read` の最新 N 件で取れる |
| notes を git 追跡する | レビューされていない記述がコミットに混ざる。人が `docs\` へ昇格させる 2 段構えにする |

---

## 10. 未決

- [ ] 着手時期 — 三校企画 (作業ツリーに未コミット 12 ファイル / +309 行) との優先度
- [ ] 日英同義語表の粒度 — `LocalizationTable.inl` の 1269 行から機械生成するか、手書きの
      小さな表を足すか。M71b で決める
- [ ] `search` の `limit` 既定値 — §8-1 の実測後
