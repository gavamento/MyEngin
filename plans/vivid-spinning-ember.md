# M63: パーティクル B群 — 描画表現力 (C1〜C5)

**再開手順**: `git log --oneline -5` で最後に完了した M63x を確認 → 本ファイルの進捗表と突き合わせ →
次のサブの節を読む → 着手前にそのサブの「冒頭確認」があれば先に潰す。
1 サブ = 1 コミット (`M63a: ...` 形式の日本語件名) = 1 セッション + /clear。
進捗の一次情報は git log、本ファイルの進捗表には**計画外の事実・罠・申し送りのみ**書く。

## Context

パーティクルシステムのレビューで挙がった「描画表現力の構造的欠落」8 件のうち、上から 5 件を回収する。
M61a〜g (A群 = 挙動: 放出基底・サブフレーム・速度継承・カールノイズ・プリウォーム・GPU 容量追従・
ローカル空間) で**粒子の動き**は揃った。残っていたのは**粒子の見た目**で、以下はすべて
「フィールドが 1 つも無い = 構造的に不可能」の状態にある (現 HEAD で実測):

| # | 欠落 | 現状の根拠 |
|---|---|---|
| C1 | 回転属性が構造的に不存在 | `ParticleInstance`(48B) / `GpuParticle`(48B) のどちらにも角度なし。`engine_spec.md:677` (spec 7.2) には `rotation` が**仕様として書かれているのに未実装** |
| C2 | 速度ストレッチビルボード不可能 | CPU インスタンスに velocity 無し。GPU プールには `vel` があるが VS が読まない (`particle_render_gpu.hlsl:141-157`) |
| C3 | フリップブックが age の純関数 | `frame = floor(age*flipCycles*tiles) % tiles` の 1 式のみ。同 tick 湧きの粒子が全部同じコマ。フレーム間ブレンド / ランダム開始 / FPS 指定なし |
| C4 | 完全 unlit | 全パーティクルシェーダにライト / シャドウ / IBL 参照ゼロ (grep 実測) |
| C5 | 深度衝突が素朴 | +1/+1 隣接 2 タップの外積のみでシルエット境界で破綻。画面外は無条件すり抜け。摩擦 / kill-on-collide なし (`particle_sim.cs.hlsl:112-145`) |

**出口の姿**: Inspector でフィールドを立てるだけで、回る火の粉・伸びる火花・コマ送りの爆発・
点光源に照らされ影の中を通る煙・床で跳ねて滑る破片が作れる。**既定値では既存シーンが 1 ビットも
変わらない** (M59/M60/M61 と同じ存在ゲート方式)。

### ユーザー決定 (2026-08-30)

1. マイルストーン番号 = **M63** (M61 = パーティクル A群で消費済み、M62 = 物理ロードマップ予約)
2. 粒度 = **5 サブ = 5 コミット**
3. M63d の範囲 = **全ライト + CSM 影 + IBL irradiance** (局所影アトラスと鏡面 IBL は除外)
4. `common.hlsli::ApplyLighting` は**抽出を試し、golden が割れたら複製ミラーへ縮退**

---

## 全体設計 (5 つの判断と根拠)

### 判断 1 — 構造体は **全部 48B 据え置き**。`pad[3]` を意味づけし直す

```
ParticleInstance.pad[3]  →  { float rot; float stretch; float flipFrame; }   // CPU が畳んで送る
GpuParticle._pad         →  { float rot0; float rotVel; float flipU; }       // GPU は VS で作る
EmitData._pad            →  { float rot0; float rotVel; float flipU; }
```

核心は「**CPU は速度 3 float を送る必要がない**」こと。`CpuParticleBackend.cpp:620` で
`const XMFLOAT4X4& vm = view.view;` がインスタンス充填ループのスコープに入っている (実測確認済み) ので、
速度ストレッチを「画面基底へ射影した角度 + 長軸倍率」の**2 スカラへ CPU 側で畳める**。
回転角と速度角は同じ θ なので枠を共有して加算する。

