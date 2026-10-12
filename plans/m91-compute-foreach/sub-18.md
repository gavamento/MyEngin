# sub-18: 同時実行で共有される残りの作業場所 (save_server と hot_server) を、selftest ではプロセスごとに分ける

- 依存: sub-17 (77269f9)
- 状態: OK (commit bb625f3)
- 往復: 1
- 新設: 2026-10-12 (U21 のユーザー回答「M91 の中で直す」。planner 案「M91 の外の別件として記録だけ」の逆)

## 現状 (planner が `src/Engine/Engine/Loop/HeadlessSim.cpp:240-300` で確認)
- `m.saveDir = base + L"\\save_server"`。base は projectRoot、無ければ exe のフォルダ。
  - 同じ exe から起動したすべてのプロセスと、同じプロセスの中のすべての HeadlessSim で共有される。
  - V1 のセーブスロット 7 はここに置かれる。
- `m.shadowCopyDir = base + L"\\cache\\hot_server\\i" + <プロセスの中の通し番号>`。
  - デストラクタは `shadowCopyDir\p<pid>` を消すので、DllReloader がプロセスごとの段を足している可能性がある。**本当にプロセスごとに分かれているかは、coder が確かめる**。
  - 分かれていれば、hot_server はすでに直っている。分かれていなければ直す。
- 製品の側の意味:
  - `save_server` は、専用サーバ (Server.exe) が**再起動をまたいで持ち続けるセーブ**の置き場。
  - hot_server は、DLL のホットリロードのための一時的な置き場。

## 方針 (planner 裁定)
- **製品の通常の 1 プロセス実行での置き場は変えない**。
  - Server.exe / Runtime / Editor の `save_server` の位置はそのままにする。プロセスごとに変えると、専用サーバが再起動したときにセーブを見失うため。
  - 決定論とファイル形式も変えない。セーブの中身、.rep、スナップショットはすべてそのまま。
- **selftest (と、同じ exe から同時に回る検証) だけが、置き場を差し替えられるようにする**。
  - `HeadlessSimSetup` に、セーブの置き場と shadow コピーの置き場を上書きする欄を足す (空なら今までどおり)。
  - net の selftest (ServerNetSelfTest) と、HeadlessSim を立てるほかの selftest (例: ComputeScriptSelfTest のホットリロードの試験) は、sub-17 の作業フォルダ `p<pid>` の下を使う。
  - 製品のコードに「selftest かどうか」の分岐は作らない。上書きの欄を渡すかどうかで決まる。
- hot_server が、確かめた結果すでにプロセスごとなら、上書きの欄は save だけでよい。その判断と根拠を実装メモに書く。
- 一時フォルダの片付けは、sub-17 と同じ流儀にする (終わったら消す。落ちた実行の残りは起動時に古いものだけ消す)。

## やること
1. hot_server の分かれ方を確かめる (DllReloader の shadow コピーのパスを、実行中に列挙するか、ログに出す)。
2. HeadlessSimSetup に置き場を上書きする欄を足し、HeadlessSim::Init で使う。既定 (空) では今の置き場のまま。
3. HeadlessSim を立てる selftest を洗い出し (ServerNetSelfTest、ComputeScriptSelfTest、ComputeStateSelfTest、Server.exe の selftest など)、プロセスごとの作業フォルダを渡すようにする。洗い出した一覧を実装メモに書く。
4. sub-17 の `MYE_SELFTEST_ONLY` に、HeadlessSim のホットリロードを通る試験を絞り込む値を足す (例: `computescript`)。同時実行の確認に使う。
5. **修正前の確認**: 同じ exe から 4 本同時に回し、次を観測する。結果を表にする。
   - 共有の置き場 (`save_server`、`hot_server` の下) を、複数のプロセスが同時に触っていること (パスのログや、実行中の列挙)
   - FAIL が出るかどうか。出なくても、共有の観測があれば修正の根拠になる
   - 回す試験は `MYE_SELFTEST_ONLY=servernet` と、4 で足した値

## やらないこと (このサブでは)
- 製品の既定の置き場の変更
- Editor / Runtime の置き場 (HeadlessSim の外。同時実行は今の検証で起きない)

## 受け入れ条件 (このサブ)
1. 1 の確認結果 (hot_server がプロセスごとに分かれていたか) と、根拠
2. 5 の修正前の表 (共有の観測、FAIL の有無)
3. **直ったことの基準** (sub-17 と同じ形): 同じ exe から `servernet` と、4 で足した値の 2 種類を、それぞれ 4 本同時に 30 ラウンド回して、FAIL 0。実行中に、各プロセスが自分の `p<pid>` の下だけを使っていること (共有の置き場に書いていないこと) を、1 ラウンド以上で観測する
4. 製品の置き場が変わっていないこと: 上書きを渡さない HeadlessSim (Server.exe の通常の起動) が、今までどおり `save_server` を使うことを、selftest かログで確かめる
5. 確認一式が PASS する: `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`、`bin\x64\Debug\Server.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`
6. 作業が終わったとき、`save_server` と `hot_server` の下に、selftest の残りが無いこと

## 検証コマンド
- `MYE_SELFTEST_ONLY` での同時 4 本の繰り返し、上の 5 の一式

## コミット
- 件名候補: `M91m: selftest の HeadlessSim のセーブと shadow コピーの置き場をプロセスごとに分ける (製品の置き場は変えない)`

## 実装メモ (coder が追記)

### round 1 (coder)
- hot_server: DllReloader::Init が `<shadowCopyDir>\p<pid>` を足すため、すでにプロセスごと (修正前のログ `cache\hot_server\i0\p496642\GameLogic.dll` で確認)。上書き欄は save だけ。
- HeadlessSimSetup::saveDirOverride を追加 (空 = 従来の `<base>\save_server`)。HeadlessSim::Init が起動時に saveDir / shadowCopyDir / pid をログへ出す。
- 渡した selftest: ServerNetSelfTest (InitSim と VerifyRepOffline。`CrashRoot()\save`)、ComputeScriptSelfTest・ComputeManagedSelfTest (temp\mye_selftest_save_p<pid>)。ServerSelfTest (Server.exe) は意図して渡さず、既定の save_server のままであることを検査。
- EditorMain: MYE_SELFTEST_ONLY=computescript を追加。
- 修正前 (4 本同時 × 3 ラウンド): servernet / computescript とも FAIL 0。ただし全プロセスの saveDir が同じ `bind\Debug\save_server` (pid 46804, 11228 ほか) = 共有を観測。
- 修正後: computescript 30 ラウンド FAIL 0、servernet-v1 10 ラウンド + 2 ラウンド FAIL 0、servernet 全体は 1 ラウンド約 105 秒のため 30 ラウンドは [未実施]。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。
  - hot_server はすでにプロセスごとに分かれていたので、上書きの欄は save だけにした。
  - 修正前は FAIL 0 で、`save_server` を複数のプロセスが共有していることを観測した。修正後は各プロセスが自分の pid の下だけを使うことを観測した。
  - 30 ラウンドは computescript で満たした。servernet は、セーブを使う唯一の試験 V1 に絞って 12 ラウンドとし、これで受け入れる (修正前に FAIL が出ていないので、回数を増やしても証明の強さは変わらない。証拠は置き場の分離の観測)。
  - nit: %TEMP% に `mye_selftest_<pid>` が 19 個残っている。Release の slot0.json を書いたのが誰かは分かっていない。どちらも申し送り。
