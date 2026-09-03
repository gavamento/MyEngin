# sub-06: M66f: fetch / pull / push + 定期 fetch + 通知 + EditorSettings

- 依存: sub-04 (sub-05 とは独立。同時に進めるなら `SourceControlWindow.cpp` と `ops.rs` のマージ順を司会が決める)
- 状態: OK (commit: M66f — ハッシュは harness.md のサブ進捗表)
- 往復: 1

## やること

spec §4.1 (op: fetch / pull / push / remote_state、背景 fetch と認証) / §4.2 (EditorSettings) / 決定 5, 6 / §5 の 2 (2 clone), 4(h), 11。

1. **Rust ops**: `fetch` (`git fetch --prune`) / `pull{allowMerge}` (既定 `--ff-only`、非 ff は `non_fast_forward`; `allowMerge=true` は `--no-rebase`、競合は `conflict` = sub-07) /
   `push{setUpstream}` (upstream 無しなら `-u origin <branch>`、拒否は `non_fast_forward`、認証失敗は `auth_failed`、到達不能は `network`) /
   `remote_state` (`{upstream, ahead, behind, commits[{sha, author, date, subject}]}`、`commits` は `HEAD..@{u}` の最大 20 件)。
   **定期 fetch**: worker が `hello` の `autoFetch` / `fetchIntervalMin` に従い、起動直後 + 間隔ごとに `fetch` → `remote_state` → 変化があれば `remote_changed` を通知。
   背景 fetch は env `GIT_TERMINAL_PROMPT=0` + `GCM_INTERACTIVE=never`、失敗は**同じ `error.code` が続く間 1 回だけ** `service_error` ではなく `remote_changed{error}` で通知。
   ユーザー操作の fetch / pull / push は `GIT_TERMINAL_PROMPT=0` のみ (GCM の GUI を許す)。設定変更は `hello` の再送で反映。
2. **C++**: Changes タブ上部に帯「upstream に N 件の新しいコミット」(展開でコミット一覧: author / subject) + `ToastCenter` (起動時 fetch の結果)。
   ボタン: Fetch (ゲート不要、`OpInFlight` のみ) / Pull (トランザクション経由 = ゲート適用、`non_fast_forward` → 「マージして pull」を提示 → `allowMerge=true` で再実行) / Push (ゲート不要、`non_fast_forward` → 「先に pull」、`auth_failed` → Credential Manager の案内)。
   **自動 pull はしない**。
3. **`EditorSettings`**: `scmAutoFetch` (true) / `scmFetchIntervalMin` (5)。窓の歯車ポップアップで編集 → `Save()` → `hello` 再送。
4. **冒頭確認**: GUI 無しの Editor.exe (実際は GUI アプリ) の孫プロセス git から GCM のダイアログが出るか、認証未設定の https リモートで 1 回実験して実装メモに結果を書く。出なければ `auth_failed` の案内文を「ターミナルで一度 `git push` して認証を通してください」にする。
5. **collab_verify**: `tests\collab\04_remote.ndjson` = bare リポを origin に 2 クローン (A / B) → A で commit → push → B で fetch → `remote_state` behind=1 → pull → 一致 / B の非 ff push → `non_fast_forward`。
6. **セルフテスト (h)**: `EditorSettings` の新キーを一時ディレクトリで Save → Load 往復、旧 JSON (キー無し) の既定値。

## やらないこと (このサブでは)

- 競合の解決 UI (sub-07)。認証 UI。

## 触る場所 (planner の見立て)

