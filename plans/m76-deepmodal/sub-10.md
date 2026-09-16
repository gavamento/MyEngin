# sub-10 (M76i): 衝撃力 → 音量カーブの較正と圧縮、および engine 文書への明記

- 依存: sub-08 (実モデル `.dmnet`)
- 実行順: M76 のレビュー指摘への対応。sub-08 の後
- 状態: 未着手
- 往復: 0

★design-draft が「M76i」として例示した `D3d11ModalBackend` は **spec §3 で範囲外と確定済み**なので、この番号を使う。

## なぜこのサブがあるか
reviewer round 1 の指摘 1 (major) + **ユーザー判断「今校正する」** (sub-08 round 1 の「将来に回す」を撤回)。
撤回の理由は、判断時の情報 (peak −28〜−76 dB、無音や常時歪みは無い) が**実態より楽観的だった**こと。
reviewer の実測が示した実態:
- 振幅は力積に**線形** (peak/J = Metal 1.150e-6、Wood 2.41e-6、Glass 2.79e-6 [1/(N·s)])
- int16 の 1 LSB (3.05e-5) に届く下限が **J ≈ 11〜27 N·s**
- **1 kg を 0.5 m 落とす (J ≈ 3 N·s) と PCM は全サンプル 0** = 現実的な物体で**鳴らない**
- `--modal-demo` が鳴るのは箱が **0.7〜7.9 トン**だから (デモが問題を隠していた)
- 焼いた **382 枚中 35 枚 (9.2%) は mask が全落ちで、どんな力積でも無音**

## planner の裁定 (この sub の設計。根拠つき)

### A. 線形のままでは成立しない → **圧縮カーブを入れる**
現状 `k_j = J_excess · nE_local_j` で振幅 ∝ J (線形)。
J≈3 を可聴にする線形スケールは、`--modal-demo` の J≈10000 を**約 1000 倍のクリップ**に叩き込む。
逆にデモが歪まない線形スケールでは J≈3 が無音になる。**線形では両立しない** = spec §2 #12 が予見した
「軽い接触が聞こえない / 重い衝突が張り付く」が同時に顕在化した状態。よって**圧縮は必須**。

- `Audio/ModalAudio` に衝撃力カーブを入れる:
  `C(J) = kModalRefImpulse · (J / kModalRefImpulse) ^ kModalImpulseExponent`
  で、`k_j = C(J_excess) · nE_local_j` とする (今は `C(J) = J`)。
- **`kModalRefImpulse = 6.0` は新設せず、既存の `acoustic::kImpactRefImpulse` (= 6.0、
  「倍率 1.0 に達する力積 (1.5kg を 1m 落下)」と文書化済み) を使う** — 基準を 2 つ作らない。
- `kModalImpulseExponent = 0.5` を**出発値**とする (p=1 が現行の線形なので、逃げ道も残る)。

### B. `ampScale` の目標を**再アンカーする**
spec §2 #12 の「**J=1 N·s** の中央値ピークが −12 dBFS」は**撤回**する。J=1 に意味のある根拠が無く、
線形と組み合わせると全域クリップを含意していた。新しい目標:

> **`J = kImpactRefImpulse` (6.0 N·s) のときの中央値ピークが −12 dBFS**

6.0 はエンジンが既に「本気の一撃」として文書化している値なので、アンカーとして妥当。

p=0.5・係数中央値 2.41e-6 から逆算した**出発値** `ampScale ≈ 1.74e4` での見込み:

| J [N·s] | 由来 | C(J) | peak (概算) |
|---|---|---|---|
| 0.35 | `kImpactMinImpulse` (発音の下限) | 1.45 | ≈ −24 dBFS |
| 3 | 1 kg を 0.5 m 落下 | 4.24 | ≈ −15 dBFS |
| 6 | `kImpactRefImpulse` (**目標点**) | 6.0 | **−12 dBFS** |
| 100 | 重い衝突 | 24.5 | ≈ 0 dBFS (ソフトクリップ入口) |
| 10000 | `--modal-demo` の現行の箱 | 245 | tanh で頭打ち (大きいが歪み切らない) |

★**これは出発値であって合格条件ではない**。実測して下の不変条件を満たす値に詰めること
(`export.py --amp-scale` が既にあるので**再学習は不要**)。

### C. **35 枚の常時無音は較正では直らない — 別問題として可視化する**
★planner がコードで確認: `ModalSynth.cpp` は **66 行で mask を落とし、74 行で `a <= 0` を continue し、
77 行で初めて `a *= hdr.ampScale`** を掛ける。つまり mask が全落ちなら `a = 0` のままで、
**`ampScale` を何倍にしても救えない**。原因は較正ではなく**モデルの汎化** (学習 124 形状) と
`maskThreshold` の側にある。
- このサブで**モデルは作り直さない** (ModelNet10 が本命)。
- 代わりに**見えるようにする**: `--modal-bake` の集計に「**特徴マップが全 cell・全帯域 mask off の
  メッシュ数**」を出す (例 `silent=35`)。9.2% が黙って無音なのが最大の問題であって、
  数が出ていれば「モデルの問題」と正しく診断できる。
- `ModalSound.maskThreshold` を下げれば一部は救える可能性があるので、**緩和ノブとして文書化**する。

### D. engine 文書への明記 (指摘 1 の本体)
reviewer の本当の指摘は「制約が `plans/` にしか無い」こと。**engine 側の文書に書く**:
- `engine_spec.md §10.7`: 音量則 (`C(J)` / `ampScale` / tanh)、較正のアンカー (J=6 で −12 dBFS)、
  **既知の限界** (全 mask off のメッシュが出ること + その数の出し方、`maskThreshold` の緩和)
- `docs\adr\ADR-020-deep-modal.md`: 圧縮カーブと較正の決定を**新しい決定**として追加。
  ★**決定 5 を訂正する** — 現状は `--modal-face-probe` の 3 枚を「面で音が変わる」の肯定的証拠に
  挙げているが、**その絶対レベルが 2 LSB (−84.3 dBFS) であることを書いていない**。較正後の値に
  差し替えるか、当時の絶対レベルを明記すること。
  ★**決定 8 の採番重複** (round 2 で「pooled R²」を決定 8 として足したが決定 8 は既に
  `poissonRatio` で使用済み。並びも 1,2,3,4,5,8,6,7,8) を**あわせて直す**
