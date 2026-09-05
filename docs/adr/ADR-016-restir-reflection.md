# ADR-016: RT 反射の ReSTIR は reconnection 方式、クラスはヒット側 `RtInstance` に置く

- 状態: 採用 (2026-09-05、M67a〜M67g)
- 出所: ユーザーの元計画 `plans/m67-restir-reflection/plan-original.md` と、それをコードと
  突き合わせて穴を埋めた `plans/m67-restir-reflection/spec.md` (§2 の疑い S1〜S15 が根拠の一次情報)。
  仕様の正本は spec.md、本 ADR は**決定と却下理由と実測値**だけを残す。
- 関連: **ADR-009** (ハイブリッドパストレーシング = このレーンの土台)、
  **ADR-014** (CI とピクセル回帰 = 「off はビット一致」を機械証明する枠組み)、
  **ADR-008** (GPU 乱数を sim ハッシュから除外する免除。ReSTIR も同じ免除の中にいる)。

## 決定

RT 反射レーン (M46h、GGX VNDF の 1spp → SVGF) に **ReSTIR (時空間サンプル再利用)** を足す。
レイ数は据え置きで、再利用によって実効サンプル数を上げる。あわせて
**ReflectionClass = 反射に映る側 (ヒットした物体) の 5 段の品質クラス**を導入し、
再利用の強さをオブジェクト種別ごとに変える。

1. **Reservoir は reconnection 方式** — 保存するのは方向ではなく**ヒット点そのもの** `xs`
   (スカイヒットは方向ベクトル + `ns = 0` のセンチネル)。受け側の画素は
   `L = normalize(xs − P)` を作り直し、**自分の V / N / α で target function を評価し直す**。
2. **ReflectionClass の出所は `Material::reflectionClass` (int32、既定 4)** で、
   `RtScene::Update` が `RtInstance.reflectionClass` (旧 `pad0`) へ写し、HLSL は
   `gRtInstances[hit.inst]` から**ヒット点で**引く。G-Buffer には 1 ビットも触らない。
3. **保存する重みは `W = wSum / (M · p̂_q(y))`** (wSum ではない)。
4. **temporal も厳密な Jacobian を掛ける** — reservoir に受け側ワールド座標 `rpos` を持ち、
   `J = (cosθ_to / cosθ_from) · (d_from² / d_to²)` を spatial と同じ関数で評価する
   (**ユーザー判断 U4。planner の「J = 1 近似」案は却下**。下の「理由」参照)。
5. **パスは 2 本** — `rt_refl.cs.hlsl` (トレース + 初期 reservoir + temporal 統合) と
   `rt_refl_restir_spatial.cs.hlsl` (spatial 統合 + resolve)。temporal 専用の 3 本目は作らない。
6. **spatial は reservoir を書き戻さない** — 2 組は `RtHistory` と同じ ping-pong で、
   履歴に載るのは `rt_refl` の出力 (= temporal の結果) だけ。
7. **spatial のタップ半径は 2 段で縛る** — 探索円板は**中心画素のクラス**で
   `r_eff = radius[c0] · s`、`s = min(1, α / kRtRestirRadiusAlphaRef)`
   (`kRtRestirRadiusAlphaRef = 0.36` = `kRtReflMaxRoughness²`)。そのうえで採用の可否を
   **候補のクラス**で `|offset| ≤ radius[c_n] · s` と再判定する。`r_eff` が 1 px 未満なら
   タップ 0 (鏡面では spatial が自然に切れる)。
   タップ回転は**画素ハッシュだけ**で決め、フレームでは回さない。
