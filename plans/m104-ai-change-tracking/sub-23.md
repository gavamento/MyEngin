# sub-23: Editor の保存をサービスへ知らせる (M104k2)

- 依存: sub-10
- 状態: 未着手
- 往復: 0
- 順序: sub-10 → **sub-23** → sub-11 (番号は末尾。sub-09 の VERDICT で、sub-10 から分けた)
- 書き込みの深さ: **着手可** (2026-11-15、sub-10 の VERDICT で planner が見直した。保存の経路の洗い出しそのものがこのサブの仕事なので、経路の一覧は coder が作る)
- 質問の運用: 推奨で進め、Notion に後回し。

## 目的

Editor の利用者が Editor で保存したファイルが、AI Changes 窓で「不明な変更」や「AI の推定」に並ばないようにする。attr §3 の表の 1 行目 (`editor_saved` があれば `editor_user` / `verified`)。結論の関数には sub-06 で入っているが、根拠を作る op と、Editor から知らせる経路がまだ無い。

## やること

1. サービス側: op `editor_saved` (`role = editor` だけ。引数 `path`、`hash`)。ipc §4.4。`Store::record_event` を通し、派生の表 (根拠) を events から作り直せる形にする (spec §2 D26)。
   - 照合: 観測した Revision の After のハッシュが、知らせの `hash` と同じなら `editor_saved` の根拠を付ける。知らせと観測のどちらが先に届いても成り立つ (知らせを一定時間保持する。保持の上限と時間は定数。sub-06 の「後から届く根拠」と同じ作り)。
   - 知らせを受けたら対象パスをすぐ flush してよい (sub-06 の管理された操作と同じ `observe_managed`)。
   - **AI の操作の窓と重なったとき**: Editor の保存と、AI の `declared_edit_match` が同じ Revision に成り立つことは、内容が同じでない限り無い。attr §3 の表は `editor_saved` が先頭なので Editor が勝つ。この優先でよいかを試験で確かめ、attr に 1 行書く。
2. Editor 側: 保存の経路を洗う (`WriteFileReplacing` の呼び手と、それ以外の書き込み)。**追跡が接続中 (`State() == Ready`) のときだけ**、書いた内容のハッシュ (`Sha256Hex`) とパスを `editor_saved` で送る。押さえる経路: シーンの保存、プレハブ / アクターの保存と Apply、アセットの `.meta` の書き込み、アセットの移動・リネーム、設定ファイル (`.mye` の下は追跡の対象外なので不要)。洗った結果の一覧 (送る経路 / 送らない経路と理由) を文書に残す。
   - 送る場所は 1 か所にまとめる (保存のたびに各所から `TrackClient` を呼ばない)。Engine 層の `WriteFileReplacing` に手を入れない。Editor 層で、書いた後に知らせる小さい関数を呼ぶ形にするか、既存の「保存した」通知の集まる場所があればそこに足す。
   - ハッシュの計算がフレームを止めないこと (大きいファイルは保存そのものが既に同じ量の仕事をしている。二重に読み直さない。書いたバイト列から計算する)。
   - 送れなかった (切断中、時間切れ) 場合は何もしない (その保存が `unknown` / `inferred` に落ちるだけ)。保存を失敗させない・遅らせない。
3. 非対話起動 (`allowChangeTracking` が偽) と追跡が無効のときは、ハッシュも計算しない。

4. sub-10 の結果 (2026-11-15):
   - AI Changes 窓の整形は `src\Editor\ChangeTracking\ChangeFormat.h/.cpp` にある。行為者が `editor_user` のときの文面 (「Editor の利用者」) と、根拠 `editor_saved` の文面が既にあるかを確かめ、無ければ足す (英日)。
   - 「セッションなし (不明 / 推定)」の項目を、読み込み済みの行からではなく、`list` の `attribution` フィルタ (unknown / inferred) で取るように直す (sub-10 の nit)。`editor_user` / `restore` の変更は、既定では AI の変更の節にも不明の節にも出さず、「Editor / 復元による変更」の節 (切り替えで表示) に分ける。
   - サービスの op を足すときは `protocol.rs` の `IMPLEMENTED_OPS` と `role_allows`、ipc の表を更新する。`schema\v1.sql` は変えない (根拠は `attribution_evidence` に入る)。
   - スクショは sub-10 と同じ流儀 (一時のプローブ。コミットしない)。撮るのは「Editor の利用者」の変更が出ている一覧の 1 枚。

## やらないこと (このサブでは)

- Engine 層の変更。保存の経路そのものの変更。`restore_expect` (sub-15)。

## 受け入れ条件 (このサブ)

1. `editor_saved` の知らせと一致する観測が `editor_user` / `verified` になる。知らせが先でも後でも同じ。ハッシュが違えばならない。`mcp` / `hook` の役割では `forbidden` → `cargo test` + `tools\track_verify.bat`。
2. DB を壊して作り直しても、`editor_saved` の根拠が戻る → `cargo test`。
3. Editor の押さえた経路それぞれで、保存すると `editor_saved` が 1 回送られ、ハッシュが書いた内容と一致する (偽の I/O で、送られた行を確かめる) → `changetracking` スイート。
4. 追跡が無効・非対話起動・切断中は、送られず、ハッシュの計算も呼ばれない → スイート。
5. 保存の経路の一覧が文書にある (送る / 送らないと理由) → 文書。
6. 実サービスでの確認 (手動 1 回、一時プロジェクト): Editor でシーンを保存すると、AI Changes 窓でその変更が「Editor の利用者」として出る。確かめられなければ「未確認」と書く。
7. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1`、`cargo test`、`tools\track_verify.bat` が通る。`src\Engine` などの下位の層に差分が増えない。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