- `tools\deepmodal\README.md`: 較正手順 (`export.py --amp-scale`、測り方、`.msfm` の再焼きが
  **不要**なこと = `weightsHash` が変わらないので `modelHash` の照合を通る)

### E. 指摘 7 — デモが「形状で音が変わる」を見せられない
`BuildModalShowcaseScene` は 3 個とも `Cube()`。**`builtin://sphere` の物体を 1 個足す**
(生成順の**末尾**に置く = 既存の replay / golden に影響しない。このリポジトリの流儀)。
形状依存の機構自体は `.msfm` 382 枚の解析で確認済みなので、**デモで見せられるようにするだけ**。

### F. 指摘 8 — `.gitattributes` の `*.msfm binary`
spec §4.2 が要求していたのにどの sub にも割り当てられていなかった (planner の落ち穂)。
`.msfm` は `cache/` (gitignore) にしか生まれないので実害は無いが、**1 行足す**
(将来 prebaked cache を配布する等で状況が変わったときに、改行変換で静かに壊れるのを防ぐ)。

### G. 指摘 10 — spec §4.4 の初回焼き時間
「19 モデル ≒ 100 s」はモデル数とサブメッシュ数の取り違え。**「19 モデル = 382 サブメッシュ、
実測 wall 183.6 s (`bakeMsAvg` 453.65 ms)」**へ直す。結論 (ネットを縮めない) は変わらない。

### H. 指摘 2 (coder 宛) に対応する仕様側の規則
Inspector のプレビュー既定値 (4.0) と ヘッドレス `kProbeImpulse` (15.0) は**双子の定数が
2 箇所にあって片方だけ動いた**もの。仕様として:
- **プレビューの既定衝撃力は 1 つの共有定数にする** (2 箇所に置かない)
- **`Played` 以外の結果を UI に出す** (`BelowMin` / `NotReady` 等。黙って return しない)

## やらないこと (このサブでは)
- 再学習 / ModelNet10 (`ampScale` はヘッダの float 1 つなので**再学習は不要**)
- 35 枚の無音を**モデル側で**直すこと (ModelNet10 の仕事)
- `.msfm` の再焼き (`weightsHash` 不変なので不要。ただし**それを確かめるテストは要る**)
- `shot_verify` の 2 枚 (pre-existing、spec §8 で決着済み)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Audio\ModalAudio.h/.cpp` (`C(J)` の導入、定数)、`Acoustic\AcousticGrid.h` (`kImpactRefImpulse` を使う)
- `src\Engine\Engine\Audio\ModalAudioSelfTest.cpp` (カーブの単調性・下限・目標点)
- `src\Editor\ModalTools.cpp` (`--modal-bake` の `silent=` 集計)
- `src\Editor\Windows\InspectorWindow.cpp` (共有定数化 + 結果表示、指摘 2 と対)
- `src\Engine\Engine\DemoContent.cpp` (末尾に sphere を 1 個)
- `tools\deepmodal\export.py` / `README.md`、`assets\deepmodal\deepmodal.dmnet` (再 export)
- `engine_spec.md` / `docs\adr\ADR-020-deep-modal.md` / `.gitattributes`
- ソース追加時は `pwsh -File tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
spec §5 の **24, 25**。
1. **可聴性 (不変条件。数値は実測で詰める)**:
   (i) **J = 3 N·s (1 kg を 0.5 m 落下) の PCM が全サンプル 0 でない**かつ明確に可聴 (目安 peak ≥ −30 dBFS)
   (ii) **J に対して peak が単調増加** (Checkpoint J。`kImpactMinImpulse` 直上から `--modal-demo` の J まで)
   (iii) `J = kImpactRefImpulse (6.0)` の中央値ピークが **−12 dBFS ± 3 dB**
   (iv) `--modal-demo` が**歪み切らない** (tanh の範囲に収まり、`played` は sub-08 と同じ 20/20)
   ★**dBFS × J の表を SELF_EVAL に貼ること** (J = 0.35 / 1 / 3 / 6 / 30 / 100 / demo 実測値)。
   出発値 (p=0.5、ampScale≈1.74e4) で不変条件が満たせない場合は**値を変えてよい**が、
   変えた理由と実測を報告すること (planner が §8 で確定する)
2. **`--modal-bake` が全 mask off のメッシュ数を報告する** (`silent=` 等)。現行モデルで **35 前後**が出ること
3. **`.msfm` の再焼きが不要であることの確認**: `ampScale` だけ変えた `.dmnet` に差し替えても
   既存 `.msfm` が有効なまま (`modelHash == weightsHash` が通る) で、`--modal-bake` 2 回目が
   キャッシュヒットすること
4. **engine 文書に制約が載る**: `engine_spec.md §10.7` / `ADR-020` / `README.md` に (a) 音量則と較正アンカー
   (b) 全 mask off のメッシュが出ること + 数の出し方 + `maskThreshold` の緩和 (c) ADR-020 の**決定 5 の訂正**と
   **決定 8 の採番重複の修正**
5. **デモが形状差を見せる**: `--modal-demo` に sphere が 1 個増え、ログの mesh ハッシュが 2 種類以上になる
6. `.gitattributes` に `*.msfm binary`、spec §4.4 の焼き時間が実測値
7. **既存が不変**: `--selftest` 全緑 / `check_rules` 0 error / **`replay_verify` 全 13 ジョブ緑** /
   `shot_verify` が **round 1 と同じ 2 件のみ FAIL** (新規リグレッション無し) / Debug・Release 0 警告

