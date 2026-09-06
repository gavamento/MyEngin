# ADR-017: 音響の波面を耳へ出す — リスナー場は「3 本目の Dial」、整形は純関数 1 本、sim 状態はゼロ

- 状態: 採用 (2026-09-06、M68a〜M68c)
- 出所: ユーザーの元計画 `plans/m68-acoustic-audio/plan-original.md` (判断 1〜7) と、それを基点
  コミット `8e4272e` のコードと突き合わせて穴を埋めた `plans/m68-acoustic-audio/spec.md`
  (§2 の疑い S1〜S24 が根拠の一次情報)。仕様の正本は spec.md、本 ADR は**決定と却下理由と実測値**
  だけを残す。M65 (音響伝播そのもの) に ADR が無いので、その土台の決定も「決定 0」として一緒に拾う。
- 関連: **ADR-004** (リプレイ一貫性 = 「sim 状態を増やさない」の判定装置)、
  **ADR-014** (CI とピクセル回帰 = golden 22 枚が「絵を 1 画素も変えない」を機械証明する枠組み)、
  **ADR-008** (決定論 RNG。一発再生が使うのは `AudioSourceSystem` 専用の Pcg32 で、
  `world.Rng()` にも `audioScriptRng` にも触れない)。

## 決定

### 決定 0 (M65 の土台。ここで初めて ADR に残す)

音の伝播は **26 近傍・Borgefors 重み `<11,16,19>` の整数チャンファ距離を Dial のアルゴリズムで
1 tick 1 リングずつ**広げる。**整数であることは最適化ではなく決定論の主張**で、順序を決める量が
すべて整数だから「どの順に訪れたか」が構成や機種で揺れない (float なら 1 ULP の差が訪問順を入れ替え、
親リンク = 到来方向が変わる)。**sim 状態は波スロット表だけ**で、占有グリッド・波ごとの距離場・
ナビの流れ場・残光ボリュームはすべて派生値 = ハッシュにもスナップショットにも載らない。これが成立
するのは「到達エネルギーが整数チャンファ距離の純関数」= 伝播中に材質で減衰させないからで、途中減衰を
入れた瞬間にエネルギーが経路依存になり、セル配列そのものが sim 状態に落ちる。

### 決定 1〜9 (M68)

1. **リスナー場は同じ Dial の「3 本目の写し」** — 波 (`AcousticField`) / ナビ (`AcousticNav`) に続く
   3 本目として `AcousticProbe` を新設し、**リスナーから外向きに一気に完走**させる (波と違って
   1 tick 1 リングではない)。近傍表・重み・「閉セルは訪れない / 中間セルは見ない」規則・
   `parentDir[ni] = OppositeNeighbor(i)` は既存と同一。`AdvanceWaveOneRing` には触らない。
2. **整形は純関数 `ShapeAcousticSpatial` 1 本**。per-voice 更新と一発再生 (波) の**両方がこれを呼ぶ** —
   `MakeSourcePlay` の「規則は必ず 1 本」と同型。出力は 4 クラス:
   `Direct` (直線からの回り込み量が 1 セル未満) / `Detour` (角を回った) /
   `Occluded` (未到達・経路上限超え・密閉) / `Bypass` (probe が無効 = リスナーがグリッド外)。
3. **`Detour` は減衰させずに音源を動かす** — voice の位置を `L + dir · dPath` (仮想発音位置) に置く。
   `dir` は親鎖の**最後の 1 歩**、`dPath` はチャンファ経路長。壁を知らない 3D パンナーが、
   それだけで「戸口の方向から」鳴らす。回折ローパスは超過長 `dPath − dLine` が駆動する。
   仮想位置のあいだ **`dopplerScale = 0`** (位置がセル単位で跳ぶので、そこから導いた速度は
   ドップラーではなくチャープになる)。
4. **波 → 音量は「振幅 = √エネルギー」で扱い、既定の減衰カーブは Logarithmic (rolloff 0)**。
   固定するのはカーブではなく関係式 **`gain² · amplitude == EnergyAt`** (selftest T13)。
   到達範囲は波そのもの: `maxDistance = maxRing · cellSize` で、`RolloffGain` は全カーブで
   `d ≥ maxD` に厳密な 0 を返す = 「波が届く所でだけ聞こえる」が調整値ではなく構造で成立する。