却下: (a) 64B 拡張 = 畳めるデータのために 100k 粒子で +1.6MB/frame を払う理由がない /
(b) モードフラグで排他再解釈 = フリップブック開始コマが回転・ストレッチと直交するので 5→3 が埋まらない /
(c) `invLife`/`size0` のビットハッシュから導出 = `ParticleInstance` はどちらも持たず、かつ
`lifetimeMin==Max && sizeMin==Max` のありふれた設定で全粒子が同一ハッシュ = **C3 そのものを再現する**。

**CPU/GPU で同じ絵になる保証**は、新規 `assets\shaders\particle_billboard.hlsli`
(register 宣言ゼロ = `froxel_common.hlsli` と同じ流儀) に四隅変換を 1 本だけ置き、
`particle_render.hlsl` / `particle_render_gpu.hlsl` / `particle_distort.hlsl` が**同じ関数を呼ぶ**こと。

### 判断 2 — 回転は **閉形式で導出**する (sim 状態にしない)

`rot = rot0 + rotVel * elapsed`、`elapsed = 1/invLife - life`。

- **`particle_sim.cs.hlsl` の変更ゼロ**。SIMD 経路 (`CpuParticleBackend.cpp:352-416`) も無風
  (不変データなので `Simulate` が 1 命令も触らない)。
- ★**触るのは `KillDead`(`:418-438`) の swap-and-pop 3 行と `EmitParticles`(`:207-215`) の
  resize 3 行だけ。ここを忘れると「粒子が死ぬたびに隣の回転が飛び移る」**という、
  絵は出るのに合わないだけの静かな壊れ方をする。本計画で最も忘れやすい 6 行。
- 却下: 毎 tick 積分 — 定数角速度の閉形式が `rot0 + rotVel*t` そのもので絵は 1 画素も変わらないのに、
  SoA を毎 tick 書き SIMD レーンを足し GPU sim CS も直すことになる。
  回転減衰を将来入れるなら閉形式が壊れる → **その時に積分へ移行する**と申し送る。

### 判断 3 — RNG 消費は**ゲートで囲う** (これが golden 15 枚を守る唯一の根拠)

`ParticleCurves.h:529-537` の消費列 (方向 → 位置 → 速度 → 寿命 → サイズ) の**末尾**へ 3 本足すが、
既定エミッタでは 1 draw も引かない:

```cpp
// M63a: 回転/ランダム開始コマを引くか。false = 従来と 1 draw も違わない消費列
// = **既定エミッタの絵をビット保存する唯一の根拠**。
// ★3 本まとめて 1 つのゲートで消費する (組み合わせで消費数が変わる系統を作らない)
inline bool ParticleUsesSpawnAttribs(const ParticleEmitterComponent& d);
// 引く順序 (不変): rot0 = Range(rotationMin,Max) → rotVel = Range(rotationSpeedMin,Max)
//                  → flipU = NextFloat01()
```

`SampleParticleShape` が `emitFrom=0` で従来と同一消費に縮退させている前例と同じ設計。
無条件に引くと後続粒子の方向/位置/速度/寿命/サイズが全部ずれて**スクショ golden が全部動く**。

### 判断 4 — M63a を「共有契約コミット」に兼任させる (M61a の前例)

**構造 / ハッシュ / snapshot に触る変更を M63a に全部畳み込む。M63b〜e は描画のみ = ハッシュ不変。**
見返りは `sizeof(ParticleEmitterComponent)` の変更が 1 回だけ → `kSimSnapshotVersion` 6→7 が 1 回だけ
→ **golden `.rep` の再記録も 1 回だけ**。M63b〜e は `replay_verify.bat` が無変更のまま緑。