- 変更: `ops.rs` / `worker.rs` (タイマー)、`SourceControlWindow.cpp`、`GitTransaction.cpp` (pull の再実行分岐)、`src\Editor\EditorSettings.{h,cpp}`、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`、`tests\collab\04_*.ndjson`、`tools\collab_verify.ps1` (bare origin の作成)。

## 受け入れ条件 (このサブ)

1. `collab_verify.bat` 緑 (04 を含む)。
2. `--selftest` 緑 (h)。
3. 実機: 2 つ目のクローンから push → fixture 側のエディタで 5 分以内 (間隔を 1 分にして試す) にトースト + 帯。Pull → シーンが開き直る (B) か絵が変わる (A)。push → `git log origin/main` に載る。
4. 実機: 非 ff push → 「先に pull」。`scmAutoFetch=false` で fetch が止まる (Rust のログで確認)。
5. `cargo test` / `check_rules.ps1` / `replay_verify.bat` 緑。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視。origin は collab_verify が作る bare を流用してよい)
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### round 1

SELF_EVAL: sub-06 (round 1)

実装:
  - **冒頭確認 (GCM のダイアログ)**: `CreateNoWindow` + stdio リダイレクトの孫プロセス git から
    `git credential fill` を走らせて実測 (2026-09-03、GCM 2.x / git 2.48.1):
    env 無し = GUI ダイアログ「Git Credential Manager」が**出る** / `GIT_TERMINAL_PROMPT=0` だけ =
    **やはり出る** (GCM は GIT_TERMINAL_PROMPT を端末プロンプトの可否としか読まない) /
    `GIT_TERMINAL_PROMPT=0` + `GCM_INTERACTIVE=never` = 出ない (`fatal: Cannot prompt because user
    interactivity has been disabled.`)。**spec の仮置き 3 はそのまま成立**したので案内文の方針は変えず、
    「資格情報マネージャーが認証を求めます。閉じてしまった場合はターミナルで一度 git push を…」
    (`Scm_AuthHint`) に落とした。★逆に **`GCM_INTERACTIVE=never` が無いと、誰も見ていない 5 分ごとの
    fetch がダイアログを画面に積み上げる**ことも確定した (背景 fetch が別関数である理由)。
    実測手順は `tools\collab\src\git.rs` の `run_background` のコメントに残した。
  - `tools\collab\src\git.rs::run_background` — 背景 fetch 専用の git 呼び出し (`GCM_INTERACTIVE=never`)。
    `classify_error` に `not possible to fast-forward` / `can't be fast-forwarded` を追加
    (`pull --ff-only` の分岐拒否。実測の原文をコメントに引用)。
  - `tools\collab\src\ops.rs` — `fetch` / `pull{allowMerge}` / `push{setUpstream}` / `remote_state` の 4 op
    (dispatch に 4 行)。`pull` の応答は checkout と同型 `{head, names, status, remote}`。
    `push` は upstream 不在なら `-u origin <branch>` (detached HEAD は `bad_request` で止める)。
    `remote_state` = `{upstream, hasRemote, ahead, behind, commits[≤20]}`。
    `background_fetch()` = 定期 fetch の本体 (成功 → `refresh_status` + 変化時のみ `remote_changed`、
    失敗 → **同じ code が続く間 1 回だけ** `remote_changed{error}`)。
    `stable_non_fast_forward()` = 非 ff の detail を固定文へ差し替え (下の「仕様との差分」参照)。
  - `tools\collab\src\worker.rs` — **スレッドを増やさずに**定期 fetch を回す (spec §4.0)。
    `recv` → `recv_timeout(1s)` + `handle_tick`。`Service::with_watch` / 新設 `with_timer` (テスト用) が
    background=true、`new` (CLI) は従来どおり `recv` で寝たまま = event 行を出さない。
    `hello` がタイマーを組み直す (`next_fetch_at`) ので、設定変更は hello の再送で反映される。
  - `src\Editor\SourceControl\CollabProtocol.h` — op 定数 4 本 + `non_fast_forward` / `auth_failed`。
    C ABI は増やしていない / PROTO_VERSION も据え置き (追加は bump しない = spec §4.2)。
  - `SourceControlState.{h,cpp}` — `RemoteState` + 純関数 `BuildRemoteState`、
    `RequestFetch` / `RequestRemoteState` / `Pull` / `Push` / `TakeRemoteChanged` / `TakeFetchError` /
    `ApplyFetchSettings`。`Start` に `autoFetch` / `fetchIntervalMin` を追加 (hello に載る)。
    通知の受け口をラムダから `ApplyEvent` へ抽出し、**`SetEventHandler` を DLL ロードより前**に移した
    (DLL が無くても配線が生きる = セルフテストが偽の通知行で検査できる)。
  - `GitTransaction.{h,cpp}` — `OpKind::Pull` + `RequestPull()`。申し送りどおり差し替えたのは
    (a) 予測 (`SendPredict` の target = `@{u}`) と (b) 実行 (`BeginOp` の分岐) の 2 箇所だけで、
    `ApplyResult` 以降は checkout と完全に共通。非 ff で拒否されたら Report に
    「マージして取り込む」を出し、押されたら `allowMerge_=true` で `BeginOp` を再入。
    `CheckoutResult` → **`TreeOpResult` に改名** (checkout と pull で 1 つの型を共有)。
  - `SourceControlWindow.{h,cpp}` — Changes タブ上部に `DrawRemoteBar`:
    ボタン 3 個 (Fetch / Pull / Push) + 帯 (「1 new commit(s) on origin/main」、▶ で author/subject 一覧)。
    Pull だけゲート + `behind > 0` を要求。歯車ポップアップに背景 fetch の設定 (チェック + 分。1〜1440 に丸める)。
    ヘッダのエラー欄に「次に何を押すか」を追加 (非 ff → 取り込み / auth_failed → 資格情報の案内)。
  - `EditorSettings.{h,cpp}` — `scmAutoFetch` (true) / `scmFetchIntervalMin` (5)。
  - `EditorApp.cpp` — `Start` へ設定を渡す / `requestPull` / `applyFetchSettings` (保存 + hello 再送) /
    背景 fetch のトースト 2 種 (件数 / 失敗) / push 成功のトースト。
  - `tests\collab\07_remote.ndjson` — bare origin + 2 クローンの往復 13 応答。
    planner の should (**`-t` 付き checkout**) もここで実走した (id 11/12 = `origin/feature` へ乗ると
    locals に `feature` が upstream 付きで現れ current になる = detached ではない証拠)。
  - `SourceControlSelfTest.cpp` — (h) EditorSettings の往復 + 旧 JSON の既定値 + マージ保存、
    (k) `BuildRemoteState` の純関数検査と `remote_changed` の配線 (成功 / 失敗 / 二重取り出し) と op 分類。
  - `LocalizationTable.inl` — M66f の 18 文字列。**不要になった `Scm_SettingsEmpty` を削除**
    (「背景 fetch の設定は pull / push と一緒に入ります」= このサブが実装したので嘘になった)。

