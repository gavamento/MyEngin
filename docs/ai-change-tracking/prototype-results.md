# AI Change Tracking: 試作の結果 (M104b)

- 日付: 2026-11-14。設計 ([architecture.md](architecture.md) ほか) が前提にしていたことを、最小の試作 6 件で確かめた記録
- 試作のコードはコミットしない (spec D2)。リポジトリの外 (`%TEMP%\mye_m104b\`) に作って走らせ、手順・コマンド・観測だけをここに残す
- 各試作は「期待を先に書く → 実行 → 観測と照合」の順で行った。予測が外れた点は各節の「予測と違った点」に書く
- 「確認済み」「推測」「未確認」を分ける。実行していないことは確認済みと書かない

## 環境

| 項目 | 値 |
|---|---|
| OS | Windows 11 Home 10.0.26300 |
| Rust | rustc 1.98.0 (88d9e12ae 2026-08-18) / cargo 1.98.0。試作はすべて edition 2021 |
| C++ | MSVC 19.51.36260 (Visual Studio 18 Community の `vcvars64`)。`/EHsc /O2 /std:c++20 /W4 /utf-8` |
| 依存 (解決された版) | windows-sys 0.61.2 / rusqlite 0.42.0 (libsqlite3-sys 0.40.0、SQLite 3.54.1、cc 1.11.2) / sha2 0.11.1 / notify 8.2.2 (mio 1.2.5) / serde_json 1.0.154 |
| 追加で作ったもの | `p1_pipe` (Rust、pipe サーバ)、`p1_client` (C++、overlapped クライアント)、`p2_sqlite`、`p3_notify` (bin 5 本)、`p5_hook`、`p6_job` (C++) |

CI のランナー (`windows-2022`) では何も走らせていない。開発機での結果だけである。

## 結論の一覧

| # | 確かめたこと | 結果 | 設計への反映 |
|---|---|---|---|
| 1 | Rust の pipe サーバ + C++ の overlapped クライアント | 確認済み。`windows-sys` 0.61 を採る | [ipc-contract.md](ipc-contract.md) §1 |
| 2 | rusqlite (bundled) のビルドと WAL | 開発機で確認済み。CI は未確認 (sub-05 で確かめる) | [db-schema.md](db-schema.md) §8 |
| 3 | notify 8 のイベント列 | 確認済み。デバウンスとリネームの扱いを具体化した | [architecture.md](architecture.md) §4、§5 |
| 4 | C# コンポーネントを `FieldFromJson` で書けるか | 書けない (コードで確定)。別の経路で Entity / Component / Field とも戻せる見込み | [restore-safety.md](restore-safety.md) §4.2 |
| 5 | フックの入力と設定 | Claude Code は文書で確認済み。入力の形から After のハッシュを計算できた。Codex / Gemini は未確認 | [attribution-rules.md](attribution-rules.md) §6.1 |
| 6 | 親と運命を共にしないか | 親の Job 次第。`CREATE_BREAKAWAY_FROM_JOB` の試行と縮退を決めた。本物の Claude Code での終了実験は未実施 | [architecture.md](architecture.md) §3 |

---

## 試作 1: 名前付きパイプ (Rust サーバ、C++ クライアント)

### 期待 (実行前)

- Rust から `CreateNamedPipeW` に SDDL 由来の DACL と `PIPE_REJECT_REMOTE_CLIENTS` を付けて張れる。
- C++ のクライアントがスレッドなしの overlapped I/O で、100 本の短い行と 1 MiB 近い 1 行を同時に流し、全部が同じ内容で返る。
- `GetNamedPipeClientProcessId` がクライアント自身の PID を返す。
- 2 本目の接続も 1 回で開ける。

### 手順

1. サーバ (`p1_pipe`、`windows-sys` 版)。呼び出しの順は次のとおり。
   - `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY)` → `GetTokenInformation(TokenUser)` → `ConvertSidToStringSidW` で自分の SID を文字列にする。
   - SDDL `D:P(A;;GA;;;<自分の SID>)` を `ConvertStringSecurityDescriptorToSecurityDescriptorW` で `SECURITY_ATTRIBUTES` にする。
   - `CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX [| FILE_FLAG_FIRST_PIPE_INSTANCE (最初の 1 本だけ)], PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa)`。
   - `ConnectNamedPipe` → 接続ごとに 1 スレッド (`GetNamedPipeClientProcessId` を呼んでから 1 行ずつ読み、行の長さ・FNV-1a 64・クライアント PID を JSON で返す)。
2. クライアント (`p1_client`、C++)。`CreateFileW(FILE_FLAG_OVERLAPPED)` で 2 接続 (A、B) を開き、`Poll()` は絶対に待たない作りにした (`WriteFile` / `ReadFile` が `ERROR_IO_PENDING` なら保留し、次の `Poll()` で `GetOverlappedResult(bWait = FALSE)`)。A は短い行を 100 本、B は 1,048,000 バイトの 1 行を送る。
3. 比較用に、`PIPE_REJECT_REMOTE_CLIENTS` を外したサーバ (環境変数 `P1_NOREJECT`) も作り、`\\localhost\pipe\<名前>` で開けるかを見た。

```powershell
cd %TEMP%\mye_m104b\p1_pipe;   cargo build
cd %TEMP%\mye_m104b\p1_client; cl /nologo /EHsc /O2 /std:c++20 /W4 /utf-8 client.cpp /Fe:client.exe
Start-Process p1_pipe.exe '\\.\pipe\mye-track-proto'    # サーバ
client.exe                                              # 既定の名前で往復
p1_pipe.exe '\\.\pipe\mye-track-proto'                  # 2 本目のサーバ (同名)
```

### 観測

最終版のクライアントの出力 (サーバに `quit` を送って終了させた回)。

```
self pid=33152 busyRetries A=0 B=1
A lines=100 B lines=1 total=46.9ms polls=648325 maxPoll=0.205ms
A mismatches=0
B match=1 resp={"id":7777,"ok":true,"result":{"len":1048000,"fnv":"038f58c0d2423204","client_pid":33152,"pid_api_ok":true}}
remote open via localhost: handle=INVALID err=5
```

- 100 本 + 1 MiB 近い 1 行が 47 ms で往復し、全部の長さとハッシュが一致した。`Poll()` 1 回の最長は 0.2 ms (約 65 万回呼んだ中で)。
- `GetNamedPipeClientProcessId` の値はクライアントの `GetCurrentProcessId()` と一致した。
- サーバが出した SDDL: `D:P(A;;GA;;;S-1-5-21-...-1001)`。実際の DACL を `(Get-Acl '\\.\pipe\<名前>').Sddl` で読み戻すと `O:<自分>G:<自分>D:P(A;;FA;;;<自分>)` (所有者・グループ・許可 1 件だけ、継承なし)。
- `\\localhost\pipe\<名前>` で開く試験は、`PIPE_REJECT_REMOTE_CLIENTS` ありで `ERROR_ACCESS_DENIED (5)`、外したサーバでは開けた (`handle=VALID`)。DACL は同じなので、拒否の原因はフラグである。
- 同名の 2 本目のサーバは `create failed err=5` (`FILE_FLAG_FIRST_PIPE_INSTANCE` が効いた)。

### 予測と違った点

- **2 本目の接続が `ERROR_PIPE_BUSY (231)` で開けなかった** (最初のクライアントが接続した直後)。サーバは接続を引き渡してから次のインスタンスを作るため、その間は空きインスタンスが無い。クライアントは `WaitNamedPipeW` で待って開き直す必要がある (上の観測は 1 回の再試行で開けた結果)。
- クライアントを最初に書いたとき、同じ名前の `PIPE_READ_MODE_BYTE` が無いというコンパイルエラーと、`ReadFile` の引数の型が違うというエラーが出た。Win32 の定数名・型を `windows-sys` がコンパイル時に検査してくれた例である。

### Win32 API の呼び方: `windows-sys` か自前の `extern "system"` か

**`windows-sys` 0.61 を採る。** 理由:

- 使った関数は 14 個 (上の手順の関数と `LocalFree`、`CloseHandle`、`GetLastError`、`DisconnectNamedPipe`、`ReadFile`、`WriteFile`)、構造体 3 個 (`SECURITY_ATTRIBUTES`、`TOKEN_USER`、`SID_AND_ATTRIBUTES`)、定数 10 個以上。自前の宣言だと、これを手で書き写し、定数の値と型の幅を目で確かめることになる。誤りは実行時の未定義動作かアクセス拒否として現れる。`windows-sys` ではコンパイルが検査する (上のエラー 2 件が実例)。
- 依存の追加はほぼゼロ。`notify` 8.2.2 が `windows-sys` 0.61.2 を引き込む (`tools\collab` の古い `Cargo.lock` には 0.60.2 と 0.61.2 の両方が既にある)。`tools\track` では `windows-sys` と `windows-link` の 2 パッケージが増えるだけで、どのみち `notify` が入れる。
- ビルド時間は無視できる (`windows-sys` を含む pipe サーバのまっさらなデバッグビルドが 3.1 秒)。
- 自前宣言は `tools\collab\src\git.rs` の前例のように 4 関数なら読める量だが、本試作は 14 関数 + 構造体で、増えるほど保ちにくい。

使う feature: `Win32_Foundation`、`Win32_Security`、`Win32_Security_Authorization`、`Win32_System_Pipes`、`Win32_Storage_FileSystem`、`Win32_System_IO` (`ReadFile` の `OVERLAPPED` に要る)、`Win32_System_Threading` (`GetCurrentProcess`、`OpenProcessToken`)。`Win32_System_IO` を外すとコンパイルが通らないことを確かめた。
`windows-sys` は生のバインディングで、ハンドルの `Drop` を持たない。本実装では `CloseHandle` を呼ぶ小さな RAII 型を 1 つ書く。

### 未確認の残り

- **別のユーザーから接続できないこと**は試していない (開発機にもう 1 人のアカウントが無い)。DACL の読み戻しで許可が自分の SID だけであることは確認した。管理者権限の別プロセスは DACL の対象外であり、管理者が自分の DACL を書き換えて開けることは Windows の仕様で防げない (脅威の範囲外)。
- **本物のリモートからの接続**は試していない。`\\localhost\pipe\` 経由 (SMB のリダイレクタを通る経路) で、フラグの有無による差を確かめただけである。
- C++ クライアントは試作の `Poll()` で、Editor の既存コード (`CollabClient`) には載せていない。Editor 側の実装は sub-09 (M104j)。

---

## 試作 2: SQLite (`rusqlite` bundled)

### 期待 (実行前)

- edition 2021 の小さい bin が `rusqlite` の `bundled` でビルドできる。C コンパイラは `cc` クレートが Visual Studio を探して使う。
- `PRAGMA journal_mode=WAL` が `wal` を返し、書き込みトランザクションを開いたまま別接続から読める (未コミットの行は見えない)。
- ビルドは数十秒、`target` は数十〜百 MiB。

### 手順

```powershell
cd %TEMP%\mye_m104b\p2_sqlite
cargo add rusqlite --features bundled; cargo add sha2    # 0.42.0 / 0.11.1 が選ばれた
cargo build                                              # まっさら (cargo clean の後) で時間を測る
cargo build --release
p2_sqlite.exe %TEMP%\mye_m104b\p2_data
```

試作のコード: DB を開いて `PRAGMA journal_mode=WAL`、`synchronous=NORMAL`、`foreign_keys=ON`。`blobs` / `revs` の 2 表 (外部キーあり) と索引を作り、10000 行 × 2 表を 1 トランザクションで入れる。外部キー違反の拒否、書き込みトランザクション中の別接続の読み、`integrity_check` を見る。

### 観測

```
journal_mode=wal synchronous=1 foreign_keys=1 sqlite_version=3.54.1
insert 10000x2 in one tx: 403.8 ms            (デバッグビルド)
fk violation rejected: true
reader during open write tx sees 10000 blobs (uncommitted 'zzz' hidden: true)
reader after commit sees 10001 blobs
indexed query count=100
integrity_check=ok
track.db exists=true len=1892352   /   track.db-wal, track.db-shm: 最後の接続を閉じると消える
```

| 項目 | 値 |
|---|---|
| まっさらなデバッグビルド | 8.8 秒 (`cc` が Visual Studio 18 を自分で見つけた。`vcvars` の事前呼び出しは不要) |
| `target` の大きさ | デバッグ 97 MiB / リリース 71 MiB (リリースは追加で 27.7 秒、exe は 1.8 MiB) |
| 依存パッケージ数 | `Cargo.lock` に 33 件 (試作自身と、`rusqlite` + `sha2` の推移的依存を含む) |
| 最低の Rust の版 | `rusqlite` 0.42.0 と `libsqlite3-sys` 0.40.0 が `rust-version` 1.89 / 1.88、`sha2` 0.11.1 が 1.88 |

### 予測と違った点

- **`sha2` 0.11 の出力は `{:x}` で整形できない** (`LowerHex` が実装されていない。最初のビルドがこれで失敗した)。16 進文字列への変換は自分で書く (`iter().map(|b| format!("{:02x}", b))`)。
- **最低の Rust の版が 1.89 に上がる。** `tools\collab\Cargo.toml` は「古い stable でも通る側に倒す」(edition 2021、`notify` 8 止まり) と書いているが、`rusqlite` を入れるとその方針は `tools\track` では成り立たない。開発機は 1.98、CI は `dtolnay/rust-toolchain@stable` なので実害は無い。本実装では `tools\track\Cargo.toml` に `rust-version = "1.89"` を明示し、古い rustc でのビルド失敗を依存のエラーではなく版の不足として読めるようにする。edition は 2021 のままで通る。

### 結論

- 採る版: `rusqlite = { version = "0.42", features = ["bundled"] }`、`sha2 = "0.11"`。
- 起動時の PRAGMA は `journal_mode=WAL`、`synchronous=NORMAL`、`foreign_keys=ON`。
- SQLite を C++ 側へ置く案 (単一ソースを `external\` へ) は取らない (サービスは Rust)。ビルド済みの SQLite や別の保存形式に変える必要は出なかった。

### 未確認の残り

- **CI のランナーでのビルド。** `windows-2022` の Visual Studio 2022 で `bundled` が通るかは、push しないと確かめられない (spec D3)。sub-05 (M104f) で `tools\track` を CI に入れるときに確かめる。通らなければ、`cc` が見るコンパイラの指定 (`CC` 環境変数) を CI 側に足す。
- 並行書き込みの競合 (複数のプロセスが同じ DB を書く場合) は試していない。設計は「Writer 1 本」(architecture §3.1) で、書き手は 1 つ。

---

## 試作 3: `notify` 8 のイベント列

### 期待 (実行前)

- `notify` 8 (`default-features = false`、Windows は `ReadDirectoryChangesW`) は、連続書き込みを何件かの `Modify` にし、削除を `Remove`、同じディレクトリ内のリネームを `Modify(Name(From))` と `Modify(Name(To))` にする。
- 「一時ファイルへ書いて rename」は、`Create` + `Modify` + `Name(From)` + `Name(To)` として見える。
- 1000 ファイルの一括変更で、取りこぼしかエラー (バッファあふれ) が出るかもしれない。

### 手順

小さい bin が一時ディレクトリを `RecursiveMode::Recursive` で監視し、操作を実行して、イベントの種別 (`{:?}`) とパスをそのまま記録する。操作は 1 件ずつ、400 ms 静かになるまで集めてから次へ進む。`WriteFileReplacing` ([PathUtil.cpp](../../src/Engine/Platform/PathUtil.cpp)) と同じく、一時ファイルの名前は `<path>.<pid>.<tid>.<n>.tmp`、置き換えは `std::fs::rename` (既存を置き換える)。

```powershell
cd %TEMP%\mye_m104b\p3_notify; cargo build
target\debug\p3_notify.exe  %TEMP%\mye_m104b\p3_watch     # (a)〜(f) の全場面
target\debug\latency.exe    %TEMP%\mye_m104b\p3_watch2    # 到着時刻を notify のコールバックの中で測る
target\debug\sharecheck.exe                               # 書き込み中のファイルを FILE_SHARE_READ だけで開く
target\debug\renames.exe    %TEMP%\mye_m104b\p3_watch3    # 200 件のリネームの対応
target\debug\moveout.exe    %TEMP%\mye_m104b\p3_watch4    # ツリーごと監視の外へ移す
```

### 観測

イベントの種別は `Create(Any)` / `Modify(Any)` / `Modify(Name(From|To))` / `Remove(Any)` だけだった。

| 場面 | 観測したイベント列 |
|---|---|
| 新規ファイル 1 本 | `Create` + `Modify` (2 件) |
| (a1) `fs::write` (切り詰め + 書き込み) を 5 ms 間隔で 20 回 | `Modify` が **40 件** (1 回につき 2 件)。パスは同じ |
| (a2) 追記 (open / write / close) を 5 ms 間隔で 20 回 | `Modify` が 20 件 (1 回につき 1 件) |
| (a3) 開いたままのハンドルに 5 回 write + flush | `Modify` が **1 件** |
| ストリーミング (1 ハンドルに 64 KiB を 50 ms ごと × 20) | `Modify` が **2 件**。1 件目は開始の 0.4 ms 後、2 件目は**ハンドルを閉じた時刻** (開始から 1007.8 ms)。途中の書き込みでは届かない |
| ストリーミング (1 KiB を 5 ms ごと × 40) | 同じく 2 件 (開始と閉じた時刻) |
| 50 MiB を 1 回で書く | `Modify` 2 件。最初は 8.9 ms、最後は 71.8 ms で、書き終わりの時刻に重なる |
| (b) 削除 | `Remove` 1 件 |
| (c1) 同じディレクトリ内のリネーム | `Name(From)` (旧名) → `Name(To)` (新名) が**隣り合って 2 件** |
| (c2) **別のディレクトリへ移す** | `Remove(旧パス)` + `Create(新パス)` + 親ディレクトリの `Modify`。**リネームとしては届かない** |
| (d1) 一時ファイルへ書いて既存へ rename | `Create(tmp)`, `Modify(tmp)`, **`Remove(x.json)`**, `Name(From)(tmp)`, `Name(To)(x.json)` の 5 件が 0.5 ms 以内に並ぶ |
| (d2) 同じく新しい名前へ | `Create(tmp)`, `Modify(tmp)`, `Name(From)`, `Name(To)` の 4 件 |
| (d3) 5 ms 間隔で 5 回置き換え | (d1) の 5 件が 5 組。計 25 件 |
| (e1) ディレクトリの名前変更 (中に 3 ファイル + 子ディレクトリ) | `Name(From)(dirA)`, `Name(To)(dirB)`, `Modify(dirB)` の **3 件だけ**。中のファイルのイベントは無い |
| (e2) 名前変更後のディレクトリの中の書き込み | `dirB/inner/deep.txt` の `Modify` が届く (監視は新しい名前でも続く) |
| (e3) ツリーごと削除 | ファイルごと・ディレクトリごとの `Remove` (6 件) |
| (e4) 外で作ったツリーを監視ルートの中へ移し入れる | `Create(arrived)` + `Modify(arrived)` の **2 件だけ**。中の 4 ファイルのイベントは無い |
| ツリーを監視ルートの外へ移し出す | `Remove(tree)` の **1 件だけ**。中のファイルのイベントは無い |
| (f1) 1000 ファイルの一括作成 (432 ms で書いた) | 2999 件 (`Create` ×1000、`Modify` ×1999)。1000 本すべてに 1 件以上。エラーなし |
| (f2) 1000 ファイルの一括変更 | 2001 件 (`Modify`)。1000 本すべてに届いた。エラーなし |
| (f3) 1000 ファイルの一括削除 | 1001 件 (`Remove`)。1000 本すべてに届いた。エラーなし |
| 200 件のリネームを続けて | `Name(From)` / `Name(To)` が 400 件。隣り合った対 200 組がすべて同じ番号で一致。`tracker` は付かない (0 件) |

- 到着の遅さは小さい: 1 KiB の書き込みで最初のイベントが 0.4 ms、一時ファイル + rename の 5 件は 0.5 ms の幅に収まる。
- 書き込み中のファイルを `FILE_SHARE_READ` だけで開く試験 (`sharecheck`): 別スレッドが `File::create` (共有は Rust の既定 = READ | WRITE | DELETE。Node の libuv と同じ) で書き続けている間、**8 回中 5 回が `os error 32` (共有違反)**。書き手が閉じた後 (500 ms 以降) は開けた。

### 予測と違った点

- **(a3) とストリーミング**: 開いたままのハンドルへの書き込みは、途中では 1 件も届かず、閉じたときに 1 件届く。「連続書き込みは細かい `Modify` が並ぶ」という予測は外れた。静かになるのを待つデバウンスだけでは、書き込みの途中で Before / After を取ってしまう。
- **(c2)**: 別ディレクトリへの移動がリネームとして届かない (`Remove` + `Create`)。期待は「`Name(From)` と `Name(To)` が出る」だった。
- **(d1)**: 置き換えの rename に `Remove(x.json)` が混じる。
- **(e1)、(e4)、移し出し**: ディレクトリ単位の移動では、中のファイルのイベントが届かない。
- 1000 ファイルの一括変更でバッファあふれ (エラーイベント) は**再現しなかった** (予測は「出るかもしれない」)。再現しなかったことは、あふれないことの証明ではない。

### 結論 (設計へ反映したこと)

1. **デバウンスはパスごと。** 最後のイベントから 300 ms 静かになったら flush する。最初のイベントから 2 秒を超えたら、まだ続いていても flush する (collab の `watch.rs` と同じ値 `DEBOUNCE = 300 ms` / `MAX_HOLD = 2 s`)。1 回の保存 (書き込み 2 件、または rename の 5 件) は 1 ms 以内に収まるので、300 ms は十分に広い。この値は初期値で、sub-03 (M104d) で実測を見て調整する。
2. **書き込み中の検出。** (sub-03 で見直し: この探りは書き手の rename・書き込み open を失敗させたため採らなかった。現在の方式は [architecture.md](architecture.md) §4。以下は試作 3 当時の結論。) flush で内容を読むときは `FILE_SHARE_READ` だけで開く。共有違反 (`os error 32`) なら、書き手がまだ開いているので読まずに 250 ms 後にやり直す。30 秒たっても開かれたままなら、そのときの内容を読み、その Revision を `coalesced` にする (中間状態の可能性がある)。書き手が閉じたときの最後のイベントでもう一度 flush が走るので、最終状態の Revision が別に残る。
3. **イベントの数を数えない。** (a3) のとおり件数は書き込みの回数を表さない。flush の時点で内容のハッシュを取り、スナップショットと違うときだけ変更とする。`Modify(Any)` は内容の変更かどうかを区別しないので、ハッシュが同じなら捨てる。
4. **リネームのまとめ方。**
   - 同じディレクトリ内のリネームは `Name(From)` の直後に `Name(To)` が来る (200 件で対応が崩れなかった)。`tracker` は付かないので、隣り合いで対にする。
   - **別のディレクトリへの移動は `Remove` + `Create` でしか来ない** (c2)。「同じデバウンス窓の中の `Remove` + 同じ内容ハッシュの `Create`」で 1 件にまとめる規則 (architecture §5) が、この経路の**主たる手段**になる。補助ではない。
   - 置き換えの rename (d) は `Remove(x.json)` が混じるが、flush 時点で `x.json` は存在するので、「削除」ではなく「内容の変更」になる。flush の判定は、イベントの種別ではなく、そのときのファイルの存在と内容で決める。
5. **ディレクトリ単位の操作はイベントに頼れない。** ディレクトリの `Create` / `Name(To)` / `Remove` を見たら、保存しているファイル一覧 (ベースライン) と、実際のディレクトリの中身 (走査) を突き合わせて、中のファイルの作成・削除・リネームを作る。(e1) のように名前が変わったディレクトリは、パスの前方一致でファイルの移動と対応づける。走査は 1 ディレクトリずつで、ルート全体の走査はしない。
6. **ディレクトリ自身の `Modify` は捨てる。** ファイルを書くたびに親ディレクトリの `Modify(Any)` が来る。flush でパスの種別 (ファイルかディレクトリか) を調べる。
7. **欠落の扱い。** `notify` のコールバックが `Err` を返したとき、または再走査を求めるフラグが付いたイベントが来たときは、その区間を `gap` として記録し、ベースラインを取り直す (architecture §4 のとおり)。今回の試作ではあふれが起きなかったので、この経路は実験で動かしていない。sub-03 で、人工的にあふれを起こして確かめる。

### 未確認の残り

- バッファあふれ時の `notify` の報告のされ方 (再現しなかった)。
- 数万ファイルのツリーを監視した場合の初期化時間と、`ReadDirectoryChangesW` のバッファの大きさの調整 (今回は 1000 ファイルまで)。sub-03。
- 書き手が「共有違反を起こさない」道具 (共有に `FILE_SHARE_WRITE` を含めずに開く書き手など) のときの挙動。`share=READ` の探りが成り立つのは、書き手が書き込み権限のハンドルを開いている間である。

---

## 試作 4: C# コンポーネントのフィールドを `FieldFromJson` で書けるか

### 期待 (実行前)

C# コンポーネントの ECS 上の中身は managed のハンドルだけで、フィールドは managed 側にある。そのため `FieldFromJson` は使えない。ただし、フィールドを書く既存の経路 (Inspector と、シーンの読み込み) はある。

### 手順

実行は不要と判断し、コードを読んだ (読むだけで決まったため一時プローブは作っていない)。読んだ範囲:

- `src\Engine\Engine\Script\ManagedHost.cpp` の `RegisterTypes` (C# 型の登録)、`SetFieldValue`、`SerializeComponent`、`DeserializeComponent`、`EnsureInstance`
- `src\Scripting\ScriptRuntime.cs` の `ApplyJson`、`Serialize`
- `src\Editor\Windows\Scene\InspectorWindow.cpp` の `DrawManagedComponentFields`、`DrawManagedFieldWidget` と、その呼び出し側
- `src\Engine\Engine\Scene\SceneSerializer.cpp` の保存 (125 行付近) と読み込み (253 行付近)
- `src\Engine\Core\Util\JsonUtil.cpp` の `FieldToJson` / `FieldFromJson`

### 観測 (コードから確認した事実)

- C# コンポーネントの `ComponentDesc` は `size = sizeof(int32_t)`、`flags = kComponentScriptState | kComponentNoHash` で、`fields` (`FieldDesc` の配列) を持たない。ECS のカラムは managed インスタンスのハンドル (int32) だけ。
- `FieldFromJson(void* comp, const FieldDesc& field, const json& value)` は `comp + field.offset` へ書く。C# コンポーネントには `FieldDesc` が無く、仮に作って呼べばハンドルの 4 バイトを壊す。**使えない。**
- managed 側のフィールドを Editor から書く既存の経路は 2 つ。
  1. `ManagedHost::SetFieldValue(handle, 添字, buf, 長さ)` (Inspector が使う)。型は Float / Int32 / UInt32 / UInt64 / Bool / Float2 / Float3 / Float4 / Quat / Color。値の受け渡しは 16 バイトまで。
  2. `ManagedHost::DeserializeComponent(型, エンティティ, ペイロード, json)` (シーンの読み込みが使う)。managed の `ApplyJson` は、JSON に**ある**キーだけを書く (`TryGetPropertyValue` で無いキーは `continue`)。1 つのキーだけの JSON を渡せば、そのフィールドだけが変わる。
- `SceneSerializer` は保存時に `SerializeComponent` を呼ぶので、シーンの JSON (`UndoStack` のスナップショットの元) には C# のフィールドの値が入る。
- Inspector の `DrawManagedComponentFields` は、描画の途中で `SetFieldValue` を直接呼び、Undo の記録をしない (この関数は返り値が `void` で、呼び出し側も Undo を渡していない)。

### 結論

- Entity 単位と Component 単位の復元は、`SceneSerializer::ApplyPartial` が `DeserializeComponent` を通るので、そのまま使える。
- **Field 単位の復元で C# コンポーネントを除外する必要は無い**: 「`DeserializeComponent` に、戻したいフィールド 1 つだけの JSON を渡す」で戻せる。ただし `FieldFromJson` は使わず、C# コンポーネントは別の分岐にする。サービスが出す Field の差分の値は、`FieldToJson` と同じ形ではなく、managed の `Serialize` が出す形 (`ApplyJson` が読む形) になる。sub-16 (M104q) で、この分岐を実装して確かめる。
- Undo: 復元は `UndoStack::Record` の中で行う。スナップショットに C# の値が入るので、Undo で戻る見込みがある (コードを読んだ推測。未実行)。

### 未確認の残り

- 実走していない。`DeserializeComponent` へ 1 キーだけの JSON を渡したときに他のフィールドが変わらないこと、`UndoStack::Record` 経由の復元が Undo で戻ること、プレハブ上書き (override) の記録が C# コンポーネントでどう働くかは、sub-16 でテストを足して確かめる。
- C# コンポーネントのフィールドとして managed が出すのは、`FieldTypeOf` が返す 10 種類 (float / int / uint / ulong・long / bool / Vec2 / Vec3 / Vec4 / Quat / Color。public なインスタンスフィールドを名前順) だけで、string や AssetRef は無い (読んで確認)。すべて 16 バイトに収まり、JSON の表現も数値か数値の配列になる。Field 差分の値の型はこの 10 種類に限られる。

---

## 試作 5: フックの入力と設定

### 期待 (実行前)

- Claude Code のフックは、道具の実行前後に JSON を標準入力へ渡し、`tool_name` と `tool_input` に Edit の `old_string` / `new_string` / `replace_all` (Write は `content`) が入っている。
- この入力から After のハッシュを計算し、実際に編集した結果のハッシュと比べて一致する。

### 手順

#### 5a. Claude Code の公式文書の確認 (planner が実施、coder は取り直していない)

出所: <https://code.claude.com/docs/en/hooks> (2026-11-14 に planner が取得。spec D14)。

- 共通の入力: `session_id`、`transcript_path`、`cwd`、`hook_event_name`。
- 道具のイベント (PreToolUse / PostToolUse): `tool_name`、`tool_input`、`tool_use_id`。PostToolUse はさらに `tool_response`。
- Write の `tool_input`: `file_path`、`content`。Edit の `tool_input`: `file_path`、`old_string`、`new_string`、`replace_all`。
- 設定: `.claude\settings.json` など。`hooks.PreToolUse[].matcher` と `hooks[].type = "command"`。
- **終了コード 2 は PreToolUse で道具を止める。** 他の 0 以外は止めない。時間切れの既定は 600 秒。
- 確認できなかったもの: MultiEdit と NotebookEdit の入力の項目名。

追加で設計に必要な 3 点 (spec D14 の結論):

1. フックは終了コード 2 を**絶対に返さない** (どの失敗でも 0)。
2. 設定の例に `timeout` を明示する (既定の 600 秒に頼らない)。
3. pre と post の対応は `tool_use_id` で取る。

#### 5b. Codex CLI / Gemini CLI

公式ページの存在だけを planner が確認している (spec D15): Codex <https://developers.openai.com/codex/hooks>、Gemini <https://geminicli.com/docs/hooks/reference/>。coder には外部ページを読む手段が無く、中身は取得していない。

**未確認。** (第三者の記事に「Codex の対象は Bash だけ」とあるが、これも未確認。) v1 のフック CLI が対応を約束するのは Claude Code だけで、他のクライアントは `change_report` (Claimed 止まり) で設計どおり成り立つ。

#### 5c. 入力の形から After のハッシュを計算できるか

`p5_hook` (Rust、`serde_json` + `sha2`)。`p5_hook pre|post` が標準入力から D14 の形の JSON を読み、次を結果ファイルへ 1 行追記する。標準出力には何も出さず、終了コードは常に 0。

- pre: 対象ファイルの現在の内容のハッシュ (Before) と、宣言された編集を Before に当てた内容のハッシュ (`declared_after`)。Edit は `old_string` の出現数を数え、0 件なら `not_found`、2 件以上で `replace_all = false` なら `ambiguous`、それ以外は置換。Write は `content` をそのまま。
- post: 対象ファイルの現在の内容のハッシュ (After)。

PowerShell の駆動スクリプトが、**Rust とは別の実装 (`String.IndexOf` / `Replace`) で編集を実際に行い**、pre / post を呼ぶ。`tool_use_id` で pre と post を対にして `declared_after` と After を比べる。入力は `ConvertTo-Json` で組み立てた (Unicode を含む)。

### 観測

```
toolu_1    Edit   before=31b056b2 after=d34b9198 -> declared_edit_match; file changed      (一意の置換、日本語を含む)
toolu_2    Edit   before=c46b303b after=ab5ba3e0 -> declared_edit_match; file changed      (replace_all で 3 か所)
toolu_3    Edit   before=3defe166 after=3defe166 -> no_declared (ambiguous); file unchanged (2 か所に当たる、replace_all なし)
toolu_4    Edit   before=5891b5b5 after=5891b5b5 -> no_declared (not_found); file unchanged
toolu_5    Edit   before=43a81638 after=43a81638 -> no_declared (not_found); file unchanged (CRLF のファイル、LF だけの old_string)
toolu_6    Edit   before=f9809ff9 after=c51560aa -> declared_edit_match; file changed      (CRLF のファイル、CRLF を含む old_string)
toolu_7    Edit   before=ecd2b943 after=64fa6091 -> declared_edit_match; file changed      (new_string が空 = 削除)
toolu_w1   Write  before=none     after=e426e640 -> declared_edit_match; file changed      (新規作成)
bad input []: exit=0 stdout_len=0                         (空、壊れた JSON、tool_input 無し、Bash の入力 のすべてで同じ)
avg process time (debug build, 20 runs): 19.1 ms
```

- 宣言を Before に当てた結果のハッシュは、別実装が実際に行った編集の After と、当てられる 5 件すべてで一致した。当てられない 3 件 (曖昧、見つからない ×2) は、ファイルが変わらないことと一致した (宣言が実現しなかった)。
- どんな入力でも終了コード 0 で、標準出力は空だった。プロセスの起動から終了までは約 19 ms (デバッグビルド)。

### 予測と違った点

なし。すべて予測どおりだった (ただし、After を作ったのは Claude Code ではなく試作の駆動スクリプトで、次の「未確認の残り」の限界がある)。

### 結論

- Claude Code の入力から `declared_edit_match` を計算できる。Edit の `replace_all` は、出現数が 1 件でも複数件でも、宣言の適用の中で扱える。
- 設計 ([attribution-rules.md](attribution-rules.md) §6.1) の前提は成り立つ。追加の制約 3 点 (終了コード 2 を返さない、`timeout` の明示、`tool_use_id` で対にする) を同節へ書いた。
- 行末 (CRLF) は一致判定にそのまま響く。`old_string` が LF だけでファイルが CRLF のとき、宣言は当てられない。このとき Claude Code 自身の Edit がどう振る舞うかは未確認で、当たらなければ Verified にならず Claimed 止まりに落ちる (安全側)。

### 未確認の残り

- **本物の Claude Code が渡す入力の実測。** 試作の入力は文書どおりに作ったもので、本物の入力のキー名・値の形 (たとえばパスの区切り、`tool_response` の中身) は、実際にフックを動かして標準入力を記録しないと確かめられない。ユーザーの設定ファイルを試作のために変更しないので、実施していない。sub-08 (M104i) の `track_verify` か手動確認で確かめる。
- Claude Code の Edit が、文字の正規化 (引用符など) をしてから置換するかどうか。正規化するなら、宣言と実際の After がずれて Claimed 止まりになる。
- MultiEdit / NotebookEdit の入力 (文書で確認できなかった)。対応しない道具は Claimed 止まりでよい。
- Codex CLI / Gemini CLI (上のとおり未確認)。

---

## 試作 6: 親と運命を共にしないか

### 期待 (実行前)

- 親が Job に入っていなければ、親が終わっても子 (サービス) は生き残る。
- 親が `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` の Job に入っていれば、既定では子も同じ Job に入り、Job を閉じると一緒に終わる。
- `CREATE_BREAKAWAY_FROM_JOB` は、Job が `JOB_OBJECT_LIMIT_BREAKAWAY_OK` を持っているときだけ効く。持っていないとき、`CreateProcess` は `ERROR_ACCESS_DENIED` で失敗する。
- `DETACHED_PROCESS` は Job の所属に影響しない。

### 手順

`p6_job` (C++、`jt.exe`) が 2 段の親子を作る。`host` が Job を作り (または作らず)、`mcp` (MCP サーバ相当の親) を中断状態で起動して Job へ入れて再開する。`mcp` は試作 1 のサーバ (`p1_pipe.exe`) を子として、指定した `CreateProcess` のフラグで起動して、子の PID と `IsProcessInJob` を結果ファイルへ書く。`host` はそれを読んでから、親を終わらせる (Job があれば `CloseHandle(job)` で kill-on-close を発火、無ければ `TerminateProcess`)。1.5 秒後に、子が生きているかを `GetExitCodeProcess` で見る。

```powershell
# jobmode: none | kill | kill_bo (+BREAKAWAY_OK) | kill_sbo (+SILENT_BREAKAWAY_OK)
# flags  : 0 | detached | breakaway | both (= DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB)
jt.exe host <jobmode> <flags> p1_pipe.exe '\\.\pipe\mye-track-jN'    # 4 × 4 = 16 通りを実行
jt.exe jobinfo <pid>                                                  # IsProcessInJob(pid, NULL)
```

### 観測 (a): 合成した親・Job の 16 通り

| Job の種類 | フラグ 0 | `DETACHED_PROCESS` | `CREATE_BREAKAWAY_FROM_JOB` | 両方 |
|---|---|---|---|---|
| Job なし (`TerminateProcess` で親を終わらせる) | 子は生存 | 生存 | 生存 | 生存 |
| kill-on-close | **子は道連れ** (子は Job 内) | **道連れ** (Job 内) | **起動に失敗** (`err=5`) | 起動に失敗 (`err=5`) |
| kill-on-close + BREAKAWAY_OK | 道連れ | 道連れ | **生存** (子は Job の外) | **生存** |
| kill-on-close + SILENT_BREAKAWAY_OK | 生存 (自動で外) | 生存 | 生存 | 生存 |

- どの行でも、親 (`mcp`) は終了した。子の `child_in_job` は、道連れになった行で 1、生存した行で 0 だった。
- 予測はすべて当たった。

### 観測 (b): 本物の Claude Code が起動した MCP サーバ

このセッションを動かしている環境で、次を `IsProcessInJob` で調べた (`jt.exe jobinfo <pid>`)。

```
pwsh.exe    -> in_job=1          cmd.exe     -> in_job=1
claude.exe  -> in_job=1          warp.exe    -> in_job=1   (端末)
explorer.exe -> in_job=0
mye_mcp.exe (親は claude.exe) -> in_job=1
```

- **本物の `mye_mcp.exe` は Job に入っている。** `claude.exe` も入っている。そのため「MCP が起動した子は親と同じ Job に入る」可能性が実際にある。
- その Job が kill-on-close か、`BREAKAWAY_OK` を持つかは**読めなかった** (`QueryInformationJobObject(NULL, ...)` が `ERROR_ACCESS_DENIED`)。
- このセッションのシェルから `jt.exe mcp` を直接起動すると、`jt.exe` 自身が Job に入っていなかった (`self_in_job=0`)。シェルが Job に入っていても、起動した子が入るとは限らない (Job の側が silent breakaway を持っていると考えられるが未確認)。そのため、この環境のシェルからの実験は、`claude.exe` が `mye_mcp.exe` を起動する経路を再現していない。
- 本物の Claude Code を終了させて子が生き残るかは**試していない**。実行すると、この作業を動かしている `claude.exe` ごと終わるため。

### 予測と違った点

- 「Claude Code が子を kill-on-close の Job に入れているか」は予測もできず、実験でも決められなかった。Job に入っていること自体 (`in_job=1`) は観測できた。

### 結論 (設計へ反映したこと)

- Job の扱いは親 (MCP を起動したクライアント) 次第で、サービス側が制御できない。**サービスの起動は「`CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS` (Rust では `CommandExt::creation_flags`、窓を出さないなら `CREATE_NO_WINDOW`) で起動 → `ERROR_ACCESS_DENIED` で失敗したら、`CREATE_BREAKAWAY_FROM_JOB` を外して起動し直す」** にする。後者では親の Job に入るので、親と一緒に終わる場合がある。
- `DETACHED_PROCESS` / `CREATE_NO_WINDOW` は Job の所属を変えない (道連れになる行は変わらなかった)。Job から離れるのは `CREATE_BREAKAWAY_FROM_JOB` だけ。
- 親と一緒に終わった場合の縮退は、設計どおり「次の起動時に `service_down` の欠落として記録する」。要求 §3.4 の範囲内で、責務の分け方は変えない。
- Editor 経由の起動は、既存の `StartChildProcess` (`CREATE_NO_WINDOW` だけ、Job なし、`bInheritHandles = TRUE` でログファイルを渡す) を使う。Editor が Job に入っていなければ、Editor の終了後もサービスは生きる (「Job なし」の行)。Editor が Job に入っているか (Visual Studio のデバッグ起動、ランチャー経由など) は未確認。

### 未確認の残り

- 本物の Claude Code (と他の MCP クライアント) が、MCP サーバを入れる Job の性質。サービスを MCP から起動する実装ができたあと (sub-05 の `track_verify`、sub-07 の MCP の道具) に、実際に起動してクライアントを終了させて確かめる。
- Editor が Job に入って起動される場合 (Visual Studio のデバッグなど)。sub-09 (M104j)。
- 起動したサービスは `bInheritHandles = TRUE` の `StartChildProcess` 経由だとログファイルのハンドルを持ち続ける (サービスの寿命の間)。Editor がそのログを削除・読み取りしたいときに共有違反になりうる。sub-09 で、`StartChildProcess` をそのまま使うか、サービス用の起動口を足すかを決める。