`Scene::kDocVersion`(3) と `kReplayFileVersion`(4) は**上げない** (フィールド名ベースのシリアライズ /
`.rep` はレイアウト不変)。`GpuParticleCB` の `static_assert` だけはサブごとに 1 行ずつ更新する
(死んだ CB 空間を先出しするより、HLSL 変更と同じコミットでガード値を動かすほうが健全)。

### 判断 5 — ピクセル被覆は **新規 `--particle-demo` + golden 2 枚 (CPU/GPU)**

新機能は全部既定 off なので、golden に絵が出ないと**回帰検出がゼロ**になる。

- 却下 (既定デモ拡張): #1 `demo_forward` / #2 `demo_deferred` は **CI 対象**。5 サブ全部でこの 2 枚が
  動くと無関係な変更のレビューで毎回赤くなる。
- 却下 (`--fog-demo` 拡張): #15 は既に「フロクセル 2 分岐 × 中間キー × VFX」の被覆を密に載せていて、
  `DemoContent.cpp:2264-2268` に**「被覆を足すために別の被覆を潰さないこと」と明文の警告がある**。
- 採用の決め手: C1〜C3 は **CPU インスタンス経路と GPU VS 経路の 2 実装**を持つのに、今の 15 枚には
  **両者を同じ被写体で突き合わせる golden が 1 枚もない**。CPU/GPU 2 枚撮れば片方だけ直したときに
  必ず赤くなる。`:shot` の 4 トークン制限 (`shot_verify.bat:295`) には 2 トークンで収まる。

---

## サブ計画

### M63a — 回転属性 + 共有契約  ★ここだけが sizeof / hash / snapshot / golden を動かす

**追加フィールド 18 本** (`Components.h:150` の `emitFrom` の後へ末尾 append。C1〜C5 全部を一括):

| フィールド | 型 / 既定 | 消費 |
|---|---|---|
| `rotationMin` / `rotationMax` / `rotationSpeedMin` / `rotationSpeedMax` | float / 0.0f | M63a |
| `stretchScale` / `stretchMax` | float / 0.0f (off) / 4.0f | M63b |
| `flipFps` / `flipBlend` / `flipRandomStart` | float 0.0f / int32 0 / int32 0 | M63c |
| `lightingMode` / `lightWrap` / `lightIntensity` / `lightReceiveShadow` | int32 0 (unlit) / float 0.5f / float 1.0f / int32 1 | M63d |
| `collisionThickness` / `collisionFriction` / `collisionLifeLoss` / `collisionFloor` / `collisionFloorY` | float 0.0f ×3 / int32 0 / float 0.0f | M63e |

既定でビット同一の根拠: `Pcg32::Range(0,0)` = `0 + (0-0)*u` = 厳密に `+0.0f` (`Random.h:33`)、
かつゲートが false で**そもそも draw しない**。

**触る場所**: `Components.h:150` / `Components.cpp:122` (`MYE_JP` 18 行) /
`InspectorWindow.cpp:183,213,231` (`lightingMode` の enum コンボ + en/ja ラベル 2 本のみ、他は素の Drag) /
`ParticleCurves.h` (`ParticleUsesSpawnAttribs` / `ParticleElapsedFromLife` / `ParticleRotationAt` /
`ParticleBillboardCornerCpu`) / `CpuParticleBackend.h:45-49` (SoA +3) /
`CpuParticleBackend.cpp` の `:21-27`(pad 再定義) `:29-61`(CB 末尾 append) `:207-215`(resize)
`:230-315`(ゲート付き 3 draw) **`:418-438`(KillDead swap ★)** `:700-720`(充填) /
`GpuParticleBackend.h:40-52` + `.cpp:70-79`(`_pad` 再定義) `:47-68`(GpuRenderCB) `:437-485`(emit ミラー) /
`WorldHasher.cpp:236-255` (+3 組) / `SimSnapshot.h:65` (**6→7**) + `.cpp:139,190` (PodVector +3) /
`DemoContent.{h,cpp}` (`BuildParticleShowcaseScene`) /
`RuntimeMain.cpp` + `EditorApp.cpp` + `EditorMain.cpp` (`--particle-demo` は**両方に要る**) /
`shot_verify.bat` (16/17 枚目) / `engine_spec.md` 7.5。