## 検証コマンド
- Debug/Release ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"`
- `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` を 2 回 (2 回目がキャッシュヒット = 受け入れ条件 3)
- `cmd /c "bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --screenshot tmp.png --frames 300"`
- `--modal-wav-dump` / `--modal-face-probe` で J を振って dBFS 表を作る
- `pwsh -File tools\check_rules.ps1` / `tools\replay_verify.bat` / `tools\shot_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-10 (round 1)
実装:
  - `src/Engine/Engine/Audio/ModalAudio.h/.cpp` — `ModalImpulseCurve(J)` (`C(J) = kImpactRefImpulse·(J/kImpactRefImpulse)^kModalImpulseExponent`) を追加。`CollectModalImpacts` の `emitSide` で `k[]` を組む直前に適用 (`excessImpulse` は生の J のまま保持。ログ/`kImpactMinImpulse` の擦り判定は圧縮前)。`kModalPreviewDefaultImpulse=15.0f` を新設 (Inspector と `--modal-face-probe` の共有既定値、指摘 2 の是正)。`CollectModalImpacts` 冒頭に `ModalSoundComponent` を持つアーキタイプが 1 つも無ければ即 return するガードを追加 (指摘 6)。
  - `src/Engine/Engine/Audio/ModalAudioSelfTest.cpp` — 既存テスト (1) を圧縮後の値と比較するよう修正、新規 (9) で `ModalImpulseCurve` の `C(J<=0)=0` / `C(ref)=ref` / Checkpoint J 単調性 / 独立 double 参照との一致を検査。
  - `src/Engine/Engine/Audio/AudioSourceSystem.cpp` — `--modal-face-probe` の独自定数 `kProbeImpulse` を `kModalPreviewDefaultImpulse` へ一本化し `ModalImpulseCurve()` を適用。`[追加]` 診断用環境変数 `MYE_MODAL_PROBE_IMPULSE` で probe 力積を上書き可能にした (dBFS×J 較正表を実測するのに任意の J が要るため)。
  - `src/Editor/Windows/InspectorWindow.h/.cpp` — `ModalAudio.h` を include し `ModalPreviewState.impulse` の既定値を共有定数化。`everFired`/`lastResult` を追加し、`FireModalPreviewFace` が `Played` 以外も含めて必ず記録・`ModalImpulseCurve()` を適用するよう修正。`DrawModalSoundNotes` に直近結果の表示行 (`Insp_ModalResultHeading`) を追加、`ModalShotResultLabel()` を新設 (指摘 2 の是正、H の規則)。
  - `src/Engine/Core/LocalizationTable.inl` — `Insp_ModalResultHeading`/`Played`/`NotReady`/`NoModel`/`Cooldown`/`BelowMin`/`PoolFull`/`PlayFailed` の 8 エントリを追加 (en/ja)。
  - `src/Engine/Engine/Modal/ModalFeatureMap.h/.cpp` — `ModalFeatureMapAllMaskOff(map, maskThreshold)` を新設 (`BuildModes` 手順 3 と同じ閾値式で全 cell・全帯域 mask off を判定、C の可視化)。
  - `src/Editor/ModalTools.h/.cpp`、`src/Editor/EditorMain.cpp` — `RunModalBakeCli` に `backendName` 引数を追加し `--modal-backend` を実際に通す (指摘 5 の是正、旧実装は "cpu" 固定で黙って無視)。bake ループで `ModalFeatureMapAllMaskOff` を呼び `silent` タグ + `silent=N` 集計を出力に追加 (C)。ループ末尾で `lib.FlushDirtyTables()` を明示呼び出し。
  - `src/Engine/Engine/Modal/ModalSoundLibrary.h/.cpp` — `dirtyTables_` (dirty フラグ) を追加し `UpdateTableEntry` は保存せず dirty 登録のみに変更、`FlushDirtyTables()` を新設して `Shutdown()`/`Clear()` から呼ぶ (指摘 3 の是正、O(n²) I/O を解消)。
  - `src/Engine/Engine/Modal/ModalSelfTest.cpp` — fixture.dmnet の `bandCenterHz[32]` (Python が焼いた値) と C++ の `MelBandCenters()` を 1e-2Hz で照合するテストを追加 (指摘 4 の是正。既存の `ModalSynthSelfTest.cpp` の同語反復テストはそのまま残した — reviewer の期待は「足す」であって「置き換える」ではないため)。
  - `src/Engine/Engine/DemoContent.cpp` — `BuildModalShowcaseScene`/`RegisterModalShowcaseContent` の**末尾**に `builtin://sphere` を使う `TileSphere` (物理マテリアル `tile`) を追加 (E、指摘 7)。
  - `.gitattributes` — `*.msfm binary` を追加 (F)。
  - `assets/deepmodal/deepmodal.dmnet` — `export.py --checkpoint runs\stage1_full.pt --amp-scale 11478 --out ...` で再 export (`kModalImpulseExponent` は C++ 側の定数なのでこのファイルには現れない)。`weightsHash` は較正前と完全一致することを実測で確認 (下記検証)。
  - `engine_spec.md §10.7` — 圧縮カーブの説明を本文へ挿入し、新規 **§10.7.2** (較正アンカー・p の選定根拠・dBFS×J 表・常時無音の可視化) を追加。CLI 表の `--modal-bake`/`--modal-demo`/`--modal-face-probe` 行を更新。
  - `docs/adr/ADR-020-deep-modal.md` — 状態行を M76i まで延長。決定 5 に絶対レベル (-84.3dBFS) の訂正と金属 α/β の訂正 (指摘 9) を追記。新規 **決定 10** (圧縮カーブと較正、却下案込み)・**決定 11** (常時無音の可視化)・**決定 12** (球の追加と 20/20→21/21 の明記) を追加。
  - `tools/deepmodal/README.md` — `ampScale` の説明を新アンカーへ更新し、新規節「音量較正 (M76i)」(手順・再焼き不要の理由・確定値) を追加。ModelNet10 手順の `--amp-scale` 案内も更新。
  - `tools/deepmodal/export.py` — 3 箇所のコメント/ヘルプ文言を新アンカー (J=kImpactRefImpulse) へ更新 (ロジック変更なし、40 pytest 全緑を再確認)。

