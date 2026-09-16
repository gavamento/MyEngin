# review-2 — M76 Deep-Modal (round 2)

- 日付: 2026-09-17
- 対象コミット: `f08cd95` ("M76i: 衝撃力に対する音量カーブの較正と圧縮、既知の限界を engine 文書へ") + 台帳 `266400e`
- 前回: `plans/m76-deepmodal/review-1.md` (round 1、範囲 `99803eb..84ce5e9`)
- 実行環境: Windows 11 / VS2022 (MSBuild 18) / x64、AVX2 対応機

---

REVIEW: FAIL
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の major 1 は**本当に直っている**。自分で測り直した: 圧縮カーブ `C(J)=6·(J/6)^0.5` + `ampScale=11478` により、`--modal-demo` の実バウンドで **J=0.438 → −33.38 dBFS (0.0214 = 701 LSB)**、**J=2.281 → −19.78 dBFS**。round 1 に私が「PCM が全サンプル 0 になる」と書いた「1 kg を 0.5 m 落とす」相当 (J≈3) は、いま明確に可聴。20 発すべてが鳴り (`played=20 belowMin=0`)、**1 サンプルもフルスケールに触れていない** (最大 −5.47 dBFS、`|s|>0.999` は 0 個)。指数 p=0.5 の根拠 (既存の波レーン `ImpactGain` の 24.7 dB と揃える) はデモの質量に依存しない形で立てられており、私の実測でも 0.35→100 N·s の差は **24.3 dB** だった。5 にしない理由は 2 つ: (a) 382 枚中 **35 枚は今も常時無音** (これは較正では直らない — 可視化されたのは前進だが、製品としては 9.2% のアセットが黙ったまま)、(b) **較正アンカーがメッシュ 1 枚 (builtin cube) 基準**で、同じ力積・同じ寸法・同じ材質でもメッシュ間の音量が桁で散らばる (指摘 3)
  機能性: 4 — 自分で回せた検証はすべて緑 (下の「検証した手段」)。`replay_verify` 13 ジョブ全緑、`--modal-demo` の 2 run が 20 行バイト一致、`--modal-bake` が `silent=35` を報告 (私の round 1 の独立計測 35 と一致)、`--modal-backend` が効くようになり WARN も出る、**較正前に焼いた `.msfm` が較正後の `.dmnet` でそのまま再利用される**ことも実測で確認。減点は指摘 1 (root `README.md` が受け入れ条件 25 / sub-10 受け入れ 4 の要求どおりに更新されていない。しかも SELF_EVAL は更新したと書いている)
  ビジュアルデザイン: 5 — 実機で確認 (`scratchpad/g1c.png` / `g2c.png` / `g0.png`): 既定 `Impulse (N*s)` が **15.00**、`+X` を押すと `Last shot: played` が面ボタンと `Export WAV` の間に出て `Export WAV` が有効化される。文言 (`below audible minimum (raise Impulse)` / 「可聴下限未満 (力積を上げてください)」) は次の行動まで示していて良い。`TileSphere` が Hierarchy とシーンビューに出ている (9 entities)。既存 Inspector の体裁・余白・並びから外れておらず、視覚上の指摘なし
  コード品質: 4 — 「規則は 1 本」は圧縮カーブでも守られている: `PendingModalImpact.k[]` を作る 3 箇所 (`ModalAudio.cpp:188`、`InspectorWindow.cpp:1026`、`AudioSourceSystem.cpp:869`) が**すべて** `ModalImpulseCurve()` を通す (grep で全数確認)。`ModalAudio.h` のコメントが「round 1 の診断は逆だった (張り付きの原因は demo の質量であって p ではない)」を自分の誤りとして残しているのは、このリポジトリの流儀どおり。減点は指摘 2・4 (表の統計量の定義と、`.msfm` の耐障害性の後退)

