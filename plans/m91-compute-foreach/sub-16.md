# sub-16: レビュー 1 の指摘を直す (State と Fx の採番を分ける、resim 中の引数の誤りを失敗にする、表示と文言)

- 依存: sub-09 (M91a〜j と追補がすべてコミット済みであること)
- 状態: OK (commit 913067d)
- 往復: 0
- 新設: 2026-10-12 (review-1 の指摘 1〜4。指摘 1 は planner が仕様を直してから coder が直す)

## やること

### 1. (review-1 #1、major) State と Fx のスロット空間を分ける — 仕様の変更 (spec D29)
- 今の方式 (State と Fx が 1 つの空間を共有し、スナップショットには Fx の「居た印と世代」だけを入れる) では、次の 2 つの経路で State の採番が連続実行とずれる。どちらも desync になる。
  - (a) 同じ Fx を持たないストアへ復元する経路。クライアントの参加 (`ClientSimRunner::ApplySnapshot`) や、ロールバックの間に Fx を作り直した場合。
  - (b) 復元した後に、スクリプトが Fx を作り直す経路。デモの ComputeDemoSparks は「Read が失敗したら作り直す」ので、作り直しの Create が、連続実行では起きないスロットの消費になる。
- reviewer の案 (空いた Fx のスロットを予約として残す / Fx の形もスナップショットに入れる) は (a) は直すが (b) は直らない。そのため採らない。
- **直し方 (planner 裁定)**: 1 つのストアの中で、State と Fx を**別々のスロット表と採番**にする。
  - State の採番 (最小の空きスロットと世代) は、State の作成と解放だけで決まる。Fx の作成・解放・復元の影響を一切受けない。
  - ハンドルには種別の印を入れる。今の世代欄の上位 2bit (レーンの印) に加えて 1bit を使う。世代のビット数が 1 つ減るので、巡回の上限を合わせる。
  - **スナップショットとハッシュは State の表だけ**にする。Fx は入れず、復元の前後で Fx の表には触らない。Fx は表示用なので、手元の Fx のハンドルは復元後もそのまま有効。撮影後に作られた Fx を捨てる処理も無くす。
  - スロットの上限 (1 ストア 1024) は、種別ごとに 1024 とする。上限の値を変えるなら実装メモに書く。
  - C# 用のストアも同じ構造にする (スナップショットには入らないが、採番の独立性は同じ)。
- **SimSnapshot の版**: CMP 節の形が変わるので `kSimSnapshotVersion` を **49 → 50** に上げる。
  - M91 の v49 はまだ外に出ていないが、「形を変えたら版を上げる」の慣例に従う。手元に残っている v49 の .rep と取り違えないため。
  - 版を固定値で見ているテスト (AcousticAudioSelfTest など) も直す。
- **ABI は変えない** (ハンドルは不透明な uint64 のまま。v29 = 174 のまま)。
- selftest (reviewer の probe を常設にする):
  - (i) State@0・Fx@1 で撮影する → 空のストアへ復元する → Create した値が、連続実行で Create した値と一致する。
  - (ii) 復元した後に Fx を Create し、続けて State を Create しても、連続実行と一致する。
  - (iii) 復元の前後で、手元の Fx のハンドルがそのまま有効である。
  - (iv) State と Fx を交互に作成・解放しても、State のハンドル列が Fx の有無に依存しない。
  - (v) 既存の「空のストアへ復元しても State と採番は戻る」の試験に、復元後の Create の確認を足す。

### 2. (review-1 #2、major) resim 中の引数の誤りを失敗にする
- `ComputeService::ForEach` で、引数の解決 (`Resolve`) に失敗したら、resim かどうかに関係なく**失敗 (0)** を返す。Skipped の判定より前で `!resolved` を返す。
- selftest: resim 中に、無効なハンドル・解放済みのハンドル・stride 違いで呼ぶと、元の実行と同じく Failed になること。resim 中に正しい引数で Fx を呼ぶと Skipped、State を呼ぶと実行されること (既存の試験を残す)。

### 3. (review-1 #3、minor) インスペクターの補足文を折り返す
- `ComputeInspectorSection.cpp:129` 付近の補足文を、同じ関数の末尾の注記と同じく折り返して出す。
- スクリーンショットを撮り直し、パスを実装メモに書く。撮るときは CWD を一時ディレクトリにする。