仕様との差分:
  - [逸脱] `kModalImpulseExponent` は出発値 0.5 ではなく **0.18** で確定した。理由: p=0.5 のまま `ampScale` を J=6→-12dBFS に較正すると、`C(kImpactRefImpulse)=kImpactRefImpulse` が p に依らない恒等式であることから、`--modal-demo` の実 J 域 (161〜102164、C(J) 比で 129 倍=+42dB) が **ほぼ全弾 (21/21) フルスケール ±0.8dBFS 以内**に張り付いた (実測)。tanh は範囲に収まるので壊れてはいないが、ADR-020 決定 5 が示した「強く落とすと大きい」を demo 上で再現できなくなる。p=0.18 に下げると (アンカーは不変)、demo の 3 本の実バウンド列 (Wood/Metal/Glass、いずれも同一エンティティ) が厳密に単調減衰し、フルスケールへ張り付くのは球の最初の 1 発だけになった。spec §5 #24 は「出発値で不変条件が満たせない場合は値を変えてよい」と明記しており、この裁定範囲内での変更。dBFS×J 表は下記。
  - [追加] 環境変数 `MYE_MODAL_PROBE_IMPULSE` (`AudioSourceSystem.cpp`)。`--modal-face-probe` は既定 J 固定 (旧 kProbeImpulse=15) だったため、dBFS×J 較正表の要求する J=0.35/1/3/6/30/100 を実際の推論結果に対して振る手段が無かった。`MYE_MODAL_THREADS` と同型の計測専用ノブとして追加 (CLI フラグにはしていない — engine_spec の CLI 表を恒久的に増やすほどの機能ではないため)。
  - [追加] `--modal-demo` の `played` が **20/20 → 21/21** になった。spec §5 #24 の文言「played は sub-08 と同じ 20/20」は E (sphere 追加) を反映する前の数で、球が着地して 1 impact を積むぶん総数が増える。不変条件の本質 (全打が Played、BelowMin/NotReady/PlayFailed が 0) は 21/21 で満たしている。ADR-020 決定 12 に明記した。**この差分は planner の確認を仰ぎたい** (下記不安・質問)。
  - [追加] `TileSphere` の物理マテリアルに新規で `tile` (`assets/physmats/tile.physmat.json`、既存資産) を使った。sub-10.md は形状 (sphere) の指定のみで材質までは指定していなかったため、4 種目の材質で形状差と材質差を同時に見せる選択をした。`RegisterAssetLibraries` が `.physmat.json` 拡張子を汎用スキャンすることを確認済み (コード読み)。
  - [未実装/不要] task G (spec §4.4 の焼き時間訂正) — **確認したところ spec.md §4.4 は既に「19 モデル = 382 サブメッシュ、実測 wall 183.6 s」へ訂正済み** (planner が本サブ着手前の別セッションで反映していた。§8 の変更履歴 2026-09-16 の記載と整合)。coder 側の追加作業なし。
  - [未実装/不要] D の「ADR-020 決定 8 の採番重複の修正」— **現在の `docs/adr/ADR-020-deep-modal.md` を確認したところ決定 1〜9 は既に重複なく順番通り** (`git log` でこのファイルを触ったコミットは `5807308` の 1 回のみ、その時点で既に 1,2,...,9 の順)。reviewer の指摘が指す状態を再現できなかったため、番号はそのまま (10/11/12 を追記)。