5. **部屋の残響は 2 プリセットの I3DL2 連続補間** (ユーザー決定 U2)。局所の開放度
   (`roomProbeM` 内の到達セル数 / 自由空間セル数) を 2 アンカーで smoothstep し、
   `LerpReverbParams` で 13 フィールドを補間する。**選択は `ApplyReverbParams()` の 1 箇所**で、
   override は資産値に**書き戻さない** — `reverbPreset_` もミキサー窓の combo も `.mixer.json` も
   資産の値のままで、ホットリロード後は override が自動で再適用される。
6. **鳴る波は `PendingWaveShot` (POD) で受け渡し、push は tick 側・drain は `AudioSourceSystem::Update`**。
   push は `TickRunner` の `if (!ts.resim)` ブロック内、drain は `IsReady/IsSuspended` の
   early return より**前**にキューを空にしてから (溜めると検証解除後に一斉に鳴る)。
7. **`AcousticAudioComponent` は TypeId 50 / `kComponentNoHash`、sim 状態はゼロ**。probe・平滑化状態・
   shot キューは `SourceState` と同じ側テーブル。`.rep` 版 / snapshot 版 11 / `MYE_API_VERSION` v15 は
   据え置き。`AcousticField.h/.cpp` への diff は**コメント 1 行だけ**。
8. **一発再生の整形は Play 時 1 回・状態なし** (ユーザー決定 U6)。クリップは ≤ 0.45 s で、その間の
   リスナー移動は 2 セル未満。
9. **決定的撮影モード (`--screenshot` かつ `--shot-every` 無し) は、生デバイス由来のレーン 0 の
   マウスデルタを 0 にする** (M68c)。合成入力 (`--synth-input`) と記録入力 (`.rep`) は**この後**に
   レーンごと置換されるので 1 カウントも殺さない。M68 の機能ではなく、golden の安定性の修正。

## 理由

### なぜ場を共通化せず「3 本目の写し」なのか

`AcousticField` の波と `AcousticNav` の流れ場は、既に同じ Dial を**別々に**実装している (M65a/M65f)。
リスナー場でそれを共通化しようとすると、テンプレート化した Dial の中に「1 リングずつ進む / 一気に
完走する」「セル単位のグリッド / `navCellRatio` で間引いたグリッド」「波は非 const で shell を書く」の
3 軸の差分が入り込み、**波の内側 (= sim 状態を生む唯一の場所) を触ることになる**。M68 の受け入れ条件は
「replay 7 ペアが無風」で、その主張が一番強いのは「波のコードに 1 行も触っていない」ときだった。

代償は同型のコードが 3 本あること。埋め合わせは**規則を 1 か所に置き、写しであることを機械で固定する**
側で払っている: 近傍表・重み・`OppositeNeighbor` の規約は `acoustic::kNeighbors` にしかなく、selftest は
**T2** (同じ原点なら波と probe の `dist` / `parentDir` が全セルで一致) / **T3** (`probe(L).dist[S] ==
wave(S).DistanceAt(L)` = 対称性) / **T11** (自由空間の Dial が閉形式
`11(a−b) + 16(b−c) + 19c` と一致) の 3 本で「3 本目が同じ距離を出す」を縛っている。

### なぜ振幅は √エネルギーなのか (逆二乗をそのまま volume に掛けない)

`RolloffGain(2, ...)` も残光も扱っているのは**エネルギー**で、XAudio2 の volume は**振幅**である。
逆二乗をそのまま振幅に掛けると 10 m で `(0.5/10)² = 0.0025` = **−52 dB** = 実質無音になる。
残光が同じ理由で `γ = 1/4` を掛けている (`kGlowGamma` の注記) のと同根の罠で、元計画の
「hum は minDistance 0.5 / maxDistance 30 の inverse」は、廊下 (経路 15 m) で −59 dB = 企画の
「戸口の方向から次第に開いて聞こえる」が絶対に成立しない値だった。

そこで**カーブではなく関係式を固定した**: `gain² · amplitude == EnergyAt`。既定は Logarithmic だが、
`AcousticAudio.waveRolloff` (0/1/2) で耳による A/B ができる。

### なぜ `AudioListener` を Watcher の子エンティティに置くのか

