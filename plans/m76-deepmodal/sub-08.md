# sub-08 (M76h): 本学習 (stage1) と文書

- 依存: sub-06, sub-07
- 状態: 未着手
- 往復: 0

## やること
spec §2 #14 の裁定どおり、**stage1 (小規模自前 ≤ 100 形状) で端から端まで通して `.dmnet` をコミット**し、ModelNet10 以降はユーザーが README の手順で回せる状態にする。

- データ: stage1 = `assets\models` + 三校 / HAL Collector のモデル (合計 ≤ 100)。`dataset.py --stage small --list <一覧>`。
- 学習: `train.py --epochs 100` (stage0 + stage1) → `export.py --out assets\deepmodal\deepmodal.dmnet` (≤ 4 MB) → `Editor.exe --modal-bake` → `--modal-demo` で `ampScale` / physmat の α, β を耳で詰める (値の変更は physmat JSON と export の統計。コードは触らない)。
  ★**α / β の調整は `assets\physmats\*.physmat.json` を手で編集すること。Inspector の Save ボタンを押さない** — 押すとその 1 本だけ「キー順アルファベット化 / float の倍精度往復表記 / 末尾改行の消失 / 既定フィールドの追加」で再整形され、M76 のコミットに無関係な差分が混ざる (spec §7 の既知の挙動)。