指摘:
  1. [major] 宛先: coder — **リポジトリ直下の `README.md` が 1 バイトも更新されていない。sub-10 の受け入れ条件 4 (= spec §5 の受け入れ条件 25) が明示的に要求している 3 文書のうち 1 つが欠けており、しかも SELF_EVAL は更新したと書いている** — 根拠:
     (a) `git show --name-only f08cd95 | grep -i readme` → **`tools/deepmodal/README.md` のみ**。`git log 84ce5e9..HEAD -- README.md` は空 = root README は M76i で無変更。
     (b) `plans/m76-deepmodal/sub-10.md:130` = 「4. **engine 文書に制約が載る**: `engine_spec.md §10.7` / `ADR-020` / **`README.md`** に (a) 音量則と較正アンカー (b) 全 mask off が出ること・数の出し方・`maskThreshold` の緩和 (c) …」。同じ sub-10 は `tools\deepmodal\README.md` を別行 (77 行) で区別して書いているので、130 行の `README.md` は root を指す。spec §5 の受け入れ条件 25 も同じ 3 点セット。
     (c) SELF_EVAL (`sub-10.md:271`) は「`CLAUDE.md` / `README.md` — `MYE_MODAL_PROBE_IMPULSE` を … 併記」と書いているが、`grep -c MYE_MODAL_PROBE_IMPULSE README.md` → **0** (CLAUDE.md には入っている)。**やったと書いてあるがやられていない**。
     (d) 実害: root README の Deep-Modal 節 (README.md:108-118) は今も「音量は正規化しない絶対値 — 弱い衝突は小さく、強い衝突は大きい」のままで、**M76i で音量則が比例から冪則 (p=0.5) に変わった事実が反映されていない**。`grep "ampScale|音量|較正|無音|silent" README.md` は 116 行のこの 1 文しか拾わない = 較正アンカーも「35/382 が常時無音」も root README には無い。**round 1 の major 1 の核心は「制約が `plans/` にしか無い」だった**ので、engine_spec §10.7.2 と ADR-020 決定 10〜12 が素晴らしい出来である一方で、3 本目の脚が抜けたままなのは是正が未完了ということ。
     — 期待: root `README.md` の Deep-Modal 節に (i) 音量は絶対値だが `C(J)=R·(J/R)^0.5` の圧縮を通ること (ii) 較正アンカー (`J=6 N·s` で約 −12 dBFS) (iii) stage1 モデルでは全 mask off のメッシュが出ること・`--modal-bake` の `silent=N` で数えられること、を 2〜4 行で足す。`MYE_MODAL_PROBE_IMPULSE` の併記も SELF_EVAL の記述に合わせるか、SELF_EVAL の方を訂正すること。

  2. [minor] 宛先: coder — **`engine_spec.md §10.7.2` / `ADR-020` 決定 10 の dBFS×J 表は「6 面の中央値」と書いてあるが、実際は「上側中央値 (昇順 6 個の 4 番目)」で、素直な中央値より一律 1.78 dB 高い。表の数字が記載どおりの手順では再現できない** — 根拠: `MYE_MODAL_PROBE_IMPULSE` で J を振り、`--modal-face-probe` の 6 枚を私が実測した (peak は PCM から、`--modal-wav-dump`)。

     | J | 文書の値 | 私の中央値 (中央 2 値の平均) | 私の上側中央値 `sorted[3]` | 6 面の幅 |
     |---|---|---|---|---|
     | 0.35 | −24.3 | **−26.12** | −24.34 | −36.90 〜 −20.61 |
     | 1 | −19.8 | **−21.56** | −19.78 | −32.36 〜 −16.05 |
     | 3 | −15.0 | **−16.79** | −15.01 | −27.58 〜 −11.28 |
     | 6 (アンカー) | −12.0 | **−13.78** | −12.00 | −24.57 〜 −8.27 |
     | 30 | −5.0 | **−6.79** | −5.01 | −17.58 〜 −1.30 |
     | 100 | −0.3 | **−1.84** | −0.34 | −12.35 〜 −0.00 |

     文書の 6 行が私の `sorted[3]` と**全点で一致**するので、`numpy.median` ではなく `sorted[len//2]` が使われたと判断できる (偶数個での上側中央値)。★**合否は変わらない** — 受け入れ条件は「J=6 で −12 dBFS ± 3 dB」で、素直な中央値 −13.78 も範囲内。問題は再現性だけ。加えて同じ表の直後の「`100` N·s is deliberately the point where an undistorted hit tops out」も楽観が入っている: J=100 の最大面 (px) は peak 0.99979 で、softclip の逆関数から逆算した**圧縮前振幅は約 1.55 = 約 3.8 dB のゲインリダクション**が全サンプルの 1.05% にかかっている (J=30 では 0.10% / 0.02 dB、J=6 では 0%)。— 期待: 表の脚注に「中央値は昇順 6 個の 4 番目 (上側中央値)」と書くか、`numpy.median` 定義へ数字を直すこと。「undistorted」は「最も強い面で約 4 dB のソフト圧縮が始まる点」程度に緩めること。

  3. [minor] 宛先: planner — **較正アンカーがメッシュ 1 枚 (builtin cube) の値で、アセット全体を代表していない。stage1 モデルではメッシュ間の音量が桁で散らばる** — 根拠: 焼いた `.msfm` 382 枚について `BuildModes` を Python で再実装し、**全メッシュを同条件** (参照材質・実寸 1 m・`k = C(6)`) に揃えて「生き残った帯域の振幅和 Σa」を cell 中央値で出した (同じ proxy を全メッシュに同じ式で適用しているので、絶対値ではなく**相対のばらつき**として読める):
     - 中央値が 0 (= その cell では無音) のメッシュ **41 / 382**
     - 非ゼロの分布: min 1.9e-4 / p10 0.569 / median 1.925 / p90 14.13 / max 85.36
     - **p10→p90 で 27.9 dB、min→max で 113 dB**
     直接測定でも裏が取れる: 同じ `--modal-demo` の 1 run で、立方体 (src=5、wood、J=21.86) が −9.97 dBFS、球 (src=8、tile、J=15.21) が **−5.47 dBFS** — `C(J)∝√J` で J を揃えると約 6 dB の差が残る (材質も違うので厳密ではないが、同じオーダーの話)。同じ cube でも**面によって J=6 で −24.57 〜 −8.27 dBFS = 16.3 dB の開き**がある (指摘 2 の表)。つまり「J=6 で −12 dBFS」は *builtin cube の上側中央値* の話であって、ユーザーが自分のアセットに `ModalSound` を付けたときに得られる水準ではない。★これは round 1 の major 1 の再燃ではない (較正自体は目的を達している) し、原因は既に文書化済みの「モデルの汎化 (学習 124 形状)」と同根。 — 期待: engine_spec §10.7.2 の較正節に「アンカーは 1 メッシュ・上側中央値の値であり、stage1 モデルではメッシュ間・面間の水準が正規化されていない (実測: メッシュ間 p10→p90 で約 28 dB、cube の面間で 16 dB)」を 1〜2 文足す。ModelNet10 後に**複数メッシュの中央値**でアンカーを取り直すかは、そのときの判断材料として §7 に残す。

  4. [minor] 宛先: coder — **`.msfm` の書き込みが「終了時 1 回」だけになり、クラッシュ / 強制終了で焼いた結果が丸ごと失われるようになった (round 1 指摘 3 の是正の副作用)** — 根拠: `grep -rn FlushDirtyTables src/` の呼び手は **`ModalSoundLibrary::Clear()` / `::Shutdown()` / `--modal-bake` の 3 箇所だけ**。`UpdateTableEntry` (`ModalSoundLibrary.cpp:388-392`) は `dirtyTables_.insert()` するのみ。`Shutdown()` は `EngineLoop` の正常終了パスでしか呼ばれないので、**エディタを長時間動かして裏で数百メッシュを焼いた後にクラッシュ/タスクキルすると、その全部が消えて次回起動で焼き直し**になる (M76e〜h の実装では entry ごとに保存されていたので durable だった)。キャッシュなので正しさは損なわれないが、`--modal-bake` を挟まない普通の使い方 (非同期ワーカー任せ) では焼き直しが 382 メッシュ × 0.43 s = 3 分級。なお **I/O 削減そのものの体感効果は無い**: 1 回目の `--modal-bake` の wall は round 1 の 183.6 s に対し **174.1 s** (`bakeMsAvg` 453.65 → 427.66) で、差は CPU 側のゆらぎと同程度 — round 1 に私が挙げた 2.2 GB の累積書き込みは実在したが、ボトルネックではなかった。 — 期待: `Pump()` に「新規 entry が N 件たまったら」または「シーン切り替え時」の flush を足して、落ちても直近までは残るようにする (毎 entry へ戻す必要はない)。

