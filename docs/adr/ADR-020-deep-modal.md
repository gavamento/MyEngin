# ADR-020: Deep-Modal 衝突音 — per-collision 合成 / CPU 推論 / 単一ボクセライザ / |k| 総和

- 状態: 採用 (2026-09-16、M76a〜M76i)
- 出所: `plans\DeepModal\DeepModal_Implementation_Plan.md` (ユーザーの 15 Phase 計画) と
  `plans\DeepModal\ACMMM20_ModalSound.pdf` (Jin et al., ACM MM 2020) を参考に、
  「形状・接触位置・力の向きと強さ・材質・サイズで音が変わる」衝突音を録音済み SE 無しで
  鳴らす計画 (`plans\m76-deepmodal\spec.md`)。仕様の本文は `engine_spec.md` §10.7。

## 決定 1: 再生は衝突ごとの ≤2 s クリップ合成、ストリーミングの新レーンは作らない

`AudioSystem::RegisterClip` は同 id の voice を止めるだけの軽い経路で、`SoundGenWindow::Preview`
が既に同型を使っている。衝突のたびに `ModalSynthRender` で PCM を合成し、`kModalClipSlots=32` の
ラウンドロビンへ `RegisterClip` して既存の `Play` / 3D 定位 / 遮蔽 / リバーブ経路にそのまま乗せる。
`MakeModalShotPlay` を純関数にして、合成が重くなったときにワーカーへ移せる形にしてある
(v1 はメインスレッド、`kMaxModalShotsPerTick=4` と 2 s 上限で最悪 ~10 ms のスパイクに収まる)。

却下:
- **ストリーミング再生の新レーン**。既存の再生経路 (回転プール + `Play`) をそのまま使えるのに
  レーンを増やす理由が無く、リバーブ/遮蔽/優先度の規則を 2 本目書くことになる。

## 決定 2: 推論はバックエンド抽象 + 自前 CPU 実装、ONNX/DirectML は使わない

外部推論ランタイムはこのリポジトリの「vendoring しない」方針に反する。ネットを ≤2M パラメータに
絞れば CPU でも実用的な時間に収まる (sub-09 最適化後の実測: フルサイズ 580 ms/mesh 平均)。
`ModalInferenceBackend` を抽象にして `CpuModalBackend` (AVX2 + 実行時 CPUID 検出 + スカラー縮退)
を実装し、GPU 実装 (`D3d11ModalBackend`) は差し込み口とその fixture テストだけを用意した
(未実装のまま — CPU 実装が既に目標帯 (≤0.6 s/mesh) に届いており、焼きはメッシュごと 1 回・
ワーカースレッド・`.msfm` キャッシュ (2 回目 0.45 ms/mesh 実測) なので GPU を要求する切迫が無い)。

**マルチスレッドは出力次元だけを分割し、GEMM のリダクション (入力チャンネル) は割らない。**
SIMD (FMA、丸め 1 回) とスカラー (乗算→加算、丸め 2 回) は同じ入力でも最後の 1 bit が食い違う
ので、スレッド数が変わって SIMD/スカラーの境界チャンクが動くと `.msfm` のバイト列が変わる —
チャンク境界を SIMD 幅の倍数へ量子化することで、スレッド数 1/3/4/5/8 のいずれでも同じメッシュから
同じ `.msfm` が焼けることを確認済み (M76e2)。cooked キャッシュのバイト列が実行環境に依存して
変わる、という Material の暗黙パディングの罠と同じ形の落とし穴を先回りで塞いだ形。

却下:
- **ONNX Runtime / DirectML**。外部バイナリ依存を増やし、決定論の契約 (Debug/Release/CI ビット一致)
  を自前で検証できない箱の中に入れることになる。

## 決定 3: ボクセライザは C++ の 1 本だけ、Python はそれを呼ぶだけ

学習時とランタイムのボクセル化規則が食い違うと「学習は正しいのにランタイムだけ音がおかしい」が
一番静かに壊れる形になる (ユーザー計画 Phase 2 の強調点そのもの)。C++ (`Voxelizer.h`) を唯一の
実装にし、Python (`tools/deepmodal/voxelize.py`) は `Editor.exe --modal-voxelize` を
`cmd /c` 越しに呼ぶだけ (Editor.exe は GUI サブシステムの exe — CLAUDE.md 環境の罠)。
代償はデータ生成に Release ビルドの Editor.exe が要ること。