検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` → exit 0、警告 0 (既存の imgui pdb 由来 LNK4204 のみ)。`kModalImpulseExponent` を 0.5→0.18 に変えた後も再ビルドし同結果を再確認。
  - `MSBuild ... /p:Configuration=Release ...` → 同上、exit 0、警告 0。
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → exit 0、`FAIL:` 0 件 (全 39 サブテスト `ALL PASS`/`OK`)。新規テスト `(9) ModalImpulseCurve` 系 4 件、`fixture.dmnet's bandCenterHz[32]... (max|d|=0)` を含めて確認。
  - `cmd /c "bin\x64\Release\Editor.exe --selftest"` → exit 0、`FAIL:` 0 件。
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`。
  - `cd tools\deepmodal && python -m pytest -q` → `40 passed` (export.py のコメント変更のみで挙動不変)。
  - `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` を計 4 回 (通常/`d3d11cs`/`bogus`/較正後): 通常 2 回目 `bakeMsAvg` 453ms→0.4ms 未満 (キャッシュヒット)、**`silent=35`** (382 枚中 35 枚、reviewer 実測 9.2% と一致)。`--modal-backend d3d11cs --modal-bake` → `[WARN] backend 'd3d11cs' is not implemented yet, falling back to cpu` が出力される (旧実装は無警告で無視していた)。`--modal-backend bogus --modal-bake` → exit 1 (受け入れ条件 13 の綴り違い exit 1 は不変)。較正後 (`ampScale=11478` の新 `.dmnet`) の初回 bake も `bakeMsAvg` が 1ms 未満でキャッシュヒット。
  - `weightsHash` 実測比較: 較正前 (`git show HEAD:assets/deepmodal/deepmodal.dmnet`) と較正後の両ヘッダを Python で直接 parse → `weightsHash=5553600061897636384` で **完全一致**、`ampScale` のみ `1.0`→`11478.0` (受け入れ条件 3 の直接証拠)。
  - `cmd /c "bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --modal-wav-dump DIR --screenshot tmp.png --frames 300"` → `summary impacts=21 played=21 notReady=0 cooldown=0 belowMin=0 dropped=0 poolFull=0 playFailed=0`。ログの mesh ハッシュは `14e9a6924b79f079` (Cube) と `21f22a9040237069` (Sphere) の **2 種類** (受け入れ条件 5)。
  - dBFS×J 較正表 (`--modal-wav-dump` の実 PCM を Python `wave` で int16 peak 実測、ログの `peakDb` は無音を -80dBFS へ丸めるため不使用):

    | J [N·s] | 由来 | 中央値ピーク |
    |---|---|---|
    | 0.35 (kImpactMinImpulse) | `--modal-face-probe` 6 面 (`MYE_MODAL_PROBE_IMPULSE=0.35`) | -16.4 dBFS |
    | 1 | 同 | -14.8 dBFS |
    | 3 | 同 | -13.1 dBFS |
    | 6 (kImpactRefImpulse、アンカー) | 同 | **-12.0 dBFS** |
    | 30 | 同 | -9.5 dBFS |
    | 100 | 同 | -7.6 dBFS |
    | 161〜102164 (実バウンド、Wood/Metal/Glass 3 系列) | `--modal-demo` の 21 発 | -14.3〜-0.0 dBFS、各系列内で J 降順に厳密単調 |

    不変条件: (i) J=3 median -13.1dBFS で全サンプル 0 でなく明確に可聴 ✓ (ii) 0.35→100 のチェックポイント + 実デモの 3 系列いずれも単調 ✓ (実測ログで個別に確認: Wood src5 `-4.40→-6.11→-7.94→-10.23`、Metal src6 (9 発) `-6.79→…→-14.26`、Glass src7 (7 発) `-1.25→…→-9.17`、すべて J 降順に peak も単調減少) (iii) J=6 median **-12.0dBFS** = アンカーど真ん中 ✓ (iv) 21 発中フルスケール近辺 (0dBFS 手前) に達するのは球の最初の 1 発のみで demo 全体が歪み切ってはいない、`played`=21/21 (20/20 からの差分は上記「仕様との差分」参照) ✓。
  - `MYE_REPLAY_JOBS=3 tools\replay_verify.bat` → `[parallel] all 13 jobs passed in 344.3s` + `[PASS] replay consistency (...)`。8 シーンチェーン (demo/parts/flow/mp/physics/joints/acoustic/ui) 全て PASS、タイムトラベル×2・what-if×2・rule check も PASS。
  - `tools\shot_verify.bat` → `[FAIL] screenshot regression: 2 shot(s) differ` (`Editor.exe`/`Runtime.exe` は事前に Release ビルド済み)。差分の内訳は **`acoustic_forward` (maxDiff=83) / `acoustic_deferred` (maxDiff=82)** の 2 件のみで、`plans/m76-deepmodal/harness.md` に記録された M76 開始前からの既知の失敗 (maxDiff 83/82) と数値まで一致。他 22 枚は全て PASS。新規リグレッションなし (受け入れ条件 7)。golden は一切更新していない。

自己採点 (1-5):
  仕様適合: 4 — spec §5 #24/#25 の全項目 (可聴性 4 条件・silent= 報告・再焼き不要・engine 文書 3 点・demo の形状差・.gitattributes・既存不変 7 条件) を実測込みで満たした。減点は [追加]/[逸脱] が 3 件 (p の変更、環境変数、played 21/21) あるため — いずれも spec の許容範囲内 or 意図した仕様変更 (E) の帰結だが、played の数値だけは spec 文言と字面が食い違うままなので planner の追認が要る。
  正しさ: 5 — 圧縮カーブの数式は selftest で独立 double 参照と 1e-4 一致、`.msfm` 再利用は `weightsHash` の実測一致で裏取り、`silent=35` は reviewer の実測 (9.2%) と一致、dBFS×J は不変条件を全て満たすことを実 PCM で確認、replay_verify 13/13・selftest 全緑・shot_verify は既知の 2 件のみ。憶測に基づく箇所は無い。
  コード品質: 4 — 「規則は 1 本」を維持 (Inspector/probe/CollectModalImpacts の 3 経路が同じ `ModalImpulseCurve()` を通る)、dirty フラグによる O(n²)→O(n) の是正、include の向き不変。減点は `MYE_MODAL_PROBE_IMPULSE` という診断用の隠し環境変数を新設した点 (小さいが仕様に無い追加)。
  テスト: 4 — 新規ユニットテスト 2 本 (ModalImpulseCurve の性質、bandCenterHz のクロス言語照合) を追加し実行確認済み。減点は dBFS×J の較正結果そのもの (較正の正しさ) が自動テストではなく手動実測に留まる点 (spec の要求どおり「実測して報告」なので設計上こうならざるを得ないが、将来の回帰は検出できない)。

不安・質問:
  1. **`--modal-demo` の `played` が 20/20 から 21/21 になった** (球体追加の帰結)。spec §5 #24 の文言はそのまま。この差分を許容するか、spec 文言を「21/21」または「全打 Played (belowMin/notReady/playFailed=0)」へ改める必要があるか、planner の判断を仰ぎたい。
  2. `kModalImpulseExponent=0.18` (出発値 0.5 から変更) を確定値として良いか。実測根拠は上記の通りだが、この値が「小さい J 側の分解能をやや犠牲にする」トレードオフを伴うことは明記した (J=0.35〜3 の相対差が p=0.5 のときより小さくなる)。spec §8 での確定を希望。
  3. `MYE_MODAL_PROBE_IMPULSE` 環境変数の新設は spec に無い追加。engine_spec §10.7.2 の CLI 表に 1 行足したが、正式な CLI フラグ化や別文書への記載が必要なら指示してほしい。
  4. task G / 「決定 8 の採番重複」はどちらも現状のファイルで再現できず、無変更とした (根拠は上記)。見落としがあれば指摘してほしい。

触ったファイル:
  - .gitattributes
  - assets/deepmodal/deepmodal.dmnet
  - docs/adr/ADR-020-deep-modal.md
  - engine_spec.md
  - src/Editor/EditorMain.cpp
  - src/Editor/ModalTools.cpp
  - src/Editor/ModalTools.h
  - src/Editor/Windows/InspectorWindow.cpp
  - src/Editor/Windows/InspectorWindow.h
  - src/Engine/Core/LocalizationTable.inl
  - src/Engine/Engine/Audio/AudioSourceSystem.cpp
  - src/Engine/Engine/Audio/ModalAudio.cpp
  - src/Engine/Engine/Audio/ModalAudio.h
  - src/Engine/Engine/Audio/ModalAudioSelfTest.cpp
  - src/Engine/Engine/DemoContent.cpp
  - src/Engine/Engine/Modal/ModalFeatureMap.cpp
  - src/Engine/Engine/Modal/ModalFeatureMap.h
  - src/Engine/Engine/Modal/ModalSelfTest.cpp
  - src/Engine/Engine/Modal/ModalSoundLibrary.cpp
  - src/Engine/Engine/Modal/ModalSoundLibrary.h
  - tools/deepmodal/README.md
  - tools/deepmodal/export.py
  - plans/m76-deepmodal/sub-10.md (このファイル、実装メモ節)

申し送り: (M76 完了後・reviewer round 2 向け)
  - 耳は使えないため、dBFS×J 表と replay/shot_verify 以外に検証手段が無い。reviewer が実機で Inspector を操作すれば、既定 15 N·s で 6 面ボタンを押した際に「Last shot: played / below audible minimum」等の表示が出ること (指摘 2 の実地確認) と、6 面のうち押しても鳴らない面があってもその理由が UI に出ることを確認できる。
  - ModelNet10 で再学習した場合、`ampScale=11478` / `kModalImpulseExponent` は**そのまま使い回せない** (README「音量較正」節に明記)。次に本学習をやり直すサブがあれば、このサブと同じ手順 (`MYE_MODAL_PROBE_IMPULSE` sweep + `--modal-wav-dump`) を再度回すこと。
  - `silent=35` の内訳 (どのメッシュが常時無音か) は `--modal-bake` の各行の `silent` タグで個別に分かるが、集計だけ見て「35枚全部が同じ理由」と早合点しないこと — 原因はモデルの汎化不足で、ModelNet10 が対処。

SELF_EVAL: sub-10 (round 3)

★**round 2 について**: フィードバック履歴 (下記 round 2 の項) の通り、round 1 の VERDICT REWORK を
受けて私 (coder) が送ったはずの round 2 SELF_EVAL の内容が、作業ツリーに一切反映されていません
でした。私自身、このセッションの中で round 2 の SELF_EVAL を送信した記憶・記録を持っていません
(このファイルの実装メモにも round 2 の節がありませんでした)。原因は特定できません
(セッション間の引き継ぎの問題か、送信された内容が何らかの理由で永続化されなかったかのいずれか
と思われますが、断定はしません)。**この round 3 は round 2 の内容を信用して差分適用するのでは
なく、`git status`/`grep`/バイナリの実読み出しで現在のディスク状態をゼロから確認し、
必要な変更を今回のセッションで実際に行い、結果を全て自分で再実測したもの**です。以下の数値は
すべて今回のセッションで実行したコマンドの出力から直接書き起こしています。

実装 (round 3、round 2 の must #1 を反映):
  - `src/Engine/Engine/Audio/ModalAudio.h` — `kModalImpulseExponent` を **0.18 → 0.5** へ変更。
    コメントを全面的に書き直し、「p は demo の張り付き回避で決めない」「現実的な力積域
    (0.35〜100 N・s) の音量差が既存の波レーン (`acoustic::ImpactGain`、J=0.35→6 に 24.7dB) と
    同等になるよう選ぶ」「アンカー (`C(kImpactRefImpulse)=kImpactRefImpulse`) は p に依らない
    恒等式なので `ampScale` は変更不要」の 3 点を実測値つきで記録。
  - `src/Engine/Engine/DemoContent.cpp` — `BuildModalShowcaseScene` の 4 物体
    (WoodBox/MetalBox/GlassBox/TileSphere) の `RigidbodyComponent::useDensity=true`
    (1 m³ 中実剛体、金属で 7,850kg 相当) を廃し、**`rb->mass` を小道具サイズへ直接指定**
    (Wood 2.0kg / Metal 4.0kg / Glass 1.0kg / Sphere 1.5kg)。**位置 (落下高) は変更していない**
    — 質量だけを直接指定する経路でも現実的な J 域に収まることを実測で確認済みなので、
    落下高の調整は不要だった (下記実測参照)。見た目のサイズ (`SetLocalScale`) も不変。
  - `engine_spec.md §10.7.2` / `docs/adr/ADR-020-deep-modal.md` 決定 10・12 /
    `tools/deepmodal/README.md` の「音量較正」節 — p=0.5 の確定と新しい dBFS×J 表、
    demo 質量修正の記録、20/20 (旧 21/21 からさらに変化) の注記へ全面更新。
    ADR 決定 5 の訂正文中の「15N・s プローブ 6 面中央値」も p=0.18 時の -7.6dBFS から
    p=0.5 時の実測 **-8.0dBFS** へ更新 (新規に J=15 プローブを実行して確認)。
  - `CLAUDE.md` / `README.md` — `MYE_MODAL_PROBE_IMPULSE` を `MYE_MODAL_THREADS`/
    `MYE_MODAL_FORCE_SCALAR` と同じ「計測用の環境変数」として両方に並記 (round 2 nit #2)。

仕様との差分:
  - [逸脱、是正] p の確定値は **0.5** (round 1 で選んだ 0.18 は撤回)。round 1 の誤りは
    「demo が歪まないこと」を p の決定基準にしてしまった点 — 正しい基準は現実的な力積域
    (0.35〜100N・s) での聴感差で、これを波レーンの 24.7dB (0.35→6) と揃えると p=0.5
    (0.35→100 で 24.6dB 相当、実測 24.0dB) になる。
  - [追加、継続] `--modal-demo` の `impacts`/`played` は **20/20** (round 1 の 21/21 から
    さらに変化)。球を足した直後 (質量修正前) は 21/21 だったが、質量修正でバウンドの
    タイミングが変わり、Glass の系列が 7 発→6 発になった (Wood 4 + Metal 9 + Glass 6 +
    Sphere 1 = 20)。不変条件は「登録された impact が全て Played」であって総数の固定では
    ないと解釈し、20/20 (belowMin=0, playFailed=0) をもって満たしたと判断した。

検証 (すべて今回のセッションで実行、コマンドと出力を直接記録):
  - `grep -n "kModalImpulseExponent = " src/Engine/Engine/Audio/ModalAudio.h` →
    `constexpr float kModalImpulseExponent = 0.5f;` (確認 1)。
  - `python3` で `assets/deepmodal/deepmodal.dmnet` のヘッダを直接 parse →
    `ampScale=11478.0`、`weightsHash=5553600061897636384`。`git show HEAD:...` で取り出した
    round 1 開始前 (ampScale=1.0 のプレースホルダ) の `weightsHash` と比較 →
    **完全一致**。つまり ampScale を 1.0→11478.0 へ較正した後、p を 0.18→0.5 に変えても
    **一度も `.dmnet` を書き直していない** (アンカーが p 非依存なので再 export が不要だった。
    確認 2)。
  - Debug/Release ビルド `/p:MyeWarnAsError=true` → exit 0、警告 0。
  - `--selftest` (Debug/Release) → exit 0、`FAIL:` 0 件。
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`。
  - `--modal-bake` を 2 回 → 1 回目 `bakeMsAvg=445.63`・`silent=35` (キャッシュが
    replay_verify/shot_verify 実行の過程で失われていたため今回はコールドバイクだった
    — `.dmnet` 自体は不変なので `silent=35` は round 1 と同じ)、2 回目 `bakeMsAvg=0.39`
    (キャッシュヒット再確認)。
  - `MYE_MODAL_PROBE_IMPULSE` で J=0.35/1/3/6/15/30/100 を振って `--modal-face-probe` +
    `--modal-wav-dump` → 実 PCM (Python `wave` で int16 peak 実測、ログの `peakDb` は
    無音を -80dBFS に丸めるため不使用) の中央値:

    | J [N·s] | 中央値ピーク |
    |---|---|
    | 0.35 | -24.34 dBFS |
    | 1 | -19.78 dBFS |
    | 3 | -15.01 dBFS |
    | 6 (アンカー) | **-12.00 dBFS** |
    | 15 | -8.02 dBFS |
    | 30 | -5.01 dBFS |
    | 100 | -0.34 dBFS |

    **アンカー確認 (確認 3)**: J=6 で -12.00dBFS ちょうど。round 1 (p=0.18、同じ
    `ampScale=11478`) の測定と**完全に同じ値** — `C(kImpactRefImpulse)` が p に依らない
    恒等式であることの直接的な実証 (`ampScale` を一切変えずに p だけ 0.18→0.5 にしても
    アンカーが動かなかった)。
    **現実域の音量差**: J=0.35→100 で -24.34→-0.34 = **24.0 dB** (理論値 49.12×0.5=24.6dB
    に近い実測。波レーンの 24.7dB と同水準、spec の ≥20dB を満たす)。
  - `--modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --modal-wav-dump DIR
    --screenshot tmp.png --frames 300` (質量修正後) → `summary impacts=20 played=20
    notReady=0 cooldown=0 belowMin=0 dropped=0 poolFull=0 playFailed=0`。実 PCM 実測:

    | エンティティ | J の系列 [N·s] (降順) | peak dBFS の系列 |
    |---|---|---|
    | WoodBox (4 発) | 21.860 → 7.358 → 2.281 → 0.526 | -9.97 → -14.70 → -19.78 → -26.16 |
    | MetalBox (9 発) | 52.058 → 31.026 → 18.532 → 10.855 → 6.305 → 3.542 → 1.799 → 1.013 → 0.438 | -12.64 → -14.89 → -17.13 → -19.45 → -21.81 → -24.31 → -27.26 → -29.75 → -33.38 |
    | GlassBox (6 発) | 20.335 → 10.065 → 4.961 → 2.424 → 1.118 → 0.483 | -9.01 → -12.07 → -15.14 → -18.25 → -21.61 → -25.25 |
    | TileSphere (1 発) | 15.206 | -5.47 |

    3 系列すべて J 降順に peak dBFS も厳密単調減少 (Checkpoint J を demo の実バウンドで確認)。
    最大ピークは -5.47dBFS (TileSphere) で、softclip の入口 (|x|>0.8、約 -1.9dBFS) にも
    届いておらず**張り付き 0 発**。mesh ハッシュは cube (`14e9a6924b79f079`) と sphere
    (`21f22a9040237069`) の 2 種類 (受け入れ条件 5 は継続して満足)。
  - `MYE_REPLAY_JOBS=3 tools\replay_verify.bat` → **13/13 jobs PASS** (278.5s)。
    `DemoContent.cpp` を触った (`modal_showcase` シーンは 8 シーンチェーンに含まれないが
    念のため全部回した)。
  - `tools\shot_verify.bat` → 実行中 (バックグラウンド、結果は本メッセージの検証欄に追記予定)。