仕様との差分:
  - [追加] `remote_state` に **`hasRemote`** を足した (spec の列挙は `{upstream, ahead, behind, commits}`)。
    理由: リモート未設定で push すると git は `fatal: No configured push destination.` = `git_failed` を返し、
    UI には「押したのに謎のエラー」しか出ない。ボタンを塞ぐ判断材料が他に無い。
  - [追加] `fetch` / `pull` / `push` の応答に **`remote`** を載せた。理由は spec §4.1「ブランチ周り」と同じ
    (2 往復目を挟むと監視の status が割り込む)。帯の更新に必要。
  - [逸脱] **`non_fast_forward` の `detail` をサービス側の固定文に差し替えた**
    ("the remote has commits that are not in this branch")。spec は `local_changes_overwritten` にしか
    固定文を要求していないが、git の原文には**短縮 SHA** (`a609161..68eeffa`) と版で変わる hint
    (`Disable this message with "git config set advice.diverging false"`) が載り、期待 NDJSON が
    **毎回**赤くなる (1 度撮って気付いた)。UI は既知 code を `Tr()` に置き換えるので detail は表示に
    使われない = 固定文にして失うものは無い。分類できない `git_failed` は従来どおり stderr 全文。
  - [追加] `pull` の失敗分類を `classify_pull_failure` に分けた。マージ競合は git が **stdout** に書くので
    (`Automatic merge failed` / `CONFLICT (content): Merge conflict in x`)、`classify_error` (stderr のみ)
    任せだと `git_failed` に化ける。detail は固定文 = 競合ファイルの列挙は sub-07 の `conflicts` op に譲る
    (`CONFLICT (modify/delete)` など別形があり案内文の解析は必ず外れる)。
  - [追加] Pull ボタンに **`behind > 0`** の条件を付けた。段階の事前判定は `HEAD..@{u}` = fetch 済みの
    追跡ブランチが基準なので、未 fetch の pull は予測が必ず空になり「確認モーダルに何も出ないまま
    working tree が入れ替わる」= 予測を挟んだ意味が消える。behind が立つのは fetch 済みのときだけ。
  - [追加] `SourceControlSession::CheckoutResult` → `TreeOpResult` に改名 (3 箇所)。pull と型を共有するため。
    型を分けると `ApplyResult` 以降の後処理を 2 通り書くことになる。
  - [追加] `Start` が `SetEventHandler` を `client_.Load()` **より前**に呼ぶよう順序を変えた。
    元の順だと DLL ロードに失敗した経路で通知の配線ごと消え、セルフテストから検査できない。
  - [追加] `Scm_SettingsEmpty` を削除 (このサブが中身を実装したので文言が嘘になった)。
    planner が sub-10 へ回した `Scm_ComingSoon` は**触っていない**。
  - [未実装] なし。