## 決定 4: 力の結合は論文の符号付き総和ではなく `Σ|k_j|` (逸脱、明示)

論文 §3 は帯域内モードを「振幅の和」で 1 本に畳み log を取る = 符号付きの結合を代数的に表現できない
(非負にしか対応しない)。もし符号付き `k_j` をそのまま振幅へ掛けて総和を取ると、逆向きの力が
帯域を「打ち消す」ことになるが、物理的には位相が変わるだけでエネルギー自体は消えない。
Mel 圧縮で多数のモードが 1 帯域へ畳まれた後では、この符号付き総和は「たまたま同じ帯域に落ちた
モード同士が向き次第で消える」という、mask/amp の分離が想定していない現象を起こす。
`Σ|k_j|·mask·amp` (Python `compact.py` と C++ `BuildModes` の両方) に統一した。

副作用として認識している欠陥: 同じ帯域で逆向きの 2 モードが実際より過大に聞こえる。
論文の設計意図 (符号付き結合で物理的な打ち消しを表現する) を単純化した形で、聴感上の破綻
(帯域が消える) より聴感上の誇張 (帯域が大きめに鳴る) を選んだ、という判断。

## 決定 5: 音量は絶対 (ピーク正規化しない) + `tanh` ソフトクリップ

「強く落とすと大きい」はユーザー計画 Checkpoint J の主張そのもので、ピーク正規化すると
弱い衝突と強い衝突が同じ音量に均されて消えてしまう。合成後の float 加算を `|x|>0.8` でだけ
`tanh` にかけてから int16 化する — 弱い衝突は線形域のまま、強い衝突だけ紙一重で潰れる。

**耳確認 (M76h) の結果 (round 2、本学習を修正した後の値)**: `assets\deepmodal\deepmodal.dmnet`
(stage0+stage1、124 npz で学習、pooled R²=0.6277・mask_acc=99.82% — 学習設定の修正は
決定 8 参照、paramCount=1,682,448、3,368,464 B) を `--modal-demo --modal-audio-log 300
--modal-wav-dump DIR --modal-face-probe --synth-input` で実際に鳴らし、生成された実 WAV を
Python で客観的に測定した (coder は音を聞けないため、耳確認は全て peak/rms/len/spectrum の
数値で判定した。手段は `--modal-face-probe` — M76h で新設した調査専用 CLI。実衝突は常に
重力方向 = 同じ面にしか当たらないため、面ごとの違いを実測するにはこの合成し直しが必須だった):
- **強く落とすと大きい**: MetalBox の衝突列 (同一エンティティ、bounce ごとに J が減衰) で
  peak が J=102164→0.1175、…、J=860→0.0010 と**単調に**変化 (約 120 倍のダイナミック
  レンジ)。「聞こえない」「常にクリップに張り付く」のどちらの症状も確認されず、
  **`min(1, J/kImpactRefImpulse)` の上限圧縮は導入しない** — 絶対音量のまま据え置く。
- **面で音が変わる**: WoodBox の同一メッシュ・同一材質・同一 impulse (再学習後は
  4.0 N·s では 6 面とも BelowMin になったため 15.0 N·s へ引き上げた、
  `AudioSourceSystem.cpp` の `kProbeImpulse`) で 6 面を合成し直すと、-X が支配周波数
  2000/1940 Hz、+X が 1980/1820 Hz、+Z が 1800/2400 Hz と面ごとに異なるスペクトルになった
  (+Y/-Y/-Z はこの impulse でも BelowMin — 学習された特徴が面ごとに大きく非対称という
  ことの表れで、幾何的な鏡映対称に縮退していない)。
  ★**訂正 (M76i、reviewer round 1 指摘 1(d))**: この 3 枚の**絶対レベル**を当時は書いていな
  かった — `ampScale` 較正前 (プレースホルダ 1.0) の実測は peak 0.00006 = **-84.3 dBFS
  (振幅 2 LSB)** で、面で音が変わること自体は本物だが耳には実質届かない音量だった。
  較正後 (`ampScale=11478`、`kModalImpulseExponent=0.5`、決定 10 参照) の同じ 15 N·s
  プローブでは 6 面の中央値が -8.0 dBFS まで上がっている (詳細は決定 10 の表)。