### 4. (review-1 #4、minor) 検査器の KL016 の文言
- 「他の要素の読み書きは禁止」のように、読むことも含む文言にする。日英の両方を直す。検査器の文言が LocalizationTable にあるならそこを直す。golden に文言が入っていれば更新する。

### 5. 文書
- ADR-030 の D11 と、採番が決定的であることの記述を、分けた方式に直す。reviewer の指摘 1 と、却下した 2 案とその理由を書く。
- engine_spec の snapshot の版を 50 にする。

## やらないこと (このサブでは)
- ABI の変更

## 受け入れ条件 (このサブ)
1. 上の 1 の selftest (i)〜(v) と、2 の selftest が PASS する (Debug / Release)
2. `bin\x64\Debug\Server.exe --selftest` が PASS する (クライアント参加の経路を含む net の試験)
3. `tools\replay_verify.bat` の全ジョブが PASS する (compute / computeab / snapshot の往復を含む)
4. `tools\check_rules.ps1` が PASS する
5. reviewer の probe と同じ手順の結果 (continuous と restored のハンドル) が一致する。実装メモに書く
6. インスペクターのスクリーンショットで、補足文が折り返されて読める

## 検証コマンド
- Debug / Release のビルド、`Editor.exe --selftest` (両構成)、`Server.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`、一時プローブと `--screenshot`

## コミット
- 件名候補: `M91k: レビュー 1 の指摘を直す (State と Fx の採番を分ける、resim 中の引数の誤りを失敗にする。snapshot v50)`

## 実装メモ (coder が追記)

SELF_EVAL: sub-16 (round 1)
- #1: ComputeBufferStore を State 表と Fx 表の 2 本に分けた (slots_[2]、BufferKind で引く)。世代欄は [lane 2 bit | 種別 1 bit | 作成回数 29 bit]。スナップショットとハッシュは State の表だけ (印 2 = Fx は読み込みで拒否)。ApplySnapshot は State の表だけ差し替え、droppedFx を廃止 (Fx は復元の前後で触らない。ComputeService::ApplyGameSnapshot の Forget も無し)。Slots/MutableSlots/HandleOf は BufferKind 引数付きに変更。maxSlots は種別ごとに 1024 (値は変えず、意味を種別ごとにした)。SimSnapshot v50。AcousticAudioSelfTest の版固定値も 50 に。ABI 不変。
- #2: ComputeService::ForEach で `!resolved` を DecideExecution の Skipped より前に出した (先に DecideExecution の Failed 理由だけは保つ)。
- #3: ComputeInspectorSection の補足文を TextDisabled 色の TextWrapped にした。スクリーンショット = scratchpad の inspector_cpp_k2.png (補足文が 2 行に折り返され、REGISTER_SCRIPT まで読める。Debug、Editor --scene compute_probe.scene.json --autoplay --select Probe --width 1800 --height 1800、CWD は scratchpad)。
- #4: KernelLint の KL016 文言を「他の要素の読み書きは禁止」にした (日本語の 1 か所。英語版・golden・selftest に旧文言は無かった)。
- 文書: ADR-030 の D11 (分けた方式・経緯・却下した 2 案)、engine_spec の版と記述、replay_verify.bat のコメントを直した。
- selftest: ComputeStateSelfTest に (i)(ii)(iii)(iv)(v) と種別印の確認、ComputeServiceSelfTest に種別ごとの上限と resim × 無効ハンドル / 世代違い / 解放済み / Fx 混じり / 本数違い / stride 違い = Failed を追加。
- 検証: Debug ビルド (Editor / Server / Runtime) OK。Debug Editor --selftest exit 0 (FAIL 0)。Debug Server --selftest exit 0。tools\replay_verify.bat exit 0 (21 ジョブ PASS、240 s)。Release Editor --selftest exit 0 (FAIL 0)。check_rules 0 error。probe 相当 (State@0・Fx@1 で撮影 → 空のストアへ復元 → Create) は ComputeStateSelfTest (i) で continuous == restored を確認 (refNext と一致)。
- 申し送り: 最初の Debug --selftest で ServerNetSelfTest の V1 (LoadPersist / LoadGame、tick 270 の .rep) が 1 回だけ FAIL したが、同じコードの再実行 (Debug 2 回、Release 1 回) では出なかった。タイミング依存の不安定試験の疑い (今回の変更とは無関係に見えるが未切り分け)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。review-1 の #1〜#4 を実装した。selftest の (i)〜(v) と resim の試験を常設にした。replay_verify の 21 ジョブ、selftest (Debug / Release / Server) が PASS した。