検証:
  - `cd tools\collab && cargo test` → **緑 (31 + 21 + 6 = 58 件)**。M66f で 8 本追加:
    fetch→pull の往復 / 非 ff push + ff-only 拒否 + マージ pull + 再 push / pull の競合分類 /
    背景 fetch の失敗を 1 回だけ / 背景 fetch が同僚のコミットを報告 / タイマーが fetch する /
    autoFetch=false でタイマーが黙る / **要求が続いてもタイマーが飢えない**。
  - `tools\build_collab.bat Debug` → 成功 (linker_messages の warning 1 件は既知・無害)。
  - `tools\collab_verify.bat` → **7 シナリオ全 PASS** (07_remote 13 行を含む)。
    07 は 2 回連続で PASS = 期待ファイルが決定的であることを確認済み。
  - `Editor.exe --selftest` (Debug / Release) → **どちらも exit 0**、Source control self test PASSED。
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning。
  - `tools\replay_verify.bat` → **exit 0、10 ジョブ全 PASS** (最終コード状態で再実行済み)。
  - 実機 (`cache\m66f_fx` + bare origin + peer クローン、`--warp --font-embedded`):
    1. 起動 → **定期 fetch が自動で走り** `[collab] background fetch: 1 behind, 0 ahead (origin/main)`。
       窓に帯「1 new commit(s) on origin/main」+ Fetch/Pull/Push の 3 ボタン (`cache\scm_m66f_band.png`)。
    2. Pull (一時プローブで確認ボタンを自動押下) → `pull (2 path(s) changed)` →
       `[reload] texture replaced` → `pull applied: 2 path(s), stage A` →
       **再起動も開き直しも無しに床と立方体の絵が入れ替わった** (`cache\scm_m66f_pulled.png`)。
       ヘッダは `ahead 0 / behind 0`、Pull は自動で無効化。
    3. push → `push done (0 ahead, 0 behind)` + `git log origin/main` に `9549afc local: edit notes` が載った。
    4. 分岐させて push → `non_fast_forward` + 赤字 + 「取り込みを押してから、もう一度送信」
       (`cache\scm_m66f_nonff.png`)。
    5. 歯車の設定ポップアップ (`cache\scm_m66f_settings.png`)。
    6. `.mye\editor_settings.json` に `scmAutoFetch=false` → ログが `auto fetch off` になり、
       **4 分走らせても `.git\FETCH_HEAD` の mtime が動かない** = 定期 fetch が止まっている。
  - ★**実機で 1 件バグを見つけて直した**: 定期 fetch の期限確認を `recv_timeout` の timeout の枝だけに
    置いていたため、1 秒より短い間隔でメッセージが届き続ける限りタイマーが**永久に飢える**
    (同じ fixture で 2 回目に起動すると 2 分半 fetch が 1 回も走らなかった。監視スレッドが
    status の触った `.git\index` を拾って Refresh を投げ返すため)。`handle_tick` を待つ前・毎周回へ移し、
    回帰テスト (`the_timer_is_not_starved_by_a_steady_stream_of_requests`) を足した。
    **修正前は 31 s で FAILED、修正後は PASS** になることを両方確認した。