- **material で減衰/周波数特性が変わる**: WoodBox (dom 1480 Hz 前後、最長 len=0.066 s)、
  MetalBox (dom 1121 Hz、最長 len=1.367 s)、GlassBox (dom 2336 Hz、最長 len=0.686 s) と
  3 材質で明確に異なる周波数・減衰時間になった。木は α=10 (質量比例減衰) が大きく即座に
  減衰、金属は **α=6・β=1e-7** (★訂正、reviewer round 1 指摘 9: 旧文は α=5・β=3e-8 で、
  これは `steel.physmat.json` の値。`--modal-demo` の MetalBox が実際に使う
  `metal.physmat.json` は α=6.0・β=1.0e-7) と両方小さく長く鳴り、ガラスは α=1 だが
  β=1e-7 が高周波数側で効いて中間の減衰になる、という physmat の初期値から予測される
  傾向と整合する (結論・減衰時間の実測値自体は元から metal の値に基づいており変わらない)。
この判断はデータが小規模自前 (stage1) の時点のものなので、ModelNet10 スケールでの
追加ユーザー確認を妨げない。実測ログは `plans\m76-deepmodal\sub-08.md` の実装メモを参照。

## 決定 6: wave の口封じは段階移行 (焼けるまでは従来の音)

`ModalSoundLibrary` の焼きは非同期 (ワーカースレッド)。焼けていないメッシュの音を即座に
消してしまうと、レベルインしてから最初の何十 ms かが無音になる。`ResolveWaveShotSound` の
先頭で `ModalSound` を持ち `muteWave` が立っていて `modalsound::IsReady(mesh)` のときだけ
従来の波の一発再生 (`PendingWaveShot`) を `mute=1` にする。初回接触 (NotReady→wave) の 1 発だけ
両方鳴りうるが、これは意図した過渡状態 (WARN で `--modal-bake` を促す) であり、恒常的な二重発音
ではない。

## 決定 7: 参照材質とサイズは自由パラメータ、`refSizeL` は 0.3 → 0.6 m へ改訂 (M76h)

参照材質 (E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7、アルミ相当) と参照サイズ `refSizeL` は、
ランタイムの σ3 = L_obj/refSizeL が吸収する自由パラメータであり、「物理的に正しい値」という
ものが無い。策定時の初期値 `refSizeL=0.3` は、サイズ漏れバグ (FEM にメッシュ実寸を渡していた)
が混ざった統計から選ばれていた。バグ修正後に全メッシュを参照サイズで解くと、帯域内モード数の
中央値が 30.5 → 14、coverage_ratio が 0.45 → 0.336 まで下がった (spec §7 の申し送り)。

固有振動数 ω ∝ sqrt(E/ρ)/h = sqrt(E/ρ)・28/L_ref という厳密なスケール則 (`fem.py` の Ke/Me が
h に対して線形/3 乗であることの帰結) を使い、`box_0` / `lshape_0` (stage0 の代表形状) で
`refSizeL ∈ {0.3, 0.6, 1.0}` を実測した:

| refSizeL | box_0 coverage_ratio | box_0 coverage_high | lshape_0 coverage_ratio | lshape_0 coverage_high |
|---|---|---|---|---|
| 0.3 | 0.281 | 0.625 | 0.281 | 0.625 |
| 0.6 | 0.562 | 1.000 | 0.469 | 0.750 |
| 1.0 | 0.656 | 1.000 | 0.656 | 1.000 |

0.6 m は高域 (coverage_high) が両サンプルとも実質飽和しつつ、1.0 m へ更に伸ばす余地
(coverage_ratio・coverage_low) を残す中間点として選んだ。stage0/stage1 の npz は
`refSizeL=0.6` で再生成し、`.dmnet` ヘッダの `refSizeL` もこの値を書く。

却下:
- **0.3 のまま据え置く**。実測が明確に coverage を改善する方向を示しており、変更コスト
  (stage0/stage1 の再生成) は本サブの範囲内で吸収できたため、据え置く理由が無かった。
- **1.0 m 以上への変更**。coverage_high は 0.6 m で既に飽和しており、それ以上大きくする効果は
  低域 (coverage_low) の穏やかな改善に限られる一方、参照サイズが実際の小物 (このゲームの
  典型的な衝突物) の代表値から離れすぎる (物理的な当てはまりの良さは σ3 が吸収するとはいえ、
  訓練データの cell ごとの励起パターン自体は参照サイズでの固有振動モードの「形」に依存する)。
  ModelNet10 規模のデータで再検討してよい。