**HLSL**: 新規 `particle_billboard.hlsli` に `ParticleBillboardCorner(corner, rot, stretch)` と
`ParticleElapsedFromLife(life, invLife)`。3 描画シェーダは
`if (gBillboardMode != 0) { c = ParticleBillboardCorner(...); }` の**1 分岐だけ**を足す
— off では元の式そのもの (`* 1.0f` も `cos(0)` 乗算も通らない)。`particle_sim.cs.hlsl` は 1 行も変えない。

**selftest** (`ParticleSelfTest.cpp` 末尾へブロック append。`EditorMain.cpp` の連鎖は触らない):
既定 desc でゲートが false + RNG 消費数が従来と同一 / `rotationSpeedMax` だけ立てて 3 draw ちょうど /
`Range(0,0)` が厳密に `+0.0f` / `ParticleBillboardCornerCpu(corner, 0, 1)` が corner と**float 等値で
ビット一致** (ここが割れると既定の絵が動く) / `rot=π/2` で `(1,1)→(-1,1)` / `stretch=3` の長軸方向。

**リスク**: `KillDead` の swap 漏れは selftest で検出できない — **`particle_cpu` golden が唯一の検出器**
なので、回転が非対称になるエミッタを必ずデモに置く。

---

### M63b — 速度ストレッチビルボード  (フィールド追加なし = hash 不変)

`ParticleCurves.h` へ `EvalParticleStretch(vx,vy,vz, camRight, camUp, scale, maxScale)` を追加:
```
angle = atan2(dot(v, camUp), dot(v, camRight))          // 画面基底へ射影した速度の向き
scale = clamp(1 + length(v) * stretchScale, 1, stretchMax)
```
CPU は充填ループ (`:700-720`) で `rot += angle; stretch = scale;` (`stretchScale == 0.0f` では
**加算演算自体をしない** — `velocityInheritance` と同じ `-0.0` 化け回避の作法)。
`particle_render.hlsl` は**何も変わらない**。GPU 側だけ VS で同じミラーを `p.vel` から呼ぶ。

★`ParticleBillboardExpand`(`ParticleCurves.h:212`) に `stretchMax` を掛ける
— **伸びた粒子が画面端でカリングされて消える**。落とすと golden が割れる。
★`speed < 1e-4` で `angle = rot` へ落とす (静止粒子の角度ジッタ回避)。

**リスク**: `atan2f` が 100k 粒子で 1〜2ms。縮退は多項式近似だが**両側同時でないと CPU/GPU の絵が割れる**
ので片側だけの最適化は禁止と明記する。

---

### M63c — フリップブック (ブレンド / ランダム開始 / FPS)  (フィールド追加なし)

`ParticleCurves.h` へ `ParticleFlipFrame(age, lifetime, flipCycles, flipFps, flipU, tiles, randomStart)`
→ 連続コマ位置 (float)。**`flipFps==0 && randomStart==0` で `age*flipCycles*tiles` と厳密に同一の
演算列へ落ちること**が唯一のビット保存契約。
タイル UV 計算 `SampleFlipTile(tex, samp, uv, frame, tx, ty)` を `particle_billboard.hlsli` へ括り出す
(現状は同じ式が CPU/GPU の PS に手写しされている)。PS はブレンド時のみ 2 コマ sample して `frac` で lerp。
`GpuParticleCB` へ `params6` を末尾 append → **`static_assert` 320→336**。

**冒頭確認**: `--particle-demo` に使えるフリップブックテクスチャ資産が既存にあるか。
無ければ procedural 円のままになり**このサブの golden 被覆が消える** — 着手前に確認すること。

---

### M63d — ライティング  (フィールド追加なし)

