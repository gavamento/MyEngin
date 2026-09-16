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

## フィードバック履歴