8. **spatial は既定 off** (`RtReflRestirParams::spatial = 0`)。実装・UI・CLI は残す。
9. **v1 は biased** — 候補ごとの可視レイは既定 off (`--rt-restir-visray` で on)、
   MIS 重みも持たない (M 加算の biased 変種)。ただし **p̂_q(y') = 0 の候補は M も数えない**。
10. **ReSTIR off (既定) の絵は現行とビット一致**。これを golden
    `demo_render_rtrefl` / `demo_render_rtgi` (ローカル限定 tol=0) で機械証明する。
    on 側も `demo_render_rtrefl_restir` (既定構成 = temporal のみ、**frame 40**) で固定する。

## 理由

### なぜ reconnection 方式なのか (方向の再利用ではなく)

「隣の画素のサンプルを借りる」を方向で行うと、**借りた瞬間に幾何が嘘になる**。
床の画素 A が拾った方向 `L` を 8 px 離れた画素 B に貼ると、B から見た `L` の先には
A が見ていた物体が無い。ReSTIR が成立するのは、候補を**受け側の target function で
評価し直す**からで、そのためには「何に当たったか」= ヒット点が要る。

ヒット点を持つと、副産物として**そこに刺さっている `RtInstance` が分かる** —
これが ReflectionClass を per-sample の意味論として運べる唯一の理由になっている。
方向だけの reservoir では「このサンプルは主役に当たっている」を後段が知りようがない。

代償は帯域: `xs` は fp32 が要る (fp16 の座標では遠景で `d²` の比が壊れる)。
`Ls` / `ns` は fp16 で足りるので、1 組 56 B/px に収まっている。

### なぜクラスは受け側 G-Buffer (RT3.a) ではなくヒット側 `RtInstance` なのか

元計画の初版はクラスを **G-Buffer RT3 の a チャンネル**に置いていた。これは目的と噛み合わない:
反射パスがシェーディングしているのは**反射する側 (床・水面)** で、そこから読めるのは
「床のクラス」でしかない。制御したいのは**反射に映る側 (プレイヤー・敵・車)** なので、
クラスはレイのヒット点で引かなければならない。

ヒット側に置いた結果、この計画は **G-Buffer / `MaterialCB` / `deferred_gbuffer.hlsl` /
`ForwardPath` に一切触らなくなった**。`RtHitMaterial` が既に
`gRtMaterials[gRtInstances[hit.inst].materialIndex]` を引いているので、同じ構造体への
アクセスが 1 本増えるだけで追加コストは実質ゼロ。`RtInstance` は `pad0` が空いていたので
**改名だけで済み `sizeof == 80` は不変** (レイアウトが動けば golden 全枚の撮り直しになる)。
RT3.a は空いたまま残してある。

### なぜ 2 パスなのか (temporal の Dispatch を分けないのか)

temporal 統合は「自画素の初期 reservoir + 前フレームの reservoir」しか読まない =
近傍の同期が要らない。別 Dispatch にすると reservoir を**同じテクスチャで読み書き**することに
なり、`R16G16B16A16` / `R32G32B32A32` の **typed UAV load は Feature Level 11_0 では保証されない**
(保証は R32 単チャンネルのみ)。3 組目のテクスチャを持てば回避できるが、それは
14 MB/スロットをもう 1 組増やす取引になる。

spatial だけは近傍が揃ってから読むので別 Dispatch (元計画どおり)。

### なぜ保存量が wSum ではなく W なのか

統合の教科書形は `w = p̂_q(y') · W' · M' · J` で **W** を使う。wSum を保存すると
統合のたびに `W' = wSum'/(M'·p̂')` を復元することになり、`p̂'` (= 候補**自身の**受け側で
評価した値) を別に保存するか再計算する羽目になる。W で持てば統合式が教科書のまま書ける。

resolve 側は wSum が要るが、これは**パス内のレジスタ**で持てば足りる
(`out = Ls · wSum / (M · lum(Ls))`)。M = 1 なら `lum/lum = 1` で `Ls` とビット一致する —
これが「再利用ゼロなら現行と同じ絵」を保証している構造そのもの。

### なぜ temporal も厳密な Jacobian なのか (U4、planner の近似案は却下)

planner は **J = 1 近似**を推した。理由は「再投影の妥当性判定 (深度 5% / 法線しきい値) を
通った点なら `P_prev ≈ P` なので、`d²` と `cosθ` の比の誤差は 5% 以内に収まる。
`rpos` テクスチャ 1 枚 = **+16 B/px と帯域**を丸ごと節約できる」。

**ユーザーは厳密に計算する方を選んだ (2026-09-04)。** 以後蒸し返さない。
帰結として reservoir は 4 枚 → 5 枚 (40 → 56 B/px)、`spatial` と `temporal` が
**同じ `RtRestirJacobian` / 同じ棄却範囲 `kRtRestirJacobianMax = 10`** を共有する形になった。
副産物として検査しやすさが上がっている: 静止シーンでは `P_from == P_to` なので
J は 1 **ちょうど** (同じ式に同じ値が入るので fp でも比が 1) になり、
「配線が壊れていれば J が範囲外で temporal が全棄却 → M が 1 から伸びない」= **M が cap まで
伸びること自体が `rpos` の配線検査を兼ねる** (A6)。

副作用も引き受けている: カメラが大きく動いたフレームは J が範囲外に落ちて履歴が切れやすい
(= ノイズへ戻る。安全側の壊れ方)。目立つなら temporal だけ `jMax` を緩められるよう、
`RtReservoirMerge` は `jMax` を**引数で受ける** (関数内定数にしていない)。

### なぜ spatial は reservoir を書き戻さないのか (M67f round 1 の反証)

教科書 (ReSTIR GI) の spatial は結果を履歴へ書き戻す。**それをそのまま入れたら A7-b が
不成立になった**。書き戻すと近傍の履歴が自画素の履歴に混ざり、次フレームの temporal が
それをさらに運ぶ = **フレームを跨いで拡散する**:

| 症状 | 書き戻しあり (round 1) | 書き戻しなし (round 2) |
|---|---|---|
| Prop クラスの reservoir が占める画素 (frame 3 → 40) | 9,988 → 40,432 px (**+305%**、対照の temporal 単独は 8,264 → 11,656) | spatial on / off で**ビット一致** |
| 鏡面パッチの平均輝度 (temporal 単独比) | **+8.4%** | −0.18% |
| 鏡面パッチのフリッカー指標 | 1.21 (temporal 単独 0.206 の 5.9 倍) | 0.198 (temporal 単独以下) |

書き戻しを断つと「**Hero のサンプルは radius[Hero] より遠くへ運ばれない**」が
フレームを跨いでも成り立つ = クラスの意味論が数十フレームで溶けなくなる。
払った代償は「spatial の効果がフレーム間で積み上がらない」こと。

採ったのは coder が出した 4 案のうち (C) = 書き戻しを断つ。却下した残り 3 つ:
(A) 受け入れ基準を緩める = 目的未達の既定 on を出荷することになる、
(B) 統合する M の上限を `min(mCap_中心, mCap_候補)` にする = 非対称を減らすだけで
拡散も乗り換えも残る、(D) クラス境界で半径を線形補間する = 症状にしか効かない。

### なぜ半径を受け側の α に比例させるのか

`p̂` のローブ幅は α に比例する。ローブの外のタップは `p̂ ≈ 0` で、
「ちょうど 0」なら M に数えないが (下記)、**「小さいが 0 ではない」候補は M に数えられて
推定を薄める** = 暗化する。実測 (鏡面パッチ、α = 0.01): 一様 Prop で −5.2%、Hero で −1.9%。

`s = min(1, α / kRtRestirRadiusAlphaRef)`、`kRtRestirRadiusAlphaRef = kRtReflMaxRoughness² = 0.36`
(粗さ 0.6 で等倍 / 0.5 で 0.69 倍 / 0.10 で 0.03 倍)。`r_eff` が 1 px 未満ならタップ 0 —
**鏡面では spatial が自然に切れる**。基準値は CB (`gRsRadiusAlphaRef`) で運び、
チューニング UI のスライダ (0.01〜1.0) から実行中に触れる。

候補側にも同じ `s` を掛けた半径判定をもう一段置いてある。中心が Prop (12 px) でも、
その円板の中に映っている Hero のサンプルは 2 px より遠くへ運ばれない = **クラスの境界で
「主役が急に遠くから借りられる」が起きない**。中心と候補で尺度を揃えないと
「中心の実効半径では届く距離なのに候補の生半径で弾かれる」がまだらに出るので、
α 係数は両方に掛ける。

### なぜタップ回転をフレームで回さないのか

一般的な spatial reuse はフレームごとに Vogel 螺旋を回して候補集合を脱相関させる。
**書き戻しを断った後はそれが害になる**: 候補集合が毎フレーム入れ替わると採用サンプルが
乗り換え続け、時間フリッカーが倍になる (round 1 の構成からタップ回転だけを外した
ablation で 1.21 → 0.61、鏡面パッチ)。
脱相関が要るのは「近傍の情報が履歴に積み上がる」設計であって、こちらはそうではない。
回転は `RtNextRand2(uint3(px, kRtRestirTapSeed))` = **画素ハッシュだけ**で決める。

### なぜ spatial の既定が off なのか

**規則 (spec §7 U7) で先に決めてから測った**: 目標帯 (`--acoustic-demo` の床、粗さ 0.5、
`--rt-debug 11` = 反射レーン単体、frame 120/121 のフリッカー指標) で
spatial on が temporal 単独より良ければ on、良くなければ off。

| 条件 | 床全体 | プレイヤーの映り込み | 敵の映り込み |
|---|---|---|---|
| ReSTIR off (1spp) | 2.943 | — | — |
| temporal 単独 (**出荷構成**) | **0.181** | **0.342** | **0.239** |
| spatial on | 0.255 | 0.367 | 0.343 |

3 矩形すべてで悪化したので **off**。原因は設計の帰結で、MIS 重みを持たない biased 合成では
近傍の `p̂` 比がそのまま重みの分散になる (unbiased 化 = MIS 重みつき ReSTIR は M67 のスコープ外)。
鏡面側は α 比例半径のおかげで悪化していない (0.198 ≤ temporal 0.206) ので、
「on にすると壊れる」ではなく「今の帯では得が無い」。

実装・UI・CLI (`--rt-restir-spatial`) は残してある。粗い面が主役のシーンでは効きうるし、
反転は `RtReflRestirParams::spatial = 1` の 1 行 + golden の撮り直しで済む。

**所見: S5 (パラメータ調整) で最初に触るノブは M 上限であって半径・タップではない。**
出荷構成 (spatial off) で全インスタンスを一様 Prop (M 上限 32) にすると、既定の混在
(Default 16 / Prop 32) の **1.8 倍良い** (床全体 0.100 vs 0.181)。

### なぜ p̂ = 0 の候補は M を数えないのか

教科書の biased 変種は「p̂ = 0 でも M を足す」。**この計画の被写体ではそれが暗化として出る**:
`rdemo_mirror` は粗さ 0.10 (α = 0.01) でローブ幅が 1° 前後しかなく、Default クラスの
半径 8 px が張る立体角はそれと同じオーダー = 候補の大半がローブの外 (p̂ = 0) に落ちる。
M を数えると `W = wSum/(M·p̂)` が縮んで鏡面パッチが目に見えて暗くなる。

数えない側の偏りは「わずかに明るい / 分散が減らない」で、**鏡面のディテールを守る方向**
(= 元計画の狙い) に倒れている。「候補から外す」= M も数えないのは 4 種:
空 reservoir (M' = 0) / 受け側の幾何不一致 / J が範囲外 / 有効重み `w` が
`!(w > 0) || !(w < 1e30)`。**自画素の初期サンプルだけは lum = 0 でも常に M = 1**
(`RtReservoirUpdate` は M を数え、`RtReservoirMerge` は w > 0 のときだけ Update を呼ぶ)。

M67c round 1 の実装は数える側だったので REWORK になった。selftest
(`RtSelfTest.cpp::TestRestir`) に「p̂ = 0 の候補で M も wSum も増えない」を固定してある。

### VNDF pdf は上半球で 1 に積分されない (受け入れ条件の誤りを訂正した話)

spec の初版 A4 は「VNDF pdf を半球で積分すると 1」と書いていた。**成立しない** —
反射方向の pdf は半ベクトル側で正規化されるので、サンプラが下半球へ回した分だけ
上半球積分は 1 を下回る。実測 (決定的な (cosθ, φ) グリッド、α = 0.36 / 0.04):
**0.884 + 漏れ 0.117 = 1.001** / **0.998 + 0.002 = 1.000**。
selftest は「上半球積分 + サンプラの漏れ = 1 (±0.01)」と pdf のピークが独立な Smith Λ 形の
式と一致することの 2 本で押さえている。

### HLSL で `isfinite()` / `isinf()` を使わない

`ShaderManager` は `D3DCOMPILE_IEEE_STRICTNESS` を渡していない。`/Gis` 抜きの fxc は
`isfinite()` に警告 **X3577** を出したうえで**最適化除去しうる** (M67c で実測) =
ノーガードのまま実行時コンパイルの警告だけが出る状態になる。
非有限の防波堤は `!(w < kWeightMax = 1e30)` のような普通の比較で書く (NaN も落ちる)。
`[1e30, FLT_MAX]` の有限重みも一緒に落ちるが、そこは p̂ が 1e-25 級に潰れた異常値。

## 帰結

### 決定論と後方互換

- **描画専用のレーン**。sim / `WorldHash` / `.rep` / ECS / `FieldDesc` / ABI / C# に触れない。
  乱数は `RtPcg3d(pixel, frame)` 系列のみで CPU へ読み戻さない (ADR-008 と同じ免除)。
  `tools\replay_verify.bat` が全サブで無変更緑であることを機械確認している。
- **ReSTIR off の絵は現行とビット一致**。これを主張できるのは M67a で
  `demo_render_rtrefl` / `demo_render_rtgi` を golden に足したからで、それ以前は
  `shot_verify.bat` の 19 本に `--rt-*` が 1 つも無く **RT レーンの絵はどこにも固定されていなかった**
  (壊れても全 golden が緑のまま通る状態)。RT の 3 枚は SSR と同じ理由で
  **ローカル限定 tol=0** (`MYE_SHOT_SKIP_RT`) — BVH のトラバーサルは hit/miss で離散的に分岐し、
  1 ULP の差が画素を反射色 ⇔ IBL フォールバックへ丸ごと飛ばす。
- `Material` にフィールドが 1 本増えたことの波及が 1 つある: **cooked blob は `Material` を
  memcpy する**ので `kCookVersion` を 1 → 2 へ上げ、`ModelCook.cpp` の
  `static_assert(sizeof(Material) == 64)` を更新し、`AssetID` (uint64) の境界で丸まる 4 バイトを
  **明示 `pad0`** にした (暗黙パディングのままだと同じ入力の cooked ファイルのバイト列が
  run ごとに変わりうる = `CookedCacheSelfTest` の memcmp が不定になる)。
  影響は「初回起動で 1 回焼き直す」だけ (`cache/` は gitignore)。封印パッケージ (M51j) は
  新しい exe で作り直す。設計判断ではなく機械的帰結なので詳細は engine_spec §10.2 に置いた。

### コストと容量 (実測)

GPU 時間 (**WARP** / Release / `--render-demo --deferred` / `--frames 20`。`[rt]` ログの
`GpuTimer` は 7 フレーム目からしか回収しないので `--frames 6` の撮影 run では全項 0.000 ms になる):

| 構成 | `refl` (トレース + 初期 reservoir + temporal) | `restir` (spatial + resolve) | `denoise` (SVGF、無変更) |
|---|---|---|---|
| `--rt-restir` (既定 = temporal のみ) | 6.520 ms | **2.320 ms** | 23.327 ms |
| `--rt-restir-spatial` | — | 2.826 ms | — |
| `--rt-restir-visray` (spatial を含意) | — | 6.166 ms | — |

(M67g の golden 撮影 run — 同じシーンを `--frames 41` で回したもの — でも
`refl 7.071 / restir 2.414 / denoise 29.896 ms` と同じ桁で再現した。)

WARP の絶対値なので実 GPU の比較には使えない (README の計測表が RTX 3060 の数字)。
読むべきは比: **可視レイを on にすると `restir` が 2.2 倍**になる (2.826 → 6.166) =
タップごとに `RtTraceAnyHit` を撃つコストがそのまま出る。既定 off の根拠の 1 つ。
`refl` と `denoise` は spatial / 可視レイのどちらでも触らない (別 Dispatch)。

reservoir は 5 枚 × 2 組 = **56 B/px × 2**。960×540 の内部 1/2 (480×270) で
**1 スロット約 14.5 MB**、1600×900 の内部 1/2 (800×450) で約 40 MB。
`RtHistory` と同じく **viewKey 別に遅延確保**するので、エディタで SceneView + GameView を
両方描くと 2 スロット (960×540 なら約 29 MB)。ReSTIR を一度も on にしなければ 0 バイト。

### 既知の制限 (v1)

- **可視レイ off のバイアス (光漏れ)**。候補が受け側から見えているかを確かめないので、
  遮蔽物越しのサンプルが混ざりうる。`--rt-restir-visray` / UI のトグルで A/B できる
  (既定 off の根拠は上の GPU 時間)。
- **unbiased ではない** (MIS 重みが無い)。M 加算の biased 変種。spatial の既定 off は
  この選択の直接の帰結でもある。
- **ReSTIR をトグルしても SVGF 側の履歴 (`reflHist_`) は落ちない** — 切り替え直後の
  数フレームは新旧の値が混ざる。どちらも同じ量の推定量なので数フレームで収束し実害は無い
  (reservoir 側の履歴は `hasLast = false` で正しく落ちる)。
- **W のクランプを入れていない**。`Ls` は fp16、W は fp32 なので、極端に暗い `lum` の候補が
  大きな W を持てば firefly になりうる。M67e の実測では兆候なし
  (`--rt-debug 11` の最大輝度 on 227.5 < off 247.9、孤立高輝度画素 0) なので入れなかった。
  カメラが大きく動く条件は自動検証では通らないので、出たら `kRtRestirWMax` を 1 行足す。
- **スキンメッシュは BVH に入っていない** (M46b からの v1 制限、`RenderSystem.cpp` が
  `SkinnedMeshComponent` を持つ物を RT の収集から外す)。つまり**主役が
  スキンキャラクタなら、そもそも反射に映らない** — ReflectionClass をどれだけ積んでも
  目的は達成されない。M67 は箱アクタで成立させ、スキンの BVH 投入は後続へ切ってある。
- パラメータの確定値 (S5) は harness の外でユーザーが回し、後続 `M67h` で
  `RtTypes.h` の既定表へ焼いて golden `demo_render_rtrefl_restir` を撮り直す。