元計画は「まず本体に直付けし、アーキタイプが変わったら子へ」と 1 往復を織り込んでいた。実コードを
読むと `World::ApplySetParent` は親子とも `HierarchyComponent` を**無検査で deref** している =
全エンティティが生成時から持っている (`World.cpp:17` の既定 4 種)。つまり**子を足しても親の
アーキタイプは 1 ビットも変わらない**ので、最初から子 (`Watcher Ears`、MeshRenderer 無し、ローカル原点)
に置いた。Watcher の yaw は本体に載る (`self.SetLocalRotation`) ので、向きはそのまま継がれる。

### なぜ到来方向は「最後の 1 歩」なのか (平均は却下、U5)

元計画は「最後の 2〜3 歩の平均で和らげる (任意)」を挙げていた。**却下**した理由は 2 つ:
仮想発音位置は既に `smoothTicks` の指数平滑を通っているのでセル遷移の跳びは補間済みであること、
そして角を曲がった直後の 1〜2 セルでは平均ベクトルが**壁の中を指す**こと。1 歩に決めると
selftest T7 が「到来方向 = L 字の腕の向き」と**厳密に**書ける (平均だと「± 1 歩の許容」に緩む)。

### なぜ一発再生は追従整形しないのか (U6)

追従には「(voice ハンドル, 原点セル, 生成 tick) のリング」= `SourceState` に次ぐ**3 つ目の側テーブル**と
生存確認が要る。得られるものはクリップ長 ≤ 0.45 s のあいだのリスナー移動 ≤ 0.6 m (< 2 セル) 分の補正で、
既存の `PlayAtPoint` も Play 時 1 回である。**割に合わない**と判断した。

### なぜ reverb の override を `ApplyReverbParams()` の 1 箇所で選ぶのか

素直に書くと `SetReverbOverride` が直接 APO を叩くことになるが、そうすると `.mixer.json` の
ホットリロード (`ApplyMixer → RebuildBusGraphNow → BuildBusGraph` 末尾で `ApplyReverbParams`) が
**黙って資産のプリセットへ戻してしまう**。選択を `ApplyReverbParams()` に集約すると、
「override があればそれ、無ければ `kReverbPresets[reverbPreset_]`」が**全経路で**成立する。

同時に `reverbPreset_` に書き戻さないことも決めた。書き戻すとミキサー窓の combo が音響の値で
勝手に動き、ユーザーが保存した瞬間に**資産が音響の一時値で上書きされる**。代わりに
「(音響が上書き中)」の注記を combo の隣に出す。

### なぜ波 → 一発再生の push は tick 側で、drain はフレーム側なのか

フレーム単位で `Waves()` を舐めると、**`kMaxTicksPerFrame = 5` のため 4 tick で消える衝撃波を
取りこぼす**。逆に drain まで tick 側でやると、その tick に更新されていない probe で整形することになる。
そこで **push = tick (`bornTick == ctx.tickIndex` の波を拾う)、drain = `Update` (probe 更新の後)** に
割った。`++ctx.tickIndex` が tick の末尾にあることが `bornTick == tickIndex` の根拠。
push を `if (!ts.resim)` ブロックに入れてあるので、ロールバックやタイムトラベルの再シムで
**同じ足音が二度鳴ることはない** (`.rep` へ入力を書かないのと同じ場所・同じ理由)。

### なぜ開放度がこの指標で、アンカーが 0.30 / 0.8 なのか

開放度 = `roomProbeM` (既定 6 m) 内の**到達セル数 / 自由空間セル数** (分母は占有を無視した閉形式
チャンファ球 ∩ グリッド)。デモの間取りを実際に焼いて測った 9 点:

| 位置 | 開放度 | 位置 | 開放度 |
|---|---|---|---|
| 部屋 A の隅 (Watcher の開始点) | 0.468 | 縦廊下 (9,5) / (9,6) | 0.404 / 0.529 |
| 部屋 A の中央 | 0.668 | 部屋 B の戸口 | 0.607 |
| 横廊下 西 / 中 / 東 | 0.496 / 0.357 / 0.287 | 部屋 B の中央 (hum) | 0.800 |

`openLarge = 0.8` は部屋 B がちょうど上端に着く値。`openSmall` は元計画の 0.2 では**廊下の西端 (0.50) と
縦廊下 (0.40〜0.53) が部屋 A (0.47) と区別できない**ので **0.30** にした (廊下 東 0.00 / 中 0.09 /
西 0.24、部屋 A 中央 0.83、部屋 B 1.00)。