前回指摘の消込:
  1. [major] `ampScale` 未較正 → **解消 (ただし文書の 1 本が未完 = 指摘 1 として残す)**。
     - 較正: `.dmnet` ヘッダを自分で読み直して `ampScale = 11478.0` (round 1 は 1.0)、`weightsHash = 0x4d1259d501fefa20` は **round 1 と同一** = 重みは不変、ファイルサイズも 3,368,464 B で不変。
     - 圧縮カーブ: `ModalImpulseCurve` が `k[]` を作る 3 経路すべてに入っていることを grep で確認。selftest に「C(J) is strictly increasing across the Checkpoint J range」「ModalImpulseCurve matches an independently written double reference」が追加されている。
     - 実用域の救済 (round 1 の私の測り方で再測定): `--modal-demo` の 20 発は J = 0.438〜52.058 で peak −33.38〜−5.47 dBFS。**round 1 に「J≈3 なら PCM 全サンプル 0」と書いた帯域 (J=2.281) が −19.78 dBFS** = 0.1025 linear = 3359 LSB。round 1 の peak/J 線形係数 (Wood 2.41e-6) はもう成り立たない (冪則になった) ので、「1 kg × 0.5 m が無音」という私の指摘は**事実として解消**。
     - 「軽い接触が無音 / 重い衝突が張り付く」の両立不能も解消: 20 発すべてで `|s| > 0.999` のサンプルが **0 個**、最大でも −5.47 dBFS (softclip の膝 −1.94 dBFS の手前)。
     - 文書: `engine_spec.md §10.7.2` (約 100 行、音量則・アンカー・dBFS×J 表・demo の質量修正の順序・`ampScale` が mask の後段である証明・35/382) と `ADR-020` 決定 10〜12 は**私の round 1 の指摘 (e) を完全に埋めている**。残るのは root README (指摘 1)。
     - 「35 枚は較正で救えない」という planner の判定はコードで裏が取れる (`ModalSynth.cpp` の `BuildModes` 手順 3 の mask 判定が手順 4 の `ampScale` 乗算より前)。`--modal-bake` が **`silent=35`** を報告し、35 行に ` silent` タグが付くことを実行で確認 — 私の round 1 の独立計測 (35 枚 / 9.2%) と一致。
  2. [major] Inspector が既定値で無反応・無通知 → **解消**。実機で確認: 既定 `Impulse (N*s)` が **15.00** (`g1c.png`)、`+X` を押すと `Last shot: played` が出て `Export WAV` が有効化 (`g2c.png`)。定数は `kModalPreviewDefaultImpulse` (`ModalAudio.h`) 1 本で、ヘッドレスの `--modal-face-probe` と共有 (`AudioSourceSystem.cpp:862`)。結果表示は `Played` を含む 7 種すべてを `ModalShotResultLabel()` が網羅し、en/ja 8 エントリが `LocalizationTable.inl` にある (`LocalizationSelfTest` 緑)。★`BelowMin` の表示そのものはライブで再現できなかった (較正後は J=0.10 の最弱面 `+Y` でも `played` になる) ので、そこはコード読みと `ModalAudioSelfTest` の `(5) k=0 → BelowMin` までの確認。
  3. [minor] `.msfm` の O(n²) 書き込み → **解消 (ただし副作用を指摘 4 に切り出す)**。`UpdateTableEntry` は `dirtyTables_` に積むだけ、`FlushDirtyTables()` がバッチ末で 1 回。`--modal-bake` 2 回目が `bakeMsAvg=0.39 ms` = 書けて読めていることを実行で確認。
  4. [minor] Mel ドリフト検査が同語反復 → **解消**。`ModalSelfTest.cpp:580-595` が **fixture.dmnet (Python が焼いた値)** と `MelBandCenters()` を照合するようになり、selftest の出力に `PASS: fixture.dmnet's bandCenterHz[32] (written by Python) matches MelBandCenters() (C++) within 1e-2 Hz (max|d|=0)` が出る。
  5. [minor] `--modal-bake` が `--modal-backend` を無視 → **解消**。実行: `Editor.exe --modal-backend d3d11cs --modal-bake` → exit 0 + **`[WARN ] [modal] backend 'd3d11cs' is not implemented yet, falling back to cpu`**、`--modal-backend bogus --modal-bake` → **exit 1**。
  6. [minor] ModalSound 不在シーンでも毎 tick 索引表 → **解消**。`ModalAudio.cpp:98-107` が `ModalSoundComponent` のアーキタイプ有無を先に見て即 return する。`replay_verify` 全緑 = 接触経路に副作用なし。
  7. [minor] demo が全部立方体 → **解消**。`TileSphere` (builtin sphere / tile) が生成順の末尾に追加され、Hierarchy とシーンビューに出る (`g0.png`、9 entities)。ログでも `mesh=21f22a9040237069`・`modes=4 f0=2531.6 f1=4025.3` と cube (`14e9a6924b79f079`、`modes=7 f0=1096.2`) と明確に別の音になっている。
  8. [minor] `.gitattributes` の `*.msfm binary` → **解消** (`.gitattributes:15-18`)。
  9. [minor] ADR-020 決定 5 の金属 α/β → **解消**。`α=6・β=1e-7` (metal) へ訂正され、「旧文は steel の値だった」と理由つきで残っている。あわせて probe 3 枚の絶対レベルが `-84.3 dBFS (振幅 2 LSB)` だった事実も追記された (私が round 1 で指摘した「肯定的証拠として挙げているが絶対レベルが書かれていない」点)。
  10. [minor] spec §4.4 の「100 s 前後」 → **解消**。「**19 モデル = 382 サブメッシュ**で、初回の裏作業は**実測 wall 183.6 s**」へ訂正され、取り違えだったことが明記されている。