**段階① — `common.hlsli:295-366` の `ApplyLighting` から `LightSample(Light, posW, out toL, out atten)`
を純粋抽出するだけをやり、`shot_verify` 15 枚 + `replay_verify` がビット一致することを単独で確認する。**
1 画素でも動いたら即 revert し、`particle_light.hlsli` へ「正本は `common.hlsli::ApplyLighting`」
コメント付きの複製ミラーへ縮退 (家が明示的に許容する形 — `engine_spec.md:735-737`)。
`ApplyLighting` は forward 3 本 + deferred + hybrid が呼ぶ、本計画で最大の爆発半径を持つ編集。

**段階②** — `RenderView` 末尾へ `const SceneLightData* lights = nullptr;`
(`terrain`/`decals`/`probes` と同じ非所有ポインタ + null 自然無効化)。`RenderSystem.cpp:1128` の直前で
`view.lights = &lights;` の 1 行。`RenderTypes.h` へ `ParticleLightCB` (≒1280B) と
`MakeParticleLightCB(view, bound)` を **1 本だけ**定義し CPU/GPU が共有 (`MakeFroxelForwardCB` と同じ形)。
★`GpuParticleCB` には絶対に入れない (毎エミッタ毎 tick 上がる CB に 1KB のライト配列を積まない)。
★ライト CB のアップロードは**エミッタループの外で 1 回**。

新規スロット: CPU = b1 / t4 (CSM) / t5 (IBL irr) / s1 (比較サンプラ)、GPU = b2 / t5 / t6 / s1。
`RenderTypes.h` の `froxel::` の隣へ定数 4 本 → **`check_rules.ps1` の `$constGroups` へ 4 エントリ登録**。
★**SRV は必ず剥がす** — シャドウマップは次フレームのシャドウパスで DSV になる
(M57d/e が t15/t7/t3 で 3 度踏んだ罠と同型)。

新規 `particle_light.hlsli`: `ParticleSphericalNormal(d, camRight, camUp, camFwd)` (球面法線) /
`ParticleWrapDiffuse(ndl, wrap)` / `ParticleDiffuseLighting(...)`。
PS では `col` が確定した**直後・フォグの前**に挿す (既存チェーン
`色 → フォグ → フロクセル → ソフトフェード` は 1 文字も動かさない)。
`lightingMode` 0=unlit / 1=粒子単位 (VS で畳む) / 2=画素単位 (球面法線)。

**二重計上は起きない** (論証): フロクセルと `ApplyFog`/godray が担うのは
**カメラと粒子の間の媒質**の散乱と透過率。新規のライティングは**粒子自身のアルベド × 入射放射照度**で、
物理量も場所も別。`FroxelCompositeParticle` が加算合成に inscatter を足さない守りは
「重なった粒子の枚数ぶん霧が濃くなる」の防止であって粒子の陰影とは無関係 — **1 行も触らない**。
唯一の副作用は godray が screen-space なので粒子が明るくなればシャフトも強くなること (増幅であって
二重計上ではない) — 申し送りに書く。

**除外**: 局所影アトラス (CB +1.5KB に対し煙塊へのハード局所影の見返りが薄い。局所ライトのビームは
フロクセルが担当済み。`common.hlsli:370` の全要素 1.0 オーバーロードで恒等に落とし将来の余地は残す) /
鏡面 IBL・prefiltered・BRDF LUT (ビルボードに roughness も F0 も無く物理的に無意味)。

`GpuParticleCB` へ `params7` → **`static_assert` 336→352**。

---

### M63e — 深度衝突の改善  (フィールド追加なし。**GPU バックエンド限定**)

`particle_sim.cs.hlsl:112-145` を置き換える:

1. **法線 5 タップ化** — `pix±(1,0)`, `pix±(0,1)` を Load し、軸ごとに `|Δdepth|` の小さい側を採ってから
   外積。`clamp` に下限 `int2(1,1)` を入れる (現状は上限 `-2` のみ)。シルエット境界の破綻を潰す。