自己採点 (1-5):
  仕様適合: 5 — sub-06 の 6 項目 (Rust ops / C++ UI / EditorSettings / 冒頭確認 / collab_verify / セルフテスト h)
    をすべて実装し、planner の should (`-t` 付き checkout) も 07_remote で実走。spec §4.0 の
    「join できないスレッドを増やさない」を守り (worker の recv_timeout のみ)、追加した逸脱 6 件は
    すべて上に明記した。
  正しさ: 4 — 5 経路 (fetch / pull / 非 ff pull / push / 非 ff push) を collab_verify と cargo test の
    両方で通し、実機でも 6 項目を観測した。実機で見つけたタイマー飢餓は回帰テスト付きで修正済み。
    4 に留めるのは、(a) 認証失敗の実経路 (`auth_failed`) を**実際の https リモートでは踏んでいない**
    (GCM の挙動は `git credential fill` で確認したが push 経由ではない)、
    (b) pull の段階 B/C (シーンや schemas が pull で降ってくる形) は実機で撮っていない
    (checkout で同じ `ApplyResult` を通す経路は sub-05 が実走済み) の 2 点が未検証のため。
  コード品質: 4 — op の追加は申し送りどおり `dispatch` 1 行 + `collabop` 1 行に収め、C ABI も
    PROTO_VERSION も据え置き。pull と checkout は `TreeOpResult` と `ApplyResult` を共有し、
    後処理の二重実装を作っていない。踏んだ罠 (GCM の env / 非 ff の短縮 SHA / タイマー飢餓 /
    bare の既定ブランチ) はすべて日本語コメントで根拠付きに残した。窓の縦方向がボタン 3 行 +
    帯で少し混んでいるのが 5 にしない理由。
  テスト: 5 — cargo test 8 本 (うち 1 本は今回のバグの回帰テストで、修正前に落ちることを確認済み)、
    collab_verify 1 シナリオ 13 応答、セルフテスト 2 節 (h/k) 12 チェック。
    実機は「ログ + スクショ + FETCH_HEAD の mtime」で観測可能な事実として記録した。