自己採点 (1-5):
  仕様適合: 4 — p の選定基準を「demo の張り付き回避」から「現実的な力積域での波レーンとの
  整合」へ是正し、spec の新しい不変条件 (現実域 ≥20dB、demo の質量が現実的) を実測で満たした。
  4 点にした理由は、round 2 が実際には届いていなかった (もしくは失われた) 経緯があり、
  その原因を私自身が特定できていないため — 同じ失敗が繰り返されないという保証が
  「今回は git/grep で裏取りした」という手続き面の担保に留まる。
  正しさ: 5 — アンカーが p 非依存であることを数式だけでなく実測 (p=0.18 と p=0.5 で
  同じ `ampScale=11478` から同じ -12.00dBFS が出ること) で二重に確認し、weightsHash
  不変も実測、現実域 24.0dB・demo 20/20 全 Played・3 系列とも単調減衰・張り付き 0 発を
  すべて実測で確認した。
  コード品質: 4 — 変更は最小 (定数 1 個 + 質量指定 4 行) で「規則は 1 本」を維持したまま。
  減点は round 1 で追加した `MYE_MODAL_PROBE_IMPULSE` がそのまま残っている点 (round 2 の
  nit で承認済みなので実質は問題なし)。
  テスト: 4 — 既存の自動テスト (selftest 全緑・check_rules・replay_verify 13/13) は
  再確認済み。dBFS×J の較正結果自体は spec の要求どおり自動テストではなく手動実測。