## 決定 8: `poissonRatio` は PhysMat に保持するが `BuildModes` は読まない

論文 §4.3 は「ポアソン比の影響は複雑なので E と ρ だけスケールする」と明言しており、現行の
どのランタイムコードも ν を読まない。planner は当初「PhysMat に足さない」と裁定したが、
ユーザー判断で覆した: 「poissonRatio は FEM / 教師データ生成と reference material metadata
用。現行 Deep-Modal の runtime material scaling には使用しない。将来 Poisson 比を考慮する
モデルへ拡張可能な形で保持する」。E / ν / α / β の 4 フィールドを PhysMat へ追加し、
JSON 往復・Inspector 欄・Sanitize 範囲は全フィールドに用意するが、`ModalPostParams` /
`BuildModes` の引数からは ν を意図的に外している (読んでいないことを型で示す)。

## 決定 9: 本学習は pooled R² を必ず測る。旧既定 (epochs=100) は underfit だった (round 2、must #1)

sub-08 round 1 は本学習の品質を `run_epoch` が返す amp_mse (サンプルごとに有効 cell 数で
正規化してから重み平均する値、`compute_batch_losses`) だけで報告し、`compute_r2` (プール
定義の説明率、overfit の門では既に使っていた指標) を本学習では呼んでいなかった。
planner が 124 npz (stage0+stage1) の amp 目標のプール分散を実測すると 0.008031 で、
報告されていた `amp_mse=0.0102` はこれを**上回っていた** — 「全部に平均値を返すだけの
モデル」に負けている水準 (pooled R² を実際に計算すると **-0.2536**)。

原因は underfit: 124 サンプル/batch16 = 8 step/epoch × 100 epoch = 800 step しかなく、
しかも LR が 20 epoch ごとに半減 (下限なし) で終盤 3.1e-5 まで落ちて Adam が実質止まって
いた。overfit の門で R²=0.9237 を出したのは LBFGS (2 次法) で、本学習の Adam + 急な
減衰とは最適化の強さが桁違いだった。