不安・質問:
  1. **Pull ボタンに `behind > 0` を課した**のは planner の裁定が要るかもしれません。「fetch していない
     状態でも Pull を押せる」方が親切という判断なら、予測が空になる問題 (確認モーダルに何も出ない) を
     どう扱うか指示をください。今は「Fetch を押す → 帯が出る → Pull」の順に誘導しています。
  2. `push` の `setUpstream` 引数は**常に false で呼んでいます** (upstream が無ければサービス側が自動で
     `-u origin <branch>` を張るため、UI から明示する必要が無かった)。spec の op 一覧に引数として
     残っているので実装はしてありますが、押す口はありません。sub-10 で削るか、Branches タブに
     「upstream を張って push」を足すかは planner の判断で。
  3. 背景 fetch の失敗トーストは `error.code` だけを出しています (`Scm_FetchFailed` = "背景での取得に
     失敗しました: %s")。detail (git の生の文言) はログにだけ残しました。オフラインの人に
     読める情報が足りなければ文言を足します。

触ったファイル:
  - `tools/collab/src/git.rs`
  - `tools/collab/src/ops.rs`
  - `tools/collab/src/worker.rs`
  - `tools/collab/tests/service.rs`
  - `tests/collab/07_remote.ndjson` (新規)
  - `tests/collab/07_remote.expected.ndjson` (新規)
  - `src/Editor/SourceControl/CollabProtocol.h`
  - `src/Editor/SourceControl/SourceControlState.h`
  - `src/Editor/SourceControl/SourceControlState.cpp`
  - `src/Editor/SourceControl/GitTransaction.h`
  - `src/Editor/SourceControl/GitTransaction.cpp`
  - `src/Editor/SourceControl/SourceControlSelfTest.cpp`
  - `src/Editor/Windows/SourceControlWindow.h`
  - `src/Editor/Windows/SourceControlWindow.cpp`
  - `src/Editor/EditorApp.cpp`
  - `src/Editor/EditorSettings.h`
  - `src/Editor/EditorSettings.cpp`
  - `src/Engine/Core/LocalizationTable.inl`
  (`plans/m66-git-collab/harness.md` と `sub-05.md` は着手時点で既に司会が更新済みでした。私は触っていません)

申し送り:
  - **op を足す形は変わらず** `ops.rs::dispatch` に 1 行 + `CollabProtocol.h` の `collabop` に 1 行
    (+ 読み取り系なら `kReadOps`)。sub-07 の `merge_abort` / `conflicts` / `resolve` / `continue` のうち
    **`continue` は pull と同型の応答** (`{head, names, status}`) にすれば `GitTransaction` は
    `OpKind` を 1 つ足すだけで済みます (`TreeOpResult` と `ApplyResult` がそのまま使えます)。
  - **競合の入口は既にあります**: `pull{allowMerge:true}` が競合すると `error.code=conflict` +
    固定文 `"the merge produced conflicts"` を返し、リポジトリはマージ途中で止まります。
    sub-07 はここから `conflicts` op (git status の未マージ行) を読む想定です。
    ★競合ファイルの一覧を **git の案内文から解析しないこと** (`CONFLICT (modify/delete)` など別形がある)。
  - **Rust の worker にタイマーが載りました**。`background_fetch` は worker スレッドで走るので
    git の直列実行に自然に乗ります (ユーザーの pull と背景 fetch が index.lock で衝突しません)。
    新しい定期処理を足すなら `handle_tick` に相乗りさせてください — **スレッドを増やさない** (spec §4.0)。
  - **`handle_tick` は待つ前に毎周回呼ぶこと**。timeout の枝にだけ置くと飢えます (実機で踏んだ)。
  - **`.txt` のような未知拡張子にもエディタが `.meta` を作ります** (`assets\` 配下)。実機 fixture に
    テキストファイルを置くと未追跡が増え、checkout / pull が `local_changes_overwritten` で
    弾かれる原因になります (sub-05 の「エディタに `.meta` を作らせない」と同じ罠)。
  - **collab_verify の `# write` / `# git` は `..` を含むパスを受け付けます** (`Join-Path $root ...` が
    そのまま解決する)。07_remote はこれで fixture の**兄弟**として origin と workB を作っています。
    `# git -C ../workB ...` は `git -C $root -C ../workB ...` になり、git が -C を順に適用します。
    ★bare origin は必ず `init --bare -b main` で作ること (省くと HEAD が master になり clone がすれ違う。
    実測で「分岐しないはずの分岐テスト」が緑になりました)。
  - collab_verify の次の番号は **08**。
  - **`GCM_INTERACTIVE=never` を背景 fetch から外さないこと**。外すと誰も見ていない 5 分ごとの fetch が
    資格情報ダイアログを画面に積み上げます (実測)。逆にユーザー操作の fetch/pull/push で
    `GCM_INTERACTIVE=never` を**付けない**こと (付けると初回認証が一切できなくなる)。
  - 未検証: `auth_failed` の実経路 (実際の https リモートで push を弾かれる形)。
    pull の段階 B (シーンが降ってくる) と C (schemas が降ってくる) の実機。
    トーストの実表示 (4 秒で消えるので `--screenshot` では撮れない。INFO ログで代替)。
  - 生成物 (gitignore 済み): `cache\m66f_fx` (+ `.gitconfig`)、`cache\m66f_origin.git`、`cache\m66f_peer`、
    `cache\scm_m66f_*.png` 4 枚、`cache\m66f_*.log`、`cache\collab_verify\`。

## フィードバック履歴

## planner 追記 (sub-05 round 1 の判定から。coder は着手前に読む)

- **Rust 側に join できないスレッドを増やさない** (spec §4.0)。定期 fetch は worker スレッドのタイマー (recv_timeout) で実装し、新しいスレッドを立てない。notify の Drop が join しないせいで `FreeLibrary` を撤去した経緯 (CollabClient.cpp:276-282) を読むこと。
- [should] bare origin ができたら **リモート追跡だけのブランチへの `-t` 付き checkout** を collab_verify に 1 本足す (sub-05 で未実走の経路)。
- pull の応答は checkout と同じ型 `{head, names, status}` に揃える (spec §4.1「実行後の変更集合は op の応答に載せる」)。`GitTransaction` は `OpKind` を 1 つ足し、`SendPredict` と `BeginOp` の 2 箇所だけ差し替える (coder 申し送り)。
- `Scm_ComingSoon` は未使用。sub-07 の競合 UI で使わないなら sub-10 で消す。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: worker.rs のスレッド生成は watch (sub-02) と worker 本体の 2 箇所のみで増えていない、:113 で `handle_tick` を `recv_timeout` の前に毎周回呼ぶ (飢餓バグの修正位置) / git.rs:125-127 `run_background` だけに `GCM_INTERACTIVE=never` / tests/collab/07_remote.ndjson:62-63 で `origin/feature` への checkout (= sub-05 の should 2 消込)、ops.rs:676 で `refs/remotes` の実在判定 / SourceControlWindow.cpp:420 `canPull = behind > 0 && gateOpen` / EditorSettings.h:25-26, .cpp:29-30,60-61 の 2 キー往復 / golden*.rep 12:09 再生成 (replay 最終状態)。質問 1 (behind > 0) = 採用、2 (setUpstream) = 引数は残し UI なし、3 (code のみ) = 採用。