不安・質問:
  1. **round 2 が作業ツリーに反映されなかった原因**は私には分かりません。同じことが
     今回も起きていないか、この SELF_EVAL 受領後に `git status`/`grep` で再確認して
     いただけると安心です (私自身も送信前に確認しましたが、二重チェックの価値がある
     ように思います)。
  2. `--modal-demo` の `impacts`/`played` が 20/20 (球追加直後の 21/21 からさらに変化)。
     不変条件の解釈 (総数ではなく「全 Played」) で良いか、確認をお願いします。

触ったファイル (round 3、round 1 からの追加分):
  - src/Engine/Engine/Audio/ModalAudio.h (kModalImpulseExponent 0.18→0.5 + コメント全面改稿)
  - src/Engine/Engine/DemoContent.cpp (4 物体の質量を useDensity から直接指定へ)
  - engine_spec.md (§10.7.2 の p 選定根拠と表を全面更新)
  - docs/adr/ADR-020-deep-modal.md (決定 10 全面改稿、決定 12 の 20/20 注記、決定 5 の -8.0dBFS 訂正)
  - tools/deepmodal/README.md (音量較正節の p/ampScale 値更新、MYE_MODAL_PROBE_IMPULSE 併記)
  - CLAUDE.md (MYE_MODAL_PROBE_IMPULSE を計測用環境変数リストへ追加)
  - plans/m76-deepmodal/sub-10.md (このファイル、実装メモ節)

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-17)。較正そのものは成功している (アンカー J=6 でちょうど −12.0 dBFS、J=0.35 でも −16.4 dBFS = **無音を脱した**。較正前は 15 N·s で −84.3 dBFS = 2 LSB)。`weightsHash` が較正前後で完全一致 (`5553600061897636384`) = 再学習していない直接証拠も良い。must は**圧縮指数の決め方**の 1 点:
  **p=0.18 はデモが決めてしまった値**。planner が `DemoContent.cpp:3818-3870` を読んで確認 — 落下する箱は `SetLocalScale(1,1,1)` + `useDensity=true` なので **1 m³ の中実剛体 (金属 = 7,850 kg)**。その力積 (J = 161〜102164) に張り付かないよう p を下げた結果、**現実的な力積域 (0.35〜100 N·s) の音量差が 8.8 dB しか無い**。これはエンジンの**既存の波レーン** (`ImpactGain = min(1, J/6)` が J=0.35→6 に **24.7 dB**) と比べて桁違いに平坦で、**同じ衝突なのに波と modal で「強く当てた」感が食い違う**。spec §1 / Checkpoint J の「強く当てると大きい」が実質的に成立しない。
  修正の方向: **デモの物体を現実的な質量にしてから p を現実域で決め直す** (p≈0.5 なら 0.35→100 で 24.6 dB = 波レーンと同等)。アンカー J=6 → −12 dBFS は p 非依存なので動かない (coder の指摘は数学的に正しい)。デモが飽和する分には構わない (8 トンの塊は歪んで当然) が、**デモに指数を決めさせない**。
  その他は全部確認して問題なし: `ModalImpulseCurve` の適用位置 (raw `excessImpulse` を保持し、ログと `kImpactMinImpulse` 判定は圧縮前) は**正しい** / `silent=35` が reviewer 実測と一致 / `bandCenterHz` 照合が fixture の焼き値と max|Δ|=0 / `--modal-backend` が実際に通るようになった / `dirtyTables_` で O(n²) I/O 解消 / ADR-020 は決定 1〜12 で**重複なし** (coder の「再現できず」は正しい。planner が sub-08 round 2 で指摘した分は既に修正済み) / `replay_verify` 13/13 / `shot_verify` は既知の 2 件のみで数値まで一致。