修正: `train.py` に `evaluate_pooled()` (compute_r2 と同じプール定義をデータセット全体へ
適用、2 本目の式は書いていない) を追加し、本学習でも `--eval-every` ごとに pooled
mse/var/R²/mask_acc を学習曲線へ出す。既定値を `epochs=100→1500` /
`lr_halve_every=20→150` / `lr_min` (新設、既定 5e-5、下限を設ける) へ変更した。
124 npz で実測すると pooled R² は epoch 150 で 0.04、450 で 0.51、900 で 0.61、1500 で
**0.6277** (mask_acc=99.82%) まで単調に伸びた — 定数モデルより明確に良い。
`R² ≤ 0` の `.dmnet` はエンジン既定の資産としてコミットしない (spec §5 #19)。

ModelNet10 はサンプル数が 2 桁大きく、epoch あたりの step 数が変わるため、既定の
epoch 数で十分かは実測が要る。**閾値は固定しない** — 頭打ちならその学習曲線を添えて
報告し、planner が裁定する (README「本学習の pooled R²」参照)。

## 決定 10: 衝撃力 → 音量の圧縮カーブと較正 (M76i、reviewer round 1 指摘 1)

`ampScale` は M76h までプレースホルダ 1.0 のまま出荷されており、`BuildModes` の振幅は
力積に厳密に線形だった。reviewer round 1 の実測: peak/J は材質ごとにほぼ一定
(Metal 1.150e-6、Wood 2.41e-6、Glass 2.79e-6 [1/(N·s)])、int16 の 1 LSB (3.05e-5) に届く
下限が J≈11〜27 N·s、**1 kg を 0.5 m 落とす (J≈3) は全サンプル 0**。`--modal-demo` が
鳴っていたのは箱が 0.7〜7.9 トンあった (J=161〜102164) からに過ぎない。**線形のままでは
両立しない** — J≈3 を可聴にする線形スケールは `--modal-demo` を約 1000 倍のクリップへ
叩き込み、逆に demo が歪まない線形スケールでは J≈3 が無音になる。

採用した対処:

1. **圧縮カーブ**: `CollectModalImpacts` が `k[]` を組む前に、生の超過力積 `J` を
   `C(J) = kImpactRefImpulse · (J / kImpactRefImpulse)^p` へ通す (`ModalAudio.h`)。
   `kImpactRefImpulse` (=6.0、AcousticGrid.h の既存定数「倍率 1.0 に達する力積」) を
   そのまま基準に流用し、基準を 2 つ作らない。**`C(J)` は J=kImpactRefImpulse で
   `p` に依らず恒等** (`(1)^p=1`) — この不変性が下の較正を p の選択から独立にしている。
   `excessImpulse` フィールドは生の J のまま保つ (ログ/`kImpactMinImpulse` の擦り判定は
   圧縮前の値で行う。圧縮は k の大きさにだけ効く)。
   Inspector の面打ちプレビュー (`FireModalPreviewFace`) と `--modal-face-probe` も
   同じ `ModalImpulseCurve()` を通す — 3 経路が別の式を持つと較正の前提が経路ごとに
   ずれる。
2. **較正アンカーの再定義**: 旧仕様の「J=1 N·s で -12dBFS」を撤回し、
   **「J = kImpactRefImpulse (6.0) で中央値ピーク -12dBFS ± 3dB」**へ変更した。
   J=1 には物理的根拠が無く (「何かを 1 回落とす」の代表値ではない)、線形則と組み合わせると
   全域クリップを含意していた。
3. **`p` は「demo が歪まない値」ではなく「現実的な力積域での聴感差」で決める**:
   ★round 1 の反省 — 最初に `p=0.18` を選んだ根拠は「p=0.5 だと `--modal-demo` の実 J 域
   (161〜102164、旧 `useDensity=true` な 1 m³ 剛体の非現実的な質量) が軒並み
   ±0.8dBFS 以内までフルスケールへ張り付く」だったが、**これは診断が逆だった** — 張り付きの
   原因は demo 側の非現実的な質量であって、圧縮カーブの問題ではない。demo に指数を
   決めさせてはいけない。正しい基準は**エンジンが既に「現実的」として扱っている力積域**
   (`kImpactMinImpulse=0.35` 〜 「本気の一撃」100 N・s 程度) での聴感差で、
   既存の**波レーン** (`acoustic::ImpactGain = min(1, J/6)`、AcousticGrid.h) が
   J=0.35→6 という**モーダルより狭い**範囲に **24.7 dB** を割り当てていることを踏まえると、
   モーダルレーンだけ桁違いに平坦だと同じ衝突なのに「強く当てた」感が波と食い違う
   (spec §1 の Checkpoint J が実質不成立になる)。`C(J)` が J=`kImpactRefImpulse` で
   `p` に依らず恒等であることから、0.35→100 の音量差は
   `20·p·log10(100/0.35) ≈ 49.1·p` [dB] — **p=0.5 で 24.6 dB** (波レーンとほぼ同値)、
   p=0.4 で 19.6 dB (下限 20dB をわずかに割る)、却下した p=0.18 では 8.8 dB しか出ない。
   **p=0.5 を確定値とする** (= 出発値そのもの。アンカーは p 非依存なので `ampScale=11478`
   は変更不要 — 実測でも p=0.5・p=0.18 のどちらでも J=6 の中央値ピークは完全に同じ
   -12.0dBFS だった)。
4. **demo の質量を現実的に直す** (原因への対処、指数選びとは別の修正):
   `BuildModalShowcaseScene` の 4 物体は旧 `useDensity=true` (1 m³ の中実剛体、
   金属で 7,850 kg 相当) をやめ、`RigidbodyComponent::mass` を小道具サイズの値へ
   直接指定した (Wood 2kg / Metal 4kg / Glass 1kg / Sphere 1.5kg。見た目のサイズは
   変えていない)。結果、実バウンドの J は **0.438〜52.058 N·s** (旧: 161〜102164) と
   現実域に収まり、ピークは **-33.4〜-5.5 dBFS** — フルスケールへ張り付く発は 0 発になった
   (球の最初の 1 発が最大 -5.5dBFS で、それでもソフトクリップの手前)。
5. **測定表** (`--modal-wav-dump` の実 PCM、p=0.5・`ampScale=11478` (変更なし)。
   ログの `peakDb` は無音を -80dBFS へ丸めるため使わない):

   | J [N·s] | 由来 | ピーク¹ |
   |---|---|---|
   | 0.35 (`kImpactMinImpulse`) | `--modal-face-probe` 6 面 | -24.3 dBFS |
   | 1 | 同 | -19.8 dBFS |
   | 3 | 同 | -15.0 dBFS |
   | 6 (`kImpactRefImpulse`、アンカー) | 同 | **-12.0 dBFS** |
   | 30 | 同 | -5.0 dBFS |
   | 100 | 同 | -0.3 dBFS |
   | 0.438〜52.058 (実バウンド、Wood/Metal/Glass/Sphere) | `--modal-demo` の 20 発 | -33.4〜-5.5 dBFS、各系列内で単調 |

   ¹ **★訂正 (reviewer round 2 指摘 2)**: 6 面を昇順に並べた**上から 4 番目** (0 始まり index 3、
   `statistics.median_high`) であって、教科書的な中央値 (index 2 と 3 の平均) ではない —
   後者だと各行が一律 -1.78dB 低くなる (アンカーは -13.8dBFS)。アンカーはどちらの定義でも
   ±3dB の許容に収まるので**較正のやり直しは不要** — 再現性のための表記の問題。

   不変条件 (J=3 が全サンプル 0 でなく可聴 / peak が J に単調 / 現実域 (0.35〜100) の
   音量差 ≥20dB (実測 24.0dB) / demo が歪み切らない / 全 impact が Played) をすべて満たす。
   ★demo の `impacts`/`played` は **20/20** (質量修正で bounce のタイミングが変わり、
   球を足した直後の 21/21 からさらに変わった — 決定 12 参照。不変条件の本質は
   「登録された impact が全て鳴る」ことで、具体的な総数ではない)。
6. **再学習は不要**: `ampScale` は `.dmnet` ヘッダのみのフィールドで、`.msfm` の再利用を
   決める `weightsHash` は重み+バイアス blob だけから計算される (ヘッダ組み立て前)。
   `p` を 0.18→0.5 へ変えても `ampScale` は 11478 のまま (アンカー式が p 非依存のため
   再計算不要)、`.dmnet` 自体を書き直していないので `weightsHash` は最初の較正から
   一度も変わっていない (`5553600061897636384`)。`--modal-bake` の 2 回目が全件
   キャッシュヒットする (`bakeMsAvg` が数百 ms → 1ms 未満)ことも確認した。

却下:
- **`J=1` のままアンカーを保つ**: 物理的根拠が無く、線形則前提の値だったため。
- **p=0.18 (demo が歪まないように選んだ値)**: 現実的な力積域 (0.35〜100) の音量差が
  8.8dB しか出ず、波レーン (24.7dB@0.35→6) と桁違いに平坦になる。「demo に指数を
  決めさせない」の裁定そのもの (round 1 の誤りを round 2 で訂正)。

## 決定 11: 常時無音メッシュの可視化 (M76i、reviewer round 1 指摘 1(f)・25)

`ampScale` は `BuildModes` の mask ゲートの**後**に掛かる (unpack → mask 閾値判定 →
**ここで初めて** `a *= hdr.ampScale`)。したがって全 cell・全帯域で mask が閾値以下の
メッシュは、どんな `ampScale` や力積でも `a=0` のままで鳴らない。ステージ 1 の
`.dmnet` (124 npz で学習) では **382 枚中 35 枚 (9.2%)** がこの状態 — ModelNet10 が
本命の対処 (学習データの汎化不足) であり、このサブでは**モデルは作り直さない**。
代わりに `--modal-bake` の集計に `silent=N` を足し、該当メッシュの行にも `silent`
タグを出す (`ModalFeatureMapAllMaskOff()`、`ModalFeatureMap.h`。閾値の式は
`BuildModes` 手順 3 と同一、複製した診断用ロジックなので変えるときは両方合わせる)。
`ModalSound.maskThreshold` を下げれば境界上のメッシュの一部は救えるが、logit が
本当に閾値の遥か下にあるメッシュは救えない — **緩和ノブであって修正ではない**。

## 決定 12: `--modal-demo` に形状差を見せる 4 個目 (球) を足す (M76i、reviewer round 1 指摘 7)

`BuildModalShowcaseScene` は WoodBox/MetalBox/GlassBox の 3 個とも `Cube()` = 同一
メッシュだったため、「形状で音が変わる」機構自体は `.msfm` 382 枚の解析 (mask-on 率
0〜88%、cell ごとの mask パターン 1〜347 種) で確認できても、demo では見せられなかった。
`builtin://sphere` を使う `TileSphere` を**生成順の末尾**に 1 個足した (末尾なら既存の
replay/golden に影響しない、このリポジトリの流儀)。球を足した直後 (質量修正前、
旧 `useDensity=true`) は `impacts`/`played` が 20/20 から 21/21 へ増えていたが、
決定 10 の質量修正 (`useDensity` をやめて現実的な `mass` を直接指定) でバウンドの
タイミング自体が変わり、最終的な実測は **20/20** (Wood 4 発 + Metal 9 発 + Glass 6 発 +
Sphere 1 発) に落ち着いた。**この具体的な総数は不変条件ではない** — spec §5 #24 が
実際に要求しているのは「登録された impact が全て Played で鳴る」ことで、球の追加も
質量修正もその総数を変える権利は最初から持っている (どちらも意図した仕様変更・是正で
あり退行ではない)。

## 決定 13: モデル階層では子孫メッシュを発音元ローカルへ合成して焼く (M76j)

FBX / glTF は `ルート (MeshRenderer 無し) → ノード → partN` の階層で置かれ、Rigidbody /
Collider / ModalSound は自然にルートへ付く。M76i までの `ResolveModalMesh` は
「`ModalSound.mesh` か**同一エンティティ**の MeshRenderer」しか見なかったので、この構成では
空 ID になり `CollectModalImpacts` が接触を**ログも出さずに**捨てていた (Inspector も「未登録」の
まま)。`mesh` 欄に子のメッシュを手で入れる回避も、接触点 (ルートのローカル) と特徴マップ
(ノードのローカル) の空間がずれ、`sizeL` にノードのスケールが入らず、マルチマテリアルは
part 1 枚分の形しか焼けないので成り立たない。

- **解決規則は `ResolveModalMesh` の 1 本のまま**拡張した: 子孫に MeshRenderer があれば
  自分 + 子孫のメッシュを発音元ローカルへ変換して連結し、それを焼く。子孫が無ければ
  従来の単体経路で、ID もログもバイト一致 (`--modal-demo` の `[modal] t=` 20 行で実測)。
- **除外**: 自前の ModalSound を持つ子孫 (別の発音元) / Rigidbody を持つ子孫 (独立に動く) /
  `Active.enabled == 0` の子孫。
- **相対行列は LocalTransform の連鎖**から組む。WorldMatrix の逆行列から出すと丸めの揺れで
  合成キー (整列したパーツ列 `(mesh id, 行列の生ビット)` のハッシュ) が毎フレーム変わり、
  焼き直しが止まらない。
- **検証**: スケール 0.5 の builtin cube を「単体」と「MR 無しルート + スケール 0.5 の子」で
  落とし、同じ cell・同じモード (f0/f1 = 1651.6 / 3607.9 Hz)・同じ J で同じ peak になることを
  実測した。box.fbx / cubes_pivot.fbx のルートに ModalSound を付けた構成は、修正前 0 発 →
  修正後は全発 Played。
- **.msfm**: 合成は先頭パーツのモデルの表に `guid://<16hex>#modal#<hash>` で相乗りする
  (形式不変)。GUID の無いパーツだけの合成はディスクに持たない。`--modal-bake` は合成を
  事前に焼かない (シーン上の配置に依存するため)。
- **既知の限界**: スキン付きメッシュはバインド姿勢で合成される。

## データ段階の門と stage1 の範囲

学習パイプラインは「Primitive → 小規模自前 (stage1) → ModelNet10 → ModelNet40」の順で
データを拡張し、**`train.py --overfit 16 --epochs 300` が mask acc > 99% かつ amp の
説明率 R² ≥ 0.90 を満たすまで次段階へ進まない** (門は sub-04 round 2 で R²=0.9237 を実測して
確定)。本 ADR が確定した時点 (M76h) では、stage1 (このリポジトリの `assets\models` から
選んだ 87 サブメッシュ、cap 超の 1 本を除く 86 npz) までを端から端まで通し、
`assets\deepmodal\deepmodal.dmnet` をコミットしている。ModelNet10/40 は
`tools\deepmodal\README.md` の手順でユーザーが別途実行する。