2. **摩擦** — `ParticleCurves.h:169` へ `ReflectWithFriction(vel, n, restitution, friction)`。
   ★`friction == 0.0f` の**早期 return で `ReflectWithRestitution` を呼ぶ** — 代数的には一致するが
   演算列まで一致させるには早期 return が唯一確実。
3. **寿命損失** — 押し戻し直後に `if (gCollParams2.x > 0) { p.life -= gCollParams2.x / p.invLife; }`。
   数行下の既存 `if (p.life <= 0) { gDeadList.Append(slot); return; }` が**そのまま回収する**
   (新しい dead-list 経路も新しい分岐も要らない)。`1.0` = kill-on-collide。
4. **解析床** (`collisionFloor`) — 深度ブロックの**後**に独立したブロックとして。画面外/背面/空ピクセルでも
   効く唯一の衝突。★シェーダとフィールドのコメントに「**任意形状には効かない proxy であって
   深度衝突の代替ではない**」と明記する。ローカル空間では深度衝突と同じ理由で無効。
5. `GpuParticleBackend.cpp:543` の `collParams.z` **0.0f 固定を撤廃**し `.w` の予約枠も消費。
   `collParams2` を末尾 append → **`static_assert` 352→368**。
6. ★`particle_sim.cs.hlsl:118-119` は `ParticleClipToUv`(`ParticleCurves.h:158`) 相当を
   **インライン再記述していて共有されていない**。HLSL ミラーを `particle_gpu_common.hlsli` へ 1 本置いて
   sim CS がそれを呼ぶ形に統一する (今まで空いていた穴の回収)。

**`GpuAliveEstimator` の前提が崩れる件 — コード変更は不要、コメント更新のみ** (評価済み):
早死にで**過大推定**になる → `StepGpuIdleSkip` は「skip が遅れる = Dispatch を余分に回すだけ」で
`ParticleCurves.h:220-222` の不変量「早すぎる skip だけが害」に照らして**安全側**。恒久リークもしない
(バケットは予定 tick に必ず満期する)。`Alive()` は表示専用でハッシュに載らない。
→ `GpuAliveEstimator.h:15-17` のコメントをこの内容へ更新する。

**実装しない — 衝突イベントのスクリプト通知**: GPU 上の衝突を CPU へ届けるには readback が要り
**ADR-008 と正面衝突**する。仮に許しても 1〜3 フレーム遅延し、スクリプトのコールバックは
**ワールド状態を書き換える = ハッシュ対象**なので、GPU 由来・遅延可変・ドライバ依存の信号が
ハッシュへ流れ込む = リプレイが再現しなくなり、バックエンドを切り替えるだけで世界が割れる。
今の「通知が無い」より明確に悪い。筋の通る道は「ゲームプレイ粒子を CPU 物理のブロードフェーズに対して
判定する」別機能。理由込みで `engine_spec.md` 7.5 へ記録する。

---

## 全サブ共通の検証チェックリスト

1. `pwsh -File tools\gen_project_files.ps1` — 新規ファイルを足したサブのみ (M63a / M63d)
2. Debug + Release ビルド (警告 0)
3. `pwsh -File tools\check_rules.ps1` → **error 0** (規則 9 = M63d の新規 SRV スロット 4 本の登録込み)
4. `bin\x64\Debug\Editor.exe --selftest` → **40 スイート全通過** (`RunParticleSelfTest` は 12 番目。
   `EditorMain.cpp:559-590` の `&&` 連鎖は触らない)
5. `Editor.exe --snapshot-stress` — **M63a のみ必須** (snapshot v7 の往復)
6. `tools\replay_verify.bat` → 6 シーン一致 (M63a のみ `.rep` 再記録後)
7. `tools\shot_verify.bat` (`MYE_SHOT_SKIP_*` は立てずにローカル実行)
   - ★**既存 15 枚がビット不変であること**を毎サブ確認 — これが「既定 off」が本当に効いている証拠
   - `particle_cpu` / `particle_gpu` の差分を目視 → 意図通りなら golden 更新