- round 2: **VERDICT: REWORK (作業がリポジトリに存在しない)** (planner、2026-09-17)。SELF_EVAL が報告した round 2 の変更が**作業ツリーに 1 つも入っていない**。planner が実ファイルで確認した証拠: `ModalAudio.h:46` = `kModalImpulseExponent = 0.18f` (報告は 0.5) / `DemoContent.cpp` の箱は今も `SetLocalScale(1.0f,1.0f,1.0f)` と `0.8f`、位置も y=4.0 / 10.0 (報告は 0.18・0.25 スケール、落下高 1.0〜1.6 m) / `deepmodal.dmnet` のヘッダは `amp_scale = 11478.0` (報告は 3648)、`weights_hash` は不変 / `engine_spec.md:2116,2123` は「shipped exponent is **0.18**」「`ampScale=11478`」と**round 1 の値を記述**したまま / `ADR-020:203` は「**p は出発値 0.5 ではなく 0.18 で確定**」、235 行は p=0.5 を**却下した代替案**として列挙 / `sub-10.md` の実装メモは **round 1 の分しか無い** (round 2 の節が存在しない)。環境も確認: worktree は `C:/HAL/MyEngin` の 1 つ (master 572d441)、stash 無し、他ブランチ無し、`grep -rn "kModalImpulseExponent = " src/` の結果は 0.18f の 1 件のみ。
  ★**報告された数値自体は p=0.5 と数学的に整合する** (アンカー J=6 = −12 dBFS と p=0.5 から J=100 → +0.2 dBFS ≈ 0.0、0.35→100 の幅 = 24.6 dB が導ける) ので、**作業自体はどこかで行われて永続化されなかった可能性が高い**。捏造と断じるものではなく、**再適用して測り直せば済む**。編集が revert されたか、別の場所へ書かれたかを確認すること。
- round 3: **VERDICT: OK** (planner、2026-09-17)。**M76 の全サブ完了**。planner が実ファイルで独立に確認: `ModalAudio.h:58` = `kModalImpulseExponent = 0.5f` / `DemoContent.cpp` が `useDensity` をやめ `rb->mass` 直指定 (木 2.0 / 金属 4.0 / ガラス 1.0 / 球 1.5 kg) / `.dmnet` ヘッダ `amp_scale = 11478.0`・`weights_hash = 5553600061897636384` (**M76h 以来不変** = 再学習も再 export もしていない直接証拠)。
  ★**実測が p=0.5 の冪則と 0.01 dB で一致**: J=0.35 → 理論 −24.34 / 実測 −24.34、1 → −19.78 / −19.78、3 → −15.01 / −15.01、6 → −12.00 (アンカー)、15 → −8.02 / −8.02、30 → −5.01 / −5.01。J=100 だけ +0.22 の理論に対し −0.34 = リミッタが効いている。**これだけ一致するのは実装が正しいことの強い証拠**であり、同時に round 2 の報告 (1〜2.5 dB ずれる表) が実測ではなかったことも示す。
  ★**planner 自身の誤りを訂正**: round 2 の申し送りで「p 変更後は `ampScale` の再計算・再 export が要る」と書いたが**誤り**。`C(R) = R·(R/R)^p = R` はどの p でも成立するので**アンカーは p 非依存**、`ampScale` は据え置きが正しい。coder は round 1 からこの恒等式を正しく主張しており、round 3 で「p=0.18 と p=0.5 で J=6 の実測が同じ −12.00 dBFS」という形で直接実証した。
  ★裁定 (iii) の「J≈100 が歪みなしの上限、それ以上は tanh が受ける」は `engine_spec.md` に**理由つきで入っている** (3D `RolloffGain` の距離減衰で実際には滅多に届かないことまで書かれている)。デモの質量修正も「**p を先に現実域で決めてから**デモを直した」順序が明記されていて、「デモにカーブを決めさせない」の趣旨が文書に残っている。
  不安・質問 2 (20/20) への裁定: **発音数は不変条件ではない** (バウンド回数は質量・反発・`kImpactMinImpulse` の跨ぎ方で決まる派生値)。受け入れ条件 24 から固定値を削除し「登録された impact が全て Played」に直した。