**この指標は「部屋の隅」と「廊下の端」を区別できない** (廊下西端 0.50 > 部屋 A 隅 0.47) — 局所の
自由体積しか見ておらず、囲まれ方の形を見ていないため。v1 の割り切りとして受け入れ、下端は
「廊下の中央付近が 0 に落ちる」ところへ置いた。

### なぜ `probeMaxRing` の既定が 96 (= 48 m) なのか

経路上限を超えると分類が一段 `Occluded` (実位置 × `occludedGain`) へ落ちるので、そこで −10 dB 級の段が
出る。デモの最長経路 (部屋 A の隅 → 部屋 B の奥 ≈ 40 m) を包めば段は起きない。**Dial のコストは
リング数ではなくセル数で決まる** (デモの 16224 セルでは箱が常にグリッド全体) ので、リングを増やすのは
ただ。上限を超えるシーンでは `smoothTicks` が段を 1 段のランプに均す (仕様上許容)。

### なぜ `minWaveVolume` の既定が 0.10 なのか

`WatcherFpsCamera` の呼吸が `breathLoudness = 0.07`、carpet の `acousticLoudness` が 0.12。
0.07 < **0.10** < 0.12 に置くと「carpet 以上の足音は鳴り、呼吸は鳴らない」が既定で成立する。
元計画の「呼吸は `minWaveVolume` で無音」という記述は、既定 0.02 のままでは成立していなかった。

### なぜ決定的撮影モードで生マウスデルタを 0 にするのか (M68c、却下 2 案)

M68a で `shot_verify` が 1 回だけ「1 shot(s) differ」を出し、M68b で正体を捕まえた:
`acoustic_deferred` が **maxDiff 125 / 520 px、worst pixel (236,444) = 部屋 A の隅に立つ Watcher の箱**。
`WatcherFpsCamera` が v15 の `GetMouseDelta` (= `WM_INPUT` の生カウント) を yaw に積分し、
**MeshRenderer 付きの箱を回している**。`shot_verify` は acoustic の 2 枚に `--synth-input` を渡さない
(golden はその画角) ので、**撮影中の 123 フレームのあいだに机のマウスが動くと golden が割れる**。
M65g から埋まっていた性質で、M68 のコードはこの run で 1 行も走らない (`--no-audio` = `IsReady()` false)。

`--screenshot` は既に「dt を固定して frame == tick にする」「非同期テクスチャを drain する」という
**撮影モードの決定化**を持っている。生デバイスのマウスも同じ列に並ぶ machine-dependent な入力なので、
そこに 1 行足した。効かせる範囲は最小に絞っている: 置くのは**生デバイスを読んだ直後**で、
合成入力・.rep・ネットの置換より前 (= 合成入力と記録入力のデルタは 1 カウントも殺さない。
replay 7 ペア目は記録側に `--synth-input` を渡して**視点角を被覆している**ので、ここを殺すと検査が消える)。
キーボードとマウス**位置**は触らない (位置に依存する golden が無いことを確認していないため)。

却下した案: **(b) M65 追補として別コミット** — 内容は同じで司会の段取りが増えるだけ。
**(c) 放置** — 「acoustic の 1 枚だけ時々赤い」が恒常の雑音になり、golden 22 枚という検査の信頼が落ちる。

## 帰結

### 決定論と後方互換

- **出力レーン専用**。`AcousticAudioComponent` は `kComponentNoHash` (ハッシュから丸ごとスキップ)、
  probe / 平滑化状態 / shot キューは ECS の外の側テーブル、`world.Rng()` には触れない。
  `.rep` 版 / `kSimSnapshotVersion` 11 / `MYE_API_VERSION` v15 / `Interop.cs` に diff は無い。
- **`src/Engine/Engine/Acoustic/` への diff は `AcousticField.h` のコメント 1 行だけ** (`bornTick` に
  「M68b の出力レーンがこの tick に生まれた波を拾う鍵」を書き足しただけ)。これが「波の内側を
  触っていない」の機械確認 (`git diff 8e4272e --stat -- src/Engine/Engine/Acoustic`)。
- **replay 7 ペアと golden 22 枚は M68a/b/c の全サブで無風**。前者は sim 状態が増えていないこと、
  後者は絵を 1 画素も変えていないことの機械証明。
