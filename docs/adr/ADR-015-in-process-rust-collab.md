# ADR-015: エディタ内 Git 連携は in-process の Rust cdylib (MyeCollab.dll)

- 状態: 採用 (2026-09-02、M66a)
- 出所: 元計画 `plans/quiet-merging-harbor.md` の決定台帳 1〜3 を本 ADR へ移す。
  **決定 1 はユーザー判断で反転**した (別プロセス exe → in-process cdylib)。
  仕様の正本は `plans/m66-git-collab/spec.md` §4.0。

## 決定

- Git 連携の実体は **Rust の cdylib `MyeCollab.dll`** で、`Editor.exe` が
  `LoadLibraryW(<exeDir>\MyeCollab.dll)` + `GetProcAddress` で **in-process にロードする**。
  crate は `tools\collab\` (= `src\` の外なので `gen_project_files.ps1` の対象外、
  `MyEngine.sln` にも入らない。`build_managed.bat` と同型の位置づけ)。
- **C ABI は 6 関数だけ**。これ以外の export を増やさない:

  ```c
  uint32_t mye_collab_proto_version(void);
  void*    mye_collab_create(const char* rootUtf8);
  void     mye_collab_request(void* h, const char* json);   // 非同期
  char*    mye_collab_poll(void* h);                        // 応答 or 通知 1 件、無ければ NULL
  void     mye_collab_free(char* s);
  void     mye_collab_destroy(void* h);
  ```

- 会話は **UTF-8 の JSON 文字列 1 本**。要求 `{id,op,args}` / 応答 `{id,ok,result|error}` /
  通知 `{event,...}`。op ごとの ABI スロットは作らない。
  版は `mye_collab_proto_version()` と C++ の `kCollabProtoVersion` を
  `check_rules.ps1` の規則 9 が機械照合する (規則 11 = `Interop.cs` と同じ問題を繰り返さない)。
- スレッドは **DLL 側に worker 1 本**。C++ 側にはスレッドを作らず、毎フレーム
  `poll` を NULL まで drain するだけ。
- Rust の panic は全 export と worker で `catch_unwind` して
  `{event:"service_error", code:"internal_panic"}` に変換し、ハンドルを dead 化する
  (以後の要求は `error.code = service_dead`)。`panic = "unwind"` を profile に明示する。
- 出力は `cargo build --release` 1 回で、`bin\x64\Debug\` と `bin\x64\Release\` の
  **両方へ同じ物**を置く (`tools\build_collab.bat`)。Rust 側に構成の区別を持ち込まない。
- 同じ crate の `[[bin]] mye_collab_cli` (NDJSON を stdin → stdout) を作り、
  `tools\collab_verify.bat` が**エディタ抜きで**回帰を取る。

## 理由

### なぜ Rust なのか (元計画の前提を引き継ぐ)

git 連携の本体は「子プロセスの起動・パイプ・ファイル監視・パーサ」であって、
エンジンのどの層とも共有する型が無い。C++ 側に置くと `Editor` 層が肥大し、
テストのたびに D3D を持つ実行ファイルをビルドすることになる。
crate として切り出せば `cargo test` が数秒で回り、CLI 経由の回帰も取れる。

### なぜ別プロセスをやめたのか (決定 1 の反転)

元計画は「Rust の panic をエディタのクラッシュ経路に混ぜない」「メモリ隔離が構造になる」
を理由に**別プロセス exe + stdio IPC** としていた。ユーザー判断 (2026-09-02) で
「コンパイルして DLL としてのせる」= in-process に反転した。

反転で**得たもの**:

- spawn / Job Object (`KILL_ON_JOB_CLOSE`) / stdio パイプ / 孤児プロセスの後始末が丸ごと不要。
  エディタが落ちてもサービスが残る問題自体が消える。
- 要求 1 往復のコストがプロセス境界ぶん下がる (毎フレーム poll する設計と相性がよい)。

反転で**失ったもの (承知のうえ)**:

- **メモリ隔離**。Rust 側の未定義動作はエディタごとプロセスを落とす。
  緩和は 3 点だけ: (1) safe Rust に限定し `unsafe` は FFI 境界の 6 関数だけに置く、
  (2) 全 export と worker を `catch_unwind` で囲む、(3) `panic = "abort"` にしない。
  ★`abort` にすると (2) が意味を失う — profile の `panic` は**触ってはいけない設定**。
- クラッシュバンドル (.rep 付き) に Rust 由来のクラッシュが混ざる可能性。
  混ざったときの見分けは、スタックに `mye_collab` が出るかどうか。

### なぜ JSON 1 本で、op ごとのスロットを作らないのか

`src\Shared\EngineAPI.h` ⇄ `Interop.cs` は **位置ベースのミラーで実行時の版検証が無い**。
順序・件数がズレると全スロットが静かに別関数を指す — 規則 11 の静的検査が唯一の防波堤に
なっている。同じ構造をもう 1 組増やすと、維持すべき機械照合が 2 組になる。
JSON なら増える op は文字列 1 個で、C ABI は 6 関数のまま凍る。

### なぜ CLI を同じ crate に持つのか

Source Control は **`--project` 起動でしか動かない**のに、`replay_verify` /
`shot_verify` / CI は全部裸起動 = Collab は常に OFF。エディタ経由でしか検証できない
状態にすると、回帰は「人が窓を開いて目で見る」しか残らない。CLI + `collab_fixture.ps1` の
一時リポジトリなら、期待 NDJSON との突き合わせが CI で回る。

## 帰結

- **前提が 1 つ増える**: rustup (stable) が開発者の環境に必要。無い環境では
  `MyeCollab.dll` が作られず、Source Control が「利用不可 (NoService)」になるだけで
  エディタの他機能は無傷 (`MyeScripting.dll` 不在時と同じ縮退)。
  CI は `dtolnay/rust-toolchain@stable` で入れ、`MYE_COLLAB_REQUIRED=1` を立てて
  **DLL 不在を SKIP ではなく失敗**にする (作り忘れを素通りさせない)。
- `MyeCollab.dll` はエディタ実行中に上書きできない (ホットリロードしない。
  `MyeScripting.dll` と同じ)。
- 決定論への影響は無い: Editor 層だけが触り、`src\Engine` / `src\Runtime` /
  `src\GameLogic` / `src\Shared` からの include を**規則 12** が禁じる。
  `replay_verify.bat` は全サブで無変更緑であること。
- `--package` の出力に `MyeCollab.dll` / `MyeCollabCli.exe` / `.git` を入れない
  (Runtime に Collab は無い)。ci.yml の package contents ステップが否定検査する。