8. `Runtime.exe --particle-demo --particle-compare` — CPU/GPU を横並びで目視。
   回転向き・伸びの方向・コマ位置・陰影の付き方が一致すること
9. `.hlsl` / 新規 `.hlsli` を保存してホットリロードで依存シェーダが再コンパイルされること

※ `Editor.exe` / `Runtime.exe` は GUI サブシステムなので PowerShell から直接叩くと exit code が
取れない — **`cmd /c` を挟むこと**。

## golden 再記録が必要になるタイミング

| 対象 | M63a | M63b | M63c | M63d | M63e |
|---|---|---|---|---|---|
| **`.rep` (6 シーン)** | **要** | 不要 | 不要 | 不要 | 不要 |
| `kSimSnapshotVersion` | **6 → 7** | — | — | — | — |
| `Scene::kDocVersion` / `kReplayFileVersion` | 上げない | — | — | — | — |
| **shot #1 #2 (CI 対象)** | **不変であること** | 不変 | 不変 | 不変 | 不変 |
| shot #10 #15 | 不変 | 不変 | 不変 | 不変 | 不変 |
| shot `particle_cpu` | **新規記録** | 再記録 | 再記録 | 再記録 | 不変 |
| shot `particle_gpu` | **新規記録** | 再記録 | 再記録 | 再記録 | **再記録** |
| `GpuParticleCB` の `static_assert` | 320 (据置) | 据置 | **336** | **352** | **368** |

**「既存 15 枚が 5 サブすべてで不変」がこの設計全体の健全性指標。** どのサブでも動いたら、
そのサブのゲート (フラグ分岐 or RNG 消費ゲート) の実装ミスを疑うこと。

## 実装しない / できないもの

1. **衝突イベントのスクリプト通知** — ADR-008 と正面衝突 + 決定論が壊れる (M63e の節に詳細)
2. **CPU バックエンドの深度衝突** — CPU sim を画面依存にするのは決定論違反 (spec 7.5 の既存例外)
3. **粒子への局所影アトラス / 鏡面 IBL** — 費用対効果 (M63d の節に詳細)
4. **回転の減衰 (角速度ダンピング)** — 閉形式が壊れる。入れるときは sim 状態へ移す
5. **ビルボードスケールへのエミッタスケール適用** — M61g の v1 制限をそのまま踏襲
6. **画面外の任意形状との衝突** — スクリーンスペース法の原理的限界。解析床は proxy であって解決ではない