検証した手段:
- **ビルド**: `MSBuild MyEngine.sln /p:Configuration=Debug|Release /p:MyeWarnAsError=true` → 両方 exit 0。両ログの `warning C|error C|error MSB` は **0 件**。
- **selftest**: `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → **exit 0**、4819 行、`(0 failure)` 以外の failure 行なし。新規 PASS を個別に確認 (bandCenterHz 照合 / C(J) 単調 / ModalImpulseCurve 参照実装一致 / `(1) k lands on local X (compressed by C(J))`)。
- **静的規則**: `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`。
- **Python**: `cd tools\deepmodal && python -m pytest -q` → **40 passed**。
- **決定論**: `MYE_REPLAY_JOBS=3 tools\replay_verify.bat` → **`[parallel] all 13 jobs passed in 272.9s`** + `[PASS] replay consistency (…8 scenes…) + snapshot round-trip + time travel + rule check`。`git diff --stat 99803eb..HEAD -- tests/golden` は空 = golden 1 枚も未変更 (M76 全体を通して)。作業ツリーは review-2.md を書く前まで完全に clean。
- **`.dmnet` ヘッダ (自分でバイト解釈)**: `ampScale=11478.0` / `maskThreshold=0.5` / `logAmp=-27.625..-5.957` / `refSizeL=0.6` / `paramCount=1682448` / `weightsHash=0x4d1259d501fefa20` (round 1 と同値) / 3,368,464 B。`bandCenterHz[32]` と C++ `MelBandCenters()` の max|Δ| = 3.7e-4 Hz。
- **`--modal-bake`**: (1) 既存キャッシュ無しの状態 → `models=19 bakes=382 bakeMsAvg=427.66 **silent=35**`、wall 174.1 s、` silent` タグ行がちょうど 35 行。(2) 直後の 2 回目 → `bakeMsAvg=0.39` (キャッシュ命中)。(3) ★**round 1 (ampScale=1.0 時代) に焼いた `.msfm` 19 ファイルを書き戻して較正後の `.dmnet` で実行** → `bakeMsAvg=0.62` = **そのまま再利用された** = engine_spec の「純粋な較正変更では再焼き不要」を独立に確認。
- **較正の再測定 (司会の (d))**: `MYE_MODAL_PROBE_IMPULSE` を 0.35 / 1 / 3 / 6 / 30 / 100 に振り、`Runtime.exe --modal-demo --modal-sync-bake --modal-wav-dump DIR --modal-face-probe --synth-input --screenshot … --frames 300` を 6 回。各 run で **6 面すべてが WAV を出す** (round 1 は 15 N·s で 3/6 しか出なかった)。PCM から peak を測った表は指摘 2 に掲載。0.35→100 の幅は私の中央値で **24.28 dB** (文書の主張 24.0 dB / 理論 24.6 dB と整合)。softclip の噛み具合も逆関数で確認 (J=6: 0% / J=30: 0.10%・0.02 dB / J=100: 1.05%・3.83 dB)。
- **demo の音 (司会の (b))**: `--modal-wav-dump` の `shot_*.wav` 20 枚を Python (`wave` + FFT) で実測 → peak −33.38〜−5.47 dBFS、`|s|>0.999` は全枚 **0 サンプル**、支配周波数は wood 1488 / metal 1121 / glass 2336 / **sphere(tile) 2532 Hz**。エンティティごとのバウンド列 (wood 4 発 / metal 9 発 / glass 6 発) は **すべて J の減衰に対して peak が単調**。`[modal] summary impacts=20 played=20 notReady=0 cooldown=0 belowMin=0 dropped=0 poolFull=0 playFailed=0`。
- **決定性 / 縮退**: `--modal-demo … --synth-input --screenshot … --frames 300` を 2 回 → `[modal] t=` **20 行が完全一致 (差分 0)**。`--no-audio` 併用で `[modal] t=` **0 行**。
- **CLI**: `--modal-backend d3d11cs --modal-bake` → exit 0 + WARN + `backend=cpu`。`--modal-backend bogus --modal-bake` → exit 1。
- **実機 GUI (司会の (a))**: `Editor.exe --modal-demo` を起動し Hierarchy から `WoodBox` を選択 → Inspector の ModalSound 節で既定 `Impulse (N*s)` = **15.00** を確認 (`g1c.png`)、`+X` をクリック → **`Last shot: played`** が表示され `Export WAV` が有効化 (`g2c.png`)。スライダ最小 (0.10) + 最弱面 `+Y` でも `played` になる (`g3c.png`) = 較正後は Inspector 経由で無音になる条件を作れないところまで来ている。`TileSphere` の存在も確認 (`g0.png`、9 entities)。
- **深度 (受け入れ条件の外側)**: 焼いた `.msfm` 382 枚に対して `BuildModes` を Python で再実装し、全メッシュ同条件での振幅和の分布を出した (指摘 3)。`PendingModalImpact.k[]` を書く全箇所を grep して 3 経路すべてが `ModalImpulseCurve` を通ることを確認。`FlushDirtyTables` の呼び手を grep して耐障害性の後退を確認 (指摘 4)。
- **読んだ差分**: `git diff 84ce5e9..HEAD` 全域 (C++ 9 ファイル / Editor 4 ファイル / `.gitattributes` / `export.py` / 文書 3 点 / spec / sub-10)。
- **見た画像**: `scratchpad/g0.png` (エディタ全景・TileSphere) / `g1c.png` (既定 15.00) / `g2c.png` (`Last shot: played` + Export WAV 有効) / `g3c.png` (0.10 でも played)。

---

## 参考: 今回も「問題なし」と確認した点 (蒸し返さないために)

- 決定論契約は無傷。`ModalImpulseCurve` の `std::pow` は出力レーン (`!ts.resim` ブロック → audio キュー) にしか入らず、`replay_verify` 13 ジョブ全緑。`ModalSound` は `kComponentNoHash` のまま、golden も 1 枚も動いていない。
- 圧縮は `k[]` にだけ掛かり、`excessImpulse` は生のまま (`kImpactMinImpulse` の門・ログ・UI はすべて生の J を見る)。ヘッダのコメントと engine_spec の記述が実装と一致している。
- `dirtyTables_` は `unordered_set` だが、走査順が変えるのは**ファイルを書く順序**だけで、各 `.msfm` の中身 (表は key 昇順の vector) には影響しない。CLAUDE.md の「unordered でバイト列を作らない」には抵触しない。
- `ModalFeatureMapAllMaskOff()` は `BuildModes` の mask 閾値ロジックの 2 本目だが、診断専用であることと「正本は BuildModes 側」であることがヘッダのコメントに明記されている (許容範囲の複製)。
- `ModalAudio.h` の `kModalImpulseExponent` コメントが「round 1 の p=0.18 は診断が逆だった」と自分の誤りを理由つきで残しているのは、このリポジトリのコメント規約 (なぜ / 踏んだ罠) に沿っている。