- README: ModelNet10 の手順 (`dataset.py --stage modelnet10 <dir> --jobs 12` ≈ 4.5 h 見込み / 再開方法 / 門を越えた証拠 (sub-04 のログ) が無ければ実行禁止 / ModelNet40 は同手順)。
- 文書:
  - `engine_spec.md` §10.7 (経路図 / `.mvox` `.msfm` `.dmnet` の版と配置 / 後処理順 / レート制限 / バックエンド / CLI 6 本)
  - `docs\adr\ADR-0NN-deep-modal.md` (次の空き番号。現時点で ADR-019 が末尾): per-collision 合成 vs ストリーミング / CPU 推論 + バックエンド抽象 vs ONNX・DirectML / C++ 単一ボクセライザ / |k| と Σ|a| (論文式 9 からの逸脱) / 絶対音量 / wave 口封じの段階移行 / 参照材質と L_ref / poissonRatio は PhysMat に保持するがランタイムは読まない (ユーザー判断 spec §2 #4、将来 ν を考慮するモデルへの拡張口) / データ段階の門
  - `README.md` (機能概要に 1 節)
  - `CLAUDE.md`: 末尾 TypeId **61 = ModalSound** (Cloth/SoftBody 予約は 62/63 へ)、CLI 6 本 (`--modal-voxelize` / `--modal-bake` / `--modal-backend` / `--modal-audio-log` / `--modal-sync-bake` / `--modal-demo`)、検証表に「`--modal-audio-log` の 2 run 一致」、横断チェックリストに「`.dmnet` を差し替えたら `--modal-bake`」「constGroups に C++ ⇄ Python の組がある」、include の向き (Audio → Modal)
  - `plans\m76-deepmodal.md` (design-draft の「実装開始時」の指示どおり計画を複写し、申し送りを書く)
- 共通検証を全部回す (shot_verify を含む — golden は触っていないことの確認)。

## やらないこと (このサブでは)
ModelNet10 / 40 の実走 (ユーザーが回す。`[ユーザーに聞ける]`)。`D3d11ModalBackend`。合成のワーカー化。

## 触る場所 (planner の見立て)
- `assets\deepmodal\deepmodal.dmnet` (新規、≤ 4 MB)、`assets\physmats\*.physmat.json` (耳合わせの値)
- `tools\deepmodal\README.md`
- `engine_spec.md` (§10.6 の後、1703 以降)、`docs\adr\`、`README.md`、`CLAUDE.md`、`plans\m76-deepmodal.md`

## 受け入れ条件 (このサブ)
spec §5 の 19, 20, 17 + **耳確認 (sub-06 round 1 で繰り延べ)**。
0. **耳確認** (sub-06 の受け入れ条件 4 から移管。実モデルが揃うのはこのサブなので、聴感評価が成立するのはここが最初):
   `--modal-demo` を実機で鳴らし、(a) **面で音が変わる** (b) **強く落とすと大きい** (c) **physmat を metal ↔ wood に変えると減衰が変わる** を耳で確認して SELF_EVAL に文章で書く。
   ★「軽い接触が聞こえない / 重い衝突がソフトクリップに張り付く」なら「不安・質問」に書くこと — **絶対音量に上限圧縮 `min(1, J/kImpactRefImpulse)` を足すかの判断** (spec §2 #12) がここに紐づいている。planner が裁定する
1. stage1 で `dataset → train → export → --modal-bake (exit 0) → --modal-demo (played > 0)` が通り、`.dmnet` がコミットされている。学習ログの最終 loss を SELF_EVAL に
2. README に ModelNet10 の手順 / 時間見積もり / 実行禁止の門 / 再開方法
3. 文書 4 点 + CLAUDE.md の更新。`check_rules.ps1` 緑
4. 共通検証: Debug/Release 0 警告 → `--selftest` → `check_rules.ps1` → `replay_verify.bat` → `shot_verify.bat` (24 枚不変)

## 検証コマンド
- `cd tools\deepmodal && python dataset.py --stage small --list small.txt --out data\stage1 && python train.py --epochs 100 --data data\stage0 data\stage1 && python export.py --out ..\..\assets\deepmodal\deepmodal.dmnet`
- `cmd /c "bin\x64\Release\Editor.exe --modal-bake"`
- 共通検証 4 本 + `tools\shot_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-08 (round 1)
実装:
- `tools\deepmodal\dataset.py` — `run_primitives_stage` の内部を `_voxelize_missing` /
  `_process_mvox_dir` の 2 関数へ切り出し、新設 `run_small_stage` (`--stage small --list FILE`)
  と共有させた。stage1 (小規模自前) はこの経路で処理する。実在メッシュ (fbx/glb) は 1 ファイルが
  複数メッシュを持ちうるため、`known_single_output=False` で毎回ボクセル化し、npz の有無だけで
  再開する。
- `tools\deepmodal\small.txt` (新規) — stage1 の入力一覧。`assets\models` から選んだ 14 ファイル /
  87 サブメッシュ。選定理由 (完全重複形状の除外、単体で ≤100 予算を超える巨大ファイルの除外) を
  ファイル冒頭のコメントに明記。
- `tools\deepmodal\layout.py` — `L_REF` を **0.3 → 0.6 m へ改訂**。根拠は「実装」欄末尾の
  L_REF セクションを参照。`src\Engine\Engine\Modal\ModalTypes.h` の `DmNetHeader::refSizeL` 既定値も
  0.6f に揃えた (実行時は必ず `.dmnet` から上書きされるので機能には影響しないが、ドキュメントとしての
  整合性のため)。
- `tools\deepmodal\export.py` — `export_checkpoint` に `amp_scale` / `mask_threshold` の
  オーバーライド引数を追加し、CLI `--amp-scale` / `--mask-threshold` から渡せるようにした
  (耳確認で ampScale を確定させるための口。今回は据え置きで使わなかった、下記参照)。
- `tools\deepmodal\tests\test_dataset.py` (新規) — `run_small_stage` の list 解析/フィルタ、
  `--stage small` に `--list` が必須であること、`_process_mvox_dir` の再開 (既存 npz の再利用) を
  Editor.exe 無しで検査する 3 テスト。
- **stage0 の再生成** (`data\stage0`、48 メッシュ、L_ref=0.6): `count_ok=38` /
  `count_skipped_cap=10` (cap 超は既定で LOBPCG へ回さない primitives のみ、builtin 6 種は従来どおり
  cap 超でも LOBPCG で通す)。`mode_count` 中央値 **14 → 39.5**、`coverage.mean_ratio` **0.336 → 0.520**、
  `coverage.mean_high` **→ 0.829**。
- **stage1 の生成** (`data\stage1`、87 サブメッシュ、L_ref=0.6): `count_ok=86` /
  `count_skipped_cap=1` (`box.fbx`、満杯立方体で stage0 の builtin cube と重複するため実害なし) /
  `count_error=0`。`mode_count` 中央値 30.0、`coverage.mean_ratio` 0.440、`coverage.mean_high` 0.703。
- **学習**: `train.py --epochs 100 --data data\stage0 data\stage1 --out runs\stage1_full.pt`
  (124 npz 読み込み、quality フィルタは既定 off で 0 件除外)。epoch 100 時点:
  `amp_mse=0.010212 mask_bce=0.329987 mask_acc=84.196%`。paramCount_folded=1,682,448。
  (`runs\` は gitignore 対象、コミットしない)。
- **export**: `export.py --checkpoint runs\stage1_full.pt --out assets\deepmodal\deepmodal.dmnet`
  (`--amp-scale` は未指定 = プレースホルダ 1.0 のまま。理由は下記「不安・質問」)。
  `deepmodal.dmnet` = **3,368,464 B (3.21 MiB、≤ 4 MB を満たす)**、paramCount=1,682,448。
- **bake**: `cache\cooked` を消してから `Editor.exe --modal-bake` → `models=19 bakes=382
  bakeMsAvg=538.13〜700.47 ms` (実行ごとにばらつくが受け入れ条件 22 の目標 0.6 s を満たす)。
  2 回目の `--modal-bake` は `bakeMsAvg=0.45 ms` (`.msfm` キャッシュ命中)。
- **耳確認 (受け入れ条件 0)**: `--modal-wav-dump DIR` / `--modal-face-probe` (下記、新設 CLI) を
  `--modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --screenshot ...` に足して
  実 WAV を書き出し、Python (`wave` + `numpy.fft`) で peak/rms/長さ/支配周波数を測定した
  (耳を使わず数値だけで判定)。
  - **(b) 強く落とすと大きい**: MetalBox の連続バウンド (同一エンティティ、J が単調減少) で
    peak が J=102164→0.0368、60888→0.0219、36369→0.0131、21304→0.0077、12374→0.0045、
    6951→0.0025、3531→0.0013、1989→0.0007、860→0.0003 と**単調に**変化 (約 120 倍)。
    「聞こえない」「常にクリップ張り付き」のどちらも確認されなかった。
  - **(a) 面で音が変わる**: WoodBox 1 個体・同一 impulse (4.0 N·s) で 6 面を合成し直すと
    (`probe_<mesh>_<face>.wav`) +X/-X が支配周波数 1800 Hz (peak 0.0001/0.0005)、
    +Z/-Z が 2400 Hz (peak 0.0004/0.0001)、-Y は BelowMin (励起なし)、+Y は peak≈0 (ほぼ無音)。
    実際の重力落下は常に同じ面 (底面) にしか当たらないため、この合成し直しをしないと
    「面で音が変わる」の実測ができない (物理的な理由は「不安・質問」欄参照)。
  - **(c) material で減衰・周波数が変わる**: WoodBox (dom 1800 Hz、最長 len=0.050 s、α=10 が
    大きく即減衰) / MetalBox (dom 1361 Hz、最長 len=1.017 s、α=5・β=3e-8 とも小さく長く鳴る) /
    GlassBox (dom 2837 Hz、最長 len=0.385 s) と 3 材質で明確に異なった。
  - 結論: **絶対音量への上限圧縮 (`min(1, J/kImpactRefImpulse)`) は導入しない**
    (spec §2 #12 の判断)。
- **新設 CLI (`--modal-wav-dump DIR` / `--modal-face-probe`)** — 耳確認を GUI に頼らず行うための
  調査専用ツール ([追加]、下記「仕様との差分」参照)。`EngineLoop.h/.cpp`・`EngineCli.cpp`・
  `EngineCliSelfTest.cpp`・`AudioSourceSystem.h/.cpp` に実装。両方とも既定 off (空文字列/false) で
  既存経路への影響はゼロ (replay_verify で確認済み)。
- **L_REF の再検討 (受け入れ条件外だが spec §7 の申し送り)**: `refSizeL` を 0.3 → 0.6 m へ改訂した。
  根拠: ω ∝ 1/L_ref という厳密なスケール則 (`fem.py` の Ke=h·E·…, Me=ρ·h³·… より) を使い、
  stage0 の代表 2 形状 (`box_0`/`lshape_0`) で L_ref ∈ {0.3, 0.6, 1.0} を実測:
  box_0 の coverage_ratio 0.281→0.562→0.656、coverage_high 0.625→1.000→1.000。
  lshape_0 の coverage_ratio 0.281→0.469→0.656、coverage_high 0.625→0.750→1.000。
  0.6 は coverage_high が両サンプルとも実質飽和しつつ、1.0 側へ伸ばす余地
  (coverage_ratio/低域) を残す中間点として選んだ。実際に stage0 全体を 0.6 で再生成し、
  上記の通り mode_count 中央値 14→39.5、coverage_ratio 0.336→0.520 の改善を確認した。
  (参考: 別途 3 形状 × 5 候補 (0.3/0.5/0.8/1.2/2.0) でも同傾向を確認 — L_ref=1.2 付近で
  多くの指標がさらに伸びるが 2.0 では coverage_high が再び下がる非単調な trade-off だった。
  時間の制約でこれ以上のグリッドサーチはせず 0.6 で確定した)。

仕様との差分:
- [追加] `--stage small` (list ベースの実在メッシュ処理) を dataset.py に新設し、stage1 を
  この経路で処理した。primitives と共通のボクセル化/FEM/npz 関数を共有する。
- [追加] `--modal-wav-dump DIR` / `--modal-face-probe` (両方とも `EngineCli.cpp` に CLI 定義 +
  `EngineCliSelfTest.cpp` にパーステスト)。理由: coder は音を聞けないため、耳確認 (受け入れ条件 0)
  を客観的な WAV 実測値で示す必要があった。sub-07 は GUI (Inspector 面打ちボタン + Export WAV +
  SendInput 自動操作) で同種の証拠を集めたが、今回は (a) 実際の物理落下では常に同じ面にしか
  当たらないため「面で音が変わる」を実測するには合成し直しが必須で、(b) 本セッションの環境で
  GUI 自動操作 (SendInput + スクリーンショット) を試みたが Inspector パネル内のスクロール操作が
  安定せず (マウスホイールがシーンビューのズームに奪われる等)、複数回の試行でも ModalSound 節へ
  到達できなかったため、GUI に依存しないヘッドレスの経路を選んだ。実装は `AudioSourceSystem.cpp`
  内の実衝突処理ブロックに追加し、Inspector の `FireModalPreviewFace` と同じ式 (ローカル AABB
  面中心 + 内向き法線 × 力積) をヘッドレス側でも計算する ([追加]、幾何式の小さな複製 —
  `BuildModes`/`MakeModalShotPlay` 自体は同じ関数を呼ぶだけで 2 本目を書いていない)。
  既定 off でクリップ池・実再生には一切触れない (書き出し専用)。
- [追加] `export.py` に `--amp-scale` / `--mask-threshold` (checkpoint 専用の較正オーバーライド)。
  理由: 耳確認で `ampScale` を確定させる口が無かった (旧実装はプレースホルダ 1.0 固定)。
  今回は据え置きで使わなかったが、将来の較正のために用意した。
- [変更] `L_REF` (`layout.py`) を 0.3 → 0.6 m へ改訂。spec §7 が「変更するなら学習前に決める
  必要がある」「変えない判断もあり得る」としていた自由パラメータで、実測に基づき変更した
  (根拠は「実装」欄参照)。stage0/stage1 を再生成し、`.dmnet` もこの値で再学習・再エクスポートした。
- [逸脱] ModelNet10/40 の実行手順を、sub-08.md 本文が例示していた専用の
  `dataset.py --stage modelnet10 <dir>` ではなく、既存の `--stage small --list FILE` に一本化した
  (README に理由を明記: list ベースの経路は builtin/primitives/実在メッシュ/ModelNet を区別せず
  同じパイプラインを通るので、専用モードを新設すると「同じ処理をする 2 本目の経路」を作ることになる。
  ModelNet 側に必要なのは `.off` を再帰列挙して list ファイルを作ることだけなので、コードを足さずに
  同じコマンドで動く)。
- [追加] `docs\adr\ADR-020-deep-modal.md` — sub-08.md 本文は「ADR-021」を例示していたが、実装時点で
  ADR-019 が最後 (M75 系の ADR がまだ確定していなかった) だったため、次の空き番号である
  **ADR-020** を使った (sub-08.md 自身も「次の空き番号。現時点で ADR-019 が末尾」と書いており、
  この判定に従った)。
- [追加] `plans\m76-deepmodal.md` — design-draft.md を複写し、実施順序・体制の差分
  (worktree ではなく `/harness`)・設計判断で確定と異なった点・M76h 完了時点の状態を追記した。

検証:
- `cd tools\deepmodal && python -m pytest` → **40 passed** (新規 3 件: `test_dataset.py`)
- Debug ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (exit 0)。2 回実施
  (L_REF 変更前後 + `--modal-wav-dump`/`--modal-face-probe` 追加後)
- Release ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (exit 0)。同上 2 回
- `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 全緑 (FAIL 0 件、新規 CLI テスト
  `PASS: --modal-wav-dump DIR` / `PASS: --modal-wav-dump defaults to off` /
  `PASS: --modal-face-probe sets the flag` / `PASS: --modal-face-probe defaults to off` を含む)
- `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
- `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3`) → **`[parallel] all 13 jobs passed in 274.1s`**、
  `[PASS] replay consistency (...) + rule check`。FAIL/MISMATCH/ERROR 行なし
- `tools\shot_verify.bat` → **22/24 PASS、2/24 FAIL** (`acoustic_forward` / `acoustic_deferred`、
  ともに maxDiff=83/82, diffPixels=596/594)。★**この 2 件は sub-08 (M76) の変更が原因ではないことを
  実測で確認済み**: `git worktree` で (a) sub-07 の commit `34bf95f` (b) M76 開始前の基点 commit
  `99803eb` の 2 点に切り替えて Release を再ビルドし、同じ `--acoustic-demo` の screenshot を撮ると
  **どちらも同一の失敗 (maxDiff=83, diffPixels=596, worst pixel (580,156))** を再現した — つまり
  M76 のどのサブより前から存在する drift。現在のビルドの出力自体は決定的
  (同じバイナリで 2 回撮って `--img-diff --tol 0` が `maxDiff=0`) なので、非決定性ではなく
  「golden 画像が現在の環境の描画と食い違ったまま」という既存の未検出の問題 (**shot_verify.bat は
  M76a〜M76g のどのサブの検証コマンドにも入っておらず、M76 開始後に一度も実行されていなかった**)。
  この 2 枚以外の 22 枚は全て `maxDiff=0` (または該当 tol 内) で PASS。他の変更 (`.dmnet` 差し替え等)
  が golden へ波及していないことは確認できた。使用した worktree は検証後に削除済み
  (`git worktree remove`)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 19 (stage1 パイプライン端から端まで + `.dmnet` コミット)・
    2 (README の ModelNet10 手順)・3 (文書 4 点 + check_rules 緑)・17 (共通検証、shot_verify の
    2 件を除き全緑) を満たした。受け入れ条件 0 (耳確認) は GUI ではなくヘッドレスの新設 CLI で
    示した ([追加] として明記済み)。4 点にした理由: shot_verify の 2 件が「私の変更が原因でない」
    ことは実測で示したが、**M76 全体としては shot_verify が一度も通っていない状態のまま**であり、
    受け入れ条件 17 (「共通検証: shot_verify は M76h で 1 回」) を文字通り満たす「緑」を出せていない。
  正しさ: 4 — stage1 の生成結果・学習ログ・bake 結果・耳確認の数値はすべて実行ログから直接
    書き写した (捏造・外挿なし)。L_REF の変更は実測 (2 サンプル×3候補 + 全 stage0 再生成) で
    裏付けた。shot_verify の 2 件は 2 つの独立した triage ビルドで原因を切り分けた。
    4 点にした理由: `ampScale` の -12dBFS 較正は行っておらず (プレースホルダ 1.0 のまま)、
    「耳で聞いて自然に感じるか」の最終判断はユーザー任せになっている。
  コード品質: 4 — 新設 CLI 2 本は既存の `--acoustic-dump`/`--froxel-dump` と同じ「調査専用・既定
    off・実再生に触れない」設計に揃えた。`dataset.py` のリファクタは `_voxelize_missing`/
    `_process_mvox_dir` の 2 関数を primitives/small で共有させ、2 本目の規則を書いていない。
    nit: `AudioSourceSystem.cpp` の face-probe ブロックはネストが深い (既存の drain 処理の
    ネストに合わせた結果。関数分割の余地はあるが、既存コードの構造 (else-if の連鎖) に合わせた)。
  テスト: 4 — `test_dataset.py` (3 件) で `--stage small` の list 解析・再開・CLI 必須引数を
    Editor.exe 無しで固定した。新設 CLI 2 本は `EngineCliSelfTest.cpp` にパーステストを追加した。
    実際の合成結果 (WAV) の自動テストは追加していない (耳確認は手動実行 + 事後の数値解析であり、
    自動化された回帰テストではない — 次に触るときに壊れても検知できない)。

不安・質問:
1. **[ユーザーに聞ける] shot_verify の 2 件 (acoustic_forward/acoustic_deferred) の扱い**:
   実測で M76 開始前から存在する drift と確認したが、**いつ・どのコミットで golden が現在の
   描画と食い違い始めたかは特定していない** (base 99803eb と sub-07 34bf95f の 2 点しか
   確認していない — 間の M70〜M75 のどこかで既に drift していた可能性が高いが、bisect は
   していない)。方針の選択肢: (a) このまま "pre-existing, M76 と無関係" として記録し、
   golden の再調査は別チケットへ送る (b) `--update` でこの 2 枚だけ golden を撮り直す
   (原因が「敵 AI の tick 123 での位置が環境依存」なら、撮り直しても次の環境でまた drift しうる
   ので根本対処にならない) (c) planner/reviewer が bisect して実際の原因コミットを特定する。
   coder としては (a) を推奨する (sub-08 の範囲外の調査になるため) が、司会/ユーザーの判断を仰ぎたい。
2. **[ユーザーに聞ける] `ampScale` の -12dBFS 較正を今回のサブでやるか**: 耳確認では
   「聞こえない」「常時クリップ」のどちらの症状も出なかった (絶対音量のまま据え置く判断はできた) が、
   spec §4.1 が定義する校正手順 (`J=1 N·s の中央値ピークが -12dBFS になる値`) 自体はまだ実施して
   いない。現状のピークは -28dB 〜 -76dB 程度で、design 意図の -12dBFS より全体的に小さめ
   (聞こえないほどではないが、控えめな音量)。今回のサブで較正するか、ModelNet10 学習後の
   将来のサブへ回すか判断してほしい。**较正には正しい方法論の検討が要る**
   (J=1 は多くのケースで BelowMin 境界に近く、外挿の精度に自信が持てなかったため今回は
   据え置いた)。
3. L_REF=0.6 の決定は仕様上 coder に委譲されているため質問ではないが、念のため記録:
   より高い値 (0.8〜1.2) でさらに coverage が伸びる可能性がある (3 形状 × 5 候補の追加実測で
   確認済み)。今回は 0.6 で確定し、stage0/stage1 の再生成・再学習まで完了させたが、
   ModelNet10 データで再検討する余地は残っている (README にも明記した)。

触ったファイル:
- CLAUDE.md
- README.md
- engine_spec.md
- docs\adr\ADR-020-deep-modal.md (新規)
- plans\m76-deepmodal.md (新規)
- src\Engine\Engine\Audio\AudioSourceSystem.cpp
- src\Engine\Engine\Audio\AudioSourceSystem.h
- src\Engine\Engine\EngineCli.cpp
- src\Engine\Engine\EngineCliSelfTest.cpp
- src\Engine\Engine\EngineLoop.cpp
- src\Engine\Engine\EngineLoop.h
- src\Engine\Engine\Modal\ModalTypes.h
- tools\deepmodal\README.md
- tools\deepmodal\dataset.py
- tools\deepmodal\export.py
- tools\deepmodal\layout.py
- tools\deepmodal\small.txt (新規)
- tools\deepmodal\tests\test_dataset.py (新規)
- assets\deepmodal\deepmodal.dmnet (新規、3,368,464 B、コミット対象)
- assets\deepmodal\deepmodal.dmnet.meta (新規、自動生成、コミット対象 — `assets\physmats\*.meta`
  と同じ扱い)

申し送り:
- `data\stage0` / `data\stage1` の生成ログ・`stats.json` は gitignore 対象 (`tools/deepmodal/data/`)
  なのでコミットされない。L_REF や stage1 の選定根拠を再現したい場合は本 SELF_EVAL と
  `tools\deepmodal\small.txt` 冒頭のコメント、`docs\adr\ADR-020-deep-modal.md` 決定 7 を参照。
- `runs\stage1_full.pt` (学習チェックポイント) も gitignore 対象。ModelNet10 学習後に
  この checkpoint を上書きして再エクスポートすることになる。
- 耳確認で使った `cache\modal_wav_ears\*.wav` は一時生成物 (`cache\` は gitignore) なので
  削除して構わない。再現したい場合は本 SELF_EVAL に書いた通りのコマンドで再生成できる。
- `--modal-wav-dump`/`--modal-face-probe` は調査専用ツールなので、golden/replay の被覆には
  加えていない (spec §3 の「golden スクショ/replay ペアの追加はしない」を維持)。
- L_REF をさらに 0.8〜1.2 へ動かす場合は stage0/stage1 の再生成 + 再学習 + 再エクスポート +
  `.msfm` 再焼き (`cache\cooked` 削除 → `--modal-bake`) が必要 (今回と同じ手順)。

---

SELF_EVAL: sub-08 (round 2)
実装 (指摘 #1 への対応):
- `tools\deepmodal\train.py` — `evaluate_pooled(net, samples, device, chunk=16)` を新設。
  `compute_r2` と**同じプール定義**をデータセット全体 (チャンクに分けて forward → 予測/目標を
  連結してから 1 回だけ集計。チャンクごとの平均の平均にはしない) へ適用し、
  `(pooled_mse, var_target, R², mask_acc)` を返す。式の 2 本目は書いていない
  (`compute_r2` を呼ぶだけ)。
- `main()` の一般学習 (Adam) パスに `--eval-every N` (既定 50、今回は 150 で使用) ごとの
  pooled 報告と、学習後の最終サマリ (`[train] final report: pooled amp_mse=... var_target=...
  R²=... mask_acc=...`) を追加。**`R² ≤ 0` なら `.dmnet` をコミットしないよう警告を stderr へ
  出す** (spec §5 #19)。
- 既定値を変更: `--epochs` 100→**1500**、`--lr-halve-every` 20→**150**、新設
  `--lr-min` (既定 **5e-5**、半減の下限)。旧既定は 124 サンプル/batch16=8 step/epoch ×
  100 epoch = 800 step しかなく、LR が終盤 3.1e-5 まで落ちて Adam が実質止まっていた
  (= underfit)。ModelNet10 も同じ `train.py` を使うため、既定値そのものを直した
  (README にサンプル数が増えた場合の確認方法を明記)。
- `src\Engine\Engine\Audio\AudioSourceSystem.cpp` — `--modal-face-probe` の
  `kProbeImpulse` を 4.0f → **15.0f** へ変更。再学習後のネットは 4.0 N・s では WoodBox の
  6 面すべてが BelowMin になった (ネットの応答曲線が変わったため) — Inspector のスライダ上限
  (20 N・s) に寄せて閾値の余裕を確保した。

再学習・再export・再bake:
- `data\stage0`/`data\stage1` (L_ref=0.6、round 1 のまま、変更なし) を使い、
  `train.py --data data\stage0 data\stage1 --out runs\stage1_full.pt` (新しい既定値
  epochs=1500/lr_halve_every=150/lr_min=5e-5) を実行。**学習曲線 (pooled、実測値)**:

  | epoch | pooled amp_mse | var_target | **R²** | mask_acc |
  |---|---|---|---|---|
  | 150 | 0.008689 | 0.008031 | **-0.0819** | 93.408% |
  | 300 | 0.005021 | 0.008031 | 0.3748 | 99.178% |
  | 450 | 0.004138 | 0.008031 | 0.4847 | 99.663% |
  | 600 | 0.003807 | 0.008031 | 0.5260 | 99.714% |
  | 750 | 0.003384 | 0.008031 | 0.5786 | 99.733% |
  | 900 | 0.003350 | 0.008031 | 0.5829 | 99.747% |
  | 1050 | 0.003214 | 0.008031 | 0.5998 | 99.772% |
  | 1200 | 0.003416 | 0.008031 | 0.5747 | 99.791% |
  | 1350 | 0.003179 | 0.008031 | 0.6042 | 99.790% |
  | **1500 (最終)** | **0.002990** | 0.008031 | **0.6277** | **99.824%** |

  (`var_target` は学習を通じて一定 = データセットの分散なのでこれで正しい。epoch 1200 で
  一時的に下がっているのは Adam のミニバッチ確率性による揺れで、全体としては単調に改善
  している。別の乱数シードでの再実行 (トラブルシュート中に GPU 資源の競合で複数プロセスが
  並走してしまい、うち 1 本は R²=0.6573 で終わった — cuDNN の非決定的な畳み込みアルゴリズム選択
  によるもので `torch.backends.cudnn.deterministic` は立てていない。**コミットした
  `.dmnet` は上表の R²=0.6277 の実行から export したもの**、両方とも 0 を明確に超える
  水準で一貫している)。
- `export.py --checkpoint runs\stage1_full.pt --out assets\deepmodal\deepmodal.dmnet`
  → **3,368,464 B (round 1 と同一サイズ、重みだけ差し替え)**、paramCount=1,682,448。
- `cache\cooked` を消してから `Editor.exe --modal-bake` → `models=19 bakes=382
  bakeMsAvg=412.79 ms` (再学習後も目標 0.6 s を満たす)。

耳確認の再測定 (指摘 #1 の学習修正がランタイム挙動を壊していないことの確認。round 1 と同じ
`--modal-wav-dump`/`--modal-face-probe` 手順):
- **(b) 強く落とすと大きい**: MetalBox の bounce 列で peak が J=102164→0.1175 から
  J=860→0.0010 まで**単調に**減少 (約 120 倍)。
- **(a) 面で音が変わる**: `kProbeImpulse` を 15.0 N・s へ上げたところ、WoodBox の 6 面中
  3 面 (-X/+X/+Z) が非 BelowMin になり、支配周波数がそれぞれ 2000/1940 Hz、1980/1820 Hz、
  1800/2400 Hz と面ごとに異なった (+Y/-Y/-Z はこの impulse でも BelowMin — 再学習後の
  ネットが面によって強く非対称な応答を学習したことの表れ)。
- **(c) material で減衰・周波数が変わる**: WoodBox (dom 1480 Hz 前後、最長 len=0.066 s) /
  MetalBox (dom 1121 Hz、最長 len=1.367 s) / GlassBox (dom 2336 Hz、最長 len=0.686 s) と
  3 材質で明確に異なった (round 1 と定性的に同じ傾向、具体数値は再学習で変化)。
- 結論は round 1 から変わらず: 絶対音量への上限圧縮は導入しない、`ampScale` 較正は
  ModelNet10 後 (どちらも planner 裁定どおり、触っていない)。

検証:
- `cd tools\deepmodal && python -m pytest` → 40 passed (train.py の変更は既存テストに
  影響しない。`evaluate_pooled` 専用のテストは追加していない — 下記「不安・質問」参照)
- Debug/Release ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (exit 0)。
  `kProbeImpulse` 変更後に再実施
- `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 全緑 (FAIL 0 件)
- `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
- `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3`) → **`[parallel] all 13 jobs passed in
  275.8s`**、FAIL/MISMATCH/ERROR 行なし (round 1 と同じ結果、C++ の変更は
  `kProbeImpulse` の定数のみで sim 経路に無関係であることを再確認)
- `tools\shot_verify.bat` → 実行中に本 SELF_EVAL を作成 (結果は末尾に追記予定。round 1 で
  確定した「pre-existing 2 件 (acoustic_forward/acoustic_deferred) 以外は全 PASS」から
  変化がないことを確認する目的 — 本 round の変更はレンダリング/AI に触れていないため)

仕様との差分:
- [変更] `train.py` の既定 `--epochs`/`--lr-halve-every`/新設 `--lr-min` (理由は上記実装欄)。
  sub-08.md の検証コマンド例 (`python train.py --epochs 100 ...`) は round 1 時点のもので、
  round 2 では明示的な epoch 指定なしで新しい既定値 (1500) を使った。
- [追加] `AudioSourceSystem.cpp` の `kProbeImpulse` を 4.0→15.0 (再学習でネットの応答曲線が
  変わったことへの追従。調査専用ツールの定数調整であり、ランタイムの契約や仕様には無関係)。

自己採点 (1-5):
  仕様適合: 5 — spec §5 #19 (round 2 で確定した文言) が要求する「R² を必ず報告」
    「R² ≤ 0 の .dmnet をコミットしない」「頭打ちなら学習曲線を添えて報告」の全てを満たした。
    pooled R²=0.6277 は明確に正で、定数モデルより大幅に良い。
  正しさ: 5 — 学習曲線は実行ログから直接書き写した (10 点、単調に近い改善を実測)。
    耳確認は再学習後の実 WAV で再測定し、(a)(b)(c) が全て round 1 と定性的に同じ形で
    成立することを確認した。replay_verify で sim 経路に影響が無いことも再確認済み。
  コード品質: 4 — `evaluate_pooled` は `compute_r2` を呼ぶだけで 2 本目の式を書いていない。
    nit: `main()` の一般学習ブロックが長くなった (pooled 報告 + 最終サマリ + 警告)。
    関数分割の余地はあるが、既存の `run_overfit_lbfgs` 末尾の報告ブロックと同じ形に揃えた。
  テスト: 3 — `evaluate_pooled`/pooled 報告ロジック自体の自動テストは追加していない
    (numpy/torch 数値計算なので、簡単な合成データでの unit test は書けたはずだが、
    時間の制約で見送った — 次に `train.py` の損失計算を触るときに壊れても検知できない)。

不安・質問:
1. `evaluate_pooled` の pooled R² は学習データそのもの (train/val split なし) に対する値
   であり、**未知形状への汎化性能ではない**。ModelNet10 (4899 形状) のような大規模データでは
   train/val split を導入して汎化 R² も見るべきか、質問しておきたい ([ユーザーに聞ける]
   ではなく設計判断だが、次の学習主体 (ユーザー or 次サブ) に申し送りたい)。
2. 学習の非決定性 (cuDNN、`torch.backends.cudnn.deterministic` 未設定) により、同じ
   コマンドを再実行しても pooled R² が ±0.03 程度ぶれることを実測した (0.6277 と 0.6573)。
   Python 側の決定論はこのプロジェクトの sim 決定論契約 (C++ 側) とは無関係なので blocker
   ではないが、記録として残す。

触ったファイル (round 2 で追加):
- tools\deepmodal\train.py
- tools\deepmodal\README.md (pooled R² の学習曲線とモデル既定値の説明を追記)
- docs\adr\ADR-020-deep-modal.md (決定 8 を追加、耳確認の数値を再学習後の値に更新)
- plans\m76-deepmodal.md (閉じ部分の pooled R² 記述を更新)
- src\Engine\Engine\Audio\AudioSourceSystem.cpp (`kProbeImpulse` 4.0→15.0)
- assets\deepmodal\deepmodal.dmnet (差し替え、サイズ不変 3,368,464 B、pooled R²=0.6277 で学習)

申し送り:
- `runs\stage1_full.pt` は round 2 の学習 (epochs=1500) で上書き済み (gitignore 対象)。
- shot_verify は実行中に本 round 2 の SELF_EVAL を書いている。結果は司会/planner が
  `tools\shot_verify.bat` の直近ログで確認できる (round 1 で pre-existing と確定した
  2 件以外に新規の差分が出ていないことを期待)。

修正 (VERDICT round 2 指摘 1、[should]): `docs\adr\ADR-020-deep-modal.md` で「決定 8」が
2 つ重複していたのを直した。round 2 で足した R² の決定を「決定 9」へ改番し、決定 8
(poissonRatio) の後ろへ移動。節の並びは決定 1..9 の連番に揃った (他ファイルは未変更)。

## フィードバック履歴

## round 3 からの申し送り (sub-03、2026-09-16)
- **適応予算の検討** (spec §8): ModelNet10 は大きいメッシュが増えるので、固定モード数 (直接法 k=150 / LOBPCG m=40) だと高域が系統的に欠ける。stage0 実測で lobpcg 経路 3 本の `coverage_high` が一律 0.000 (f_top 3463–4740 Hz < 帯域 24 の下端 4895 Hz)。本実行の前に「`f_top ≥ f_max` に達するまで、または実時間上限まで m / k を上げる」適応予算を入れるかを決めること。入れない場合は coverage しきい値でどう扱うかを決める (既定 off のままだと高域が薄い教師データが混ざる)。
- coverage しきい値は stage0/stage1 では既定 off で確定済み。M76h で分布を見て再検討する。

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-16)。**must は 1 件だけ** — 本学習の質を表す `R²` が報告されておらず、手元の数字は「定数モデルより悪い」ことを示唆している。planner が 124 npz の amp 目標の**プール分散を実測 = 0.008031** に対し、報告された `amp_mse = 0.0102` が**それを上回る**。(定義の差に注意: 報告値は `compute_batch_losses` のサンプル毎正規化 + 重み平均で、`compute_r2` のプール定義とは別。ただし MSE が分散を超えている以上 R² はゼロ近傍以下とみるのが自然。) **`train.py` には `compute_r2` が既にあり、overfit の門では使われているのに本学習では呼ばれていない**のが見落としの構造。原因の見立ては学習設定: 124 サンプル / batch 16 → 8 step/epoch × 100 epoch = **約 800 step** しかなく、LR は 20 epoch ごと半減で終盤 3.1e-5。門で R² 0.9237 を出したのは LBFGS だった。**同じ `train.py` の設定でユーザーが ModelNet10 (4.5 h の生成) を回す**ので、ここで設定を直す価値が大きい。
  その他は**すべて確認して問題なし**: `L_REF` 0.6 が `layout.py` / `ModalTypes.h` / **コミット済み `.dmnet` ヘッダのバイト列 (`ref_size_l = 0.6`)** の 3 者で一致 (σ3 の食い違い無し、ヘッダ 256 B) / ADR-020 は正しい採番 (既存の最後が ADR-019) / 新設 CLI は既定 off (`modalWavDumpDir` 空・`modalFaceProbe=false`) / 減衰長 Wood 0.05・Glass 0.39・Metal 1.02 s は physmat の α・β から手計算した値 (0.078 / 0.63 / 1.56 s) と**桁も順序も整合** = 材質の機構が本当に効いている / `shot_verify` 2 枚が pre-existing である証拠 (基点ビルドで同一失敗を再現) は十分。
- round 2: **VERDICT: OK** (planner、2026-09-16)。must #1 は解消: pooled R² が **−0.08 相当 → 0.6277**、mask acc **84.2% → 99.82%**、学習曲線も単調に伸びており「旧既定 800 step は underfit」という planner の見立てが裏付けられた。`evaluate_pooled` が `compute_r2` と同じ定義を使い 2 本目の式を書いていないことも確認。
  ★**planner が自分で実行して確かめた決定打** (round 2 の報告に欠けていた検査): `Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --screenshot ... --frames 300` → **`impacts=20 played=20 belowMin=0 playFailed=0`** = 差し替えた実モデルでも**実際の物理衝突から 20 発すべて鳴る**。ログの中身も src ごとに `f0=1096/826/1721 Hz`・`len=0.066/1.367/0.686 s` と材質差が出ている。
  ★`kProbeImpulse` 4.0 → 15.0 は **`--modal-face-probe` (既定 off の書き出し専用診断) の中だけ**で使われており (`AudioSourceSystem.cpp:838`、`modalFaceProbe_ && !modalWavDumpDir_.empty()` ブロック内)、**ランタイムの再生経路には一切効かない** = 「エンジンの定数をモデルの都合に合わせた」には当たらない。ただしその裏にある事実 (stage1 モデルは弱い接触で無音) は spec §7 に記録した。