## 進捗表 (完了時に更新。計画外の事実・ハマった所・申し送りだけ書く)

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M63a 共有契約 + 回転 | **完了** | (このコミット) | 計画外の事実 4 点: ①**既定の procedural ソフト円は点対称なので回しても絵が 1 画素も変わらない** — golden で C1 を検出するには非対称スプライトが必須で、`RegisterParticleShowcaseContent` で矢羽根とフリップブックアトラスを**手続き生成**した (テクスチャ資産はリポジトリに 1 枚も無かった)。②プレフィクスは `vdemo_` — `pdemo_` は --physics-demo が使用済み。③`ParticleSelfTest` の L2 プローブ池が `alive=2` のまま新 SoA を空で残していて**アクセス違反で落ちた** (ハッシュは alive 件を生バイトで畳むため)。SoA を増やしたらプローブ池も必ず埋めること。④`ParticleInstance` は 48B 据え置きのまま — CPU は充填ループで view 行列を持っているので、速度ストレッチを「画面角 + 長軸倍率」の 2 スカラへ畳める (64B 拡張は不要)。**実測: 深度衝突を切ると CPU/GPU golden が maxDiff=0 でビット一致** = 共有 `particle_billboard.hlsli` が効いている証拠。変異テスト実施 (`KillDead` の rotVel swap を 1 行落として selftest FAIL を実証→復元) |
| M63b 速度ストレッチ | **完了** | (このコミット) | 計画外の事実 3 点: ①**長軸倍率は 3D 速度長ではなく画面射影長で測る** (ユーザー決定)。3D 長だとカメラへ真っ直ぐ飛ぶ粒子が「速いので長い線」になるのに向きは atan2(≈0,≈0) 由来で毎フレーム暴れる — 射影長なら同じ状況が閾値に落ちて stretch=1 へビット一致で縮退する。②ローカル空間の速度は `renderWorld` の 3x3 で回してから射影する (ユーザー決定。回さないとエミッタを回した瞬間だけ伸びの向きがズレる)。GPU は `mul(float4(vel,0), gEmitterWorld)`。③M63a が CPU/GPU で**手写ししていた useRotation 判定**を `ParticleUsesRotation/Stretch/Billboard` の 3 本へ寄せた — ストレッチが枠に加わって判定が 2 種類になり、手写しのままだと片方だけ緩める事故が起きる。★`billboardParams` は x(変換を通すか) と w(回転を評価するか) を**分けている** — 束ねると「ストレッチだけ ON」で GPU だけ `rot0+rotVel*e` を評価し CPU はリテラル 0.0f を書く形になり演算列が食い違う。**実測: 既存 15 枚は maxDiff=0 で不変**、CPU/GPU golden 間の差は M63a 由来の 217→221 画素で伸び領域は画素一致 (atan2/sqrt の実装差は現状 0 画素)。`ParticleBillboardExpand` の stretchMax 掛けは `stretchScale != 0` でゲート必須 (既定 4.0 を無条件に掛けると従来カリングされていたプールが可視へ転じる) |
| M63c フリップブック | **完了** | (このコミット) | 冒頭確認は**解決済みだった** — アトラスは M63a が手続き生成済み (コマごとに半径/切り欠き角が違う) で、デモの P3 も 3 フィールドが設定済み = コンテンツ側の追加ゼロ。計画外の事実 3 点: ①**計画の契約「fps=0 で同一演算列へ落ちる」だけではビット保存に足りない** — PS の `age` はラスタライザ補間を通った値で、CPU 充填ループ / GPU の VS が作る値とは別の道を来ている。M63a/M63b と同じくフラグ分岐 (`gFlipMode` / `gParams6.x`) を PS に置いて初めて既存 15 枚が maxDiff=0 になった。②**実測: CPU/GPU golden 間の差は 221 画素のまま不変** (worst pixel の座標まで M63b 時点と同一) = フリップブック領域は 2 バックエンドで画素一致。CPU が充填ループで畳み GPU が VS で作るという非対称でも絵は割れない。③`flipFps < 0` はゲートを開けない — 述語を `> 0` の 1 本に統一して「ゲートは開くが fps は効かない」第 3 の状態を作らない (`ParticleUsesFlipbook` と `ParticleFlipFrameAt` の分岐が同じ式であることが契約)。★申し送り: `SampleFlipTile` の `blendFrames` は **CB 由来 = uniform 前提** (Sample を分岐の中で呼んでいる)。粒子ごとに変える日が来たら 2 サンプル固定へ倒すこと |
| M63d ライティング | 未着手 | | |
| M63e 深度衝突 | 未着手 | | |

## 残る C6〜C8 / D1〜D5 (今回スコープ外、次の 5 つ)

C6 ブレンド 2 種のみ / C7 dissolve 皆無 / C8 distortion が color.a 相乗り /
D1 トレイルにテクスチャ不可 / D2 トレイル UV が弧長でなく age / D3 VfxRenderer 単一ブレンド固定 /
D4 60Hz ハードコード 2 箇所 / D5 スプライトのアトラス UV なし・TextMesh 単一行。