- ゲートは 5 つ: `--no-audio` / デバイス無し (`IsReady()` で即 return = ヘッドレスはゼロコスト) /
  記録・検証・タイムトラベル (`IsSuspended()`。キューは return より前に clear) / 再シム (push しない) /
  有効な `AcousticAudio` が無い (全 Bypass + override Clear)。ネットロックステップでは**止めない**
  (出力レーン)。
- 副産物として **M45 からの既存不具合を 1 つ直した**: 起動時 `ApplyMixer` が保留したバスグラフ再構築が
  フレーム 0 で走り、その直前に `playOnAwake` で鳴った voice を `DestroyAllSourceVoices` が全部殺していた
  (`started` が立ったままなのでループ音が二度と復活しない)。メインループ直前に `audioSystem.Update(0.0f)`
  を 1 回。`--acoustic-demo` に `AudioSource` が 0 個だったので M45 から誰も踏んでいなかった。

### コスト (実測)

| 項目 | 実測 |
|---|---|
| probe の再構築 (16224 セル = デモのグリッド全体) | **Debug 9.3 ms / Release 0.65 ms** per 回 |
| 再構築の頻度 (合成入力の歩行、600 tick) | 66 回 = 約 6.6 回/秒 (契機はセル変化・署名変化・グリッド変化・`probeMaxRing` 変化) |
| 600 tick の整形 | shaped 652 (voice) + shots 51、クラス内訳 `D/T/O/B = 23/629/0/0` |
| 一発再生の内訳 | Direct 23 / Detour 28、tone 0/1/2/3 = 7/33/4/7 (音色 4 本すべてが鳴っている)、`playFailed = 0` |
| `--no-audio` (CI) | ゼロ (`Update` が `IsReady()` で return するので probe も整形も log も走らない) |

Debug の 9.3 ms は 1 フレーム (16.7 ms) に 1 回のヒッチとして見える。v1 で許容した (Release は無視できる)。
詰めるなら `assign` の毎回確保をやめてバケットの capacity を使い回す。

`--acoustic-audio-log 600` の 2 run は `[acaudio] t=` 行 **650 行がバイト一致** (summary の
`probeMsAvg` を除く) = 出力レーンの計算が合成入力の下で決定的であることの実測。

### 既知の制限 (v1)

- **伝播中の材質吸収は無い**。材質は波を出す瞬間の振幅と到達リングだけを決める (決定 0 の帰結で、
  途中減衰を入れるとセル配列が sim 状態に落ちる)。
- **動く物は遮蔽しない**。占有は静的コライダのベイクだけ。
- **リスナーは 1 人、部屋ごとの複数 reverb バスも無い**。override は 1 本のグローバル reverb。
- **開放度は部屋の隅と廊下の端を区別できない** (上記の実測表)。
- **経路上限 (`probeMaxRing`) を超えると `Detour → Occluded` の段が出る**。デモでは起きないが、
  大きなシーンでは `smoothTicks` のランプで均されるだけ。滑らかにするなら「上限付近で gain を
  `RolloffGain(dPathMax)` へ寄せる」を足す。
- **tick 1 だけ場が「壁なし」で焼かれる** (M65a から継承)。`AcousticField::Sync` の占有ベイクは静的
  コライダの WorldMatrix を見るが、シーン構築直後の最初の tick は行列が確定していない。tick 2 で署名が
  変わって正常化するので、tick 1 に立つ波が無い限り実害は無い。直す場所は sim 側 (「占有ベイクを最初の
  transform 更新の後にする」) なので M65 の追補候補として残した。
- **壁越しの hum は `Detour` であって `Occluded` ではない**。デモの L 字は開いているので経路が通る —
  `Occluded` になるのは密閉と経路上限超えだけ (ログを読むときの注意)。
- **「実際に空気が震えた音」だけは機械で検査できない**。`playFailed` (Play が無効ハンドルを返した回数) が
  「51 回 Play を呼んだ」と「51 回 voice が立った」の差を埋める唯一の口で、その先は耳
  (`docs/test_checklists.md` の M68 節)。
- 調整値 (`bendFullM` / `lpfFloor` / `occludedGain` / `detourWet` / `waveVolume` / 残響のアンカー) は
  spec の計算に基づく**初期値であって最終値ではない**。`AcousticAudio` は NoHash なので実行中に
  Inspector で触れる (replay に影響しない)。確定値はユーザーが耳で決めて別コミットで焼く。
