# M68: 音響伝播 × 実オーディオ (波面の 4 役目 = 遮蔽・回折ローパス・部屋の残響・鳴る波) — 仕様書

- slug: m68-acoustic-audio
- 状態: 確定 (2026-09-06)。planner に `AskUserQuestion` が無い環境なので §2 の疑いは planner が裁定し、
  依頼原文が許した 2 点 (S16 到来方向の平均化 / S17 一発再生の追従整形) だけ `[ユーザーに聞ける]` の印を付けて
  司会へ返す。ユーザー決定 U1〜U4 と設計レビューの修正 5 点 (harness.md) は本書でも蒸し返さない。
- 依頼原文: M68: 音響伝播 × 実オーディオ (波面の 4 役目 = 遮蔽・回折ローパス・部屋の残響・鳴る波)。承認済みの計画 `plans\m68-acoustic-audio\plan.md` を正本に 3 サブ (M68a 場 + 遮蔽 + LPF / M68b 残響 + 鳴る波 + WAV 4 本 / M68c 仕上げ) で実装する。ユーザー決定 4 点 (WAV を焼いてコミット / 残響は 2 プリセット間の連続補間 / 3 サブ / harness) は確定済みで蒸し返さない。判断 1〜7 は Plan エージェントの設計レビュー (2026-09-06) を通した内容。TypeId は 50 (Cloth/SoftBody 予約は 51/52 へ)。planner が選択肢を出すなら「一発再生の追従整形」「到来方向の平均化」の 2 点だけ。sim 状態ゼロ・ABI v15 据え置き・replay 7 ペアと golden 22 枚は全サブで無風が受け入れ条件。
- 正本: `plans/m68-acoustic-audio/plan-original.md` (以下「元計画」。判断 1〜7 / サブ計画 / 検証チェックリスト /
  失敗の切り分け表 / v1 の境界)。本書は元計画を基点コミットのコードと突き合わせ、**食い違った点と埋めた穴だけ**を
  §2 に書き、仕様の書式 (harness-protocol 5.1) に組み直したもの。設計はやり直していない。
- 基点コミット: 8e4272e (M67 完了直後、master、clean)

## 1. 目的 (なぜ作るか)

M65 の波面は「描画・AI・残光」の 3 役を担うが、**プレイヤーの耳には何も届かない** (`--acoustic-demo` に
`AudioSource` / `AudioListener` は 0 個、`Emit` の 3 呼び出し点はどれも鳴らさない、`src\Engine\Engine\Audio\` に
occlusion の語は無い — 元計画 Context、基点で再確認)。鳴らしたとしても壁の向こうの音が素通しで聞こえる。

達成したい状態:
- 隣の部屋の持続音が**壁越しにはこもって小さく**、廊下を進むと**戸口の方向から**次第に開いて聞こえる。
- 見える波・敵が聞く波・耳に届く音の減衰が**同じ距離場 (`acoustic::kNeighbors` + `<11,16,19>`)** から出る。
- 廊下と部屋で残響が**連続に** (段差なく) 変わる。
- 足音・衝撃・投擲・敵の自発音の波が**実際に鳴る**。鳴る範囲 = 波の到達範囲。
- **sim 状態を 1 バイトも増やさない**。既存 7 ペアの replay / 22 枚の golden / ABI v15 / `.rep` 版 / snapshot 版 (11)
  は全サブで無風。これが健全性の指標であり、受け入れ条件そのもの。

## 2. 疑った点と結論

基点で**そのまま使える**と再確認したもの: `AcousticField.h` の公開 API (`DistanceAt / ParentDirAt / FieldOf / Waves /
Grid / HasVolume / Occupancy / IsSolid / StaticSignature / DebugSetGrid`、`WaveField` 公開 struct)、
`AdvanceWaveOneRing` は非 const で `WriteShell` を内包 (`AcousticField.cpp:423-487`)、`Rebuild()` の memcmp 不変条件、
`Emit` の 26 近傍 nudge (表順)、`TraceToOrigin` の上限 `maxRing*2+8`、`parentDir[ni] = OppositeNeighbor(i)` (= 親へ向かう向き)、
`AcousticNav::BuildDistance` (`AcousticNav.cpp:115-180`) が同じ Dial の 2 本目の写しで `StaticSignature()` で焼き直しを判定、
`AudioSpatial` (`AudioSystem.h:47-57`) と全生成箇所 6 件が default 構築、`applyLpf` (`AudioSystem.cpp:1133-1147`)、
`ApplyReverbParams` (`:589-600`、`BuildBusGraph` 末尾 `:374` と `SetReverbPreset` `:730` から呼ばれる)、
`AudioSystem::Play` は `xaudio_ == nullptr || suspended_` で no-op (`:1166`)、`IsReady()` = `xaudio_ != nullptr` で
`--no-audio` は `Init(false)` が早期 return、`AudioSourceSystem::Update` (`AudioSourceSystem.cpp:324-453`) の
`IsReady/IsSuspended` return (`:330`) → `lastTick_` 上書き (`:340`) → per-voice 更新 (`:421-448`)、`st.vel = {}` の
リセット 3 箇所 (`:258` StartSource / `:389` 非 usable / `:404` stream)、`rng_` は専用 Pcg32、`MakeSourcePlay` の
「規則は 1 本」、`TickRunner.cpp:570-576` の `if (!ts.resim)` と `++ctx.tickIndex` が末尾 (`:675`)、
`TickServices` の出力レーン欄 (`TickRunner.h:100-106`、`ts.audioSources` は `:228` で無検査 deref = 常に非 null)、
`kMaxTicksPerFrame = 5` (`EngineLoop.cpp:83`)、配線点 `:287` (`renderSystem.acousticField = &acoustic`) と `:1544`
(`audioSources.Update`)、`bornTick` はハッシュ済み (`WorldHasher.cpp:318`)、NoHash はハッシュから丸ごとスキップ
(`WorldHasher.cpp:179`)、プリセット表の並び (`AudioMixer.cpp:23-26`: 3 = SmallRoom / 6 = Hall = MEDIUMHALL)、
`kReverbPresets` (`AudioSystem.cpp:43-52`)、`ReloadHub.cpp:362` の `ApplyMixer`、`AudioMixerWindow.cpp:376-397` の combo、
`EditorMain.cpp:621-625` の連鎖末尾 (44 本、最後は `RunSourceControlSelfTest`)、TypeId 末尾 49 = AgentBrain
(`Components.cpp:843` の「飛ばし登録はできない」)、`RegisterComponent<T>(name, {...}, kComponentNoHash)` の書式
(`:474-476`)、`--acoustic-dump` の配線 (EditorMain / RuntimeMain / EngineLoop.h / EngineLoop.cpp / RenderSystem.h / .cpp)、
`SynthCore.h` の `SynthParams` (ADSR 付き) / `SynthRender` / `WriteWavToFile`、`ResolveSoundKey` の解決順
(`.sound.json` 名前キー → GUID → 生クリップ)、`MakePlayDesc` / `PickVariationIndex`、`RegisterAssetLibraries` が
`Build*Scene` より前 (`RuntimeMain.cpp:62/122`、`EditorApp.cpp:147/195`)、`MakeLMaze` (24×1×24、`AcousticSelfTest.cpp:46`)、
`docs\test_checklists.md` は M3/M4/M5/M46 の 4 節のみ、README に音響の bullet 無し、`engine_spec.md:1701/1703/1742` の
"six replay pairs / seventeen golden / six scene pairs"、`CLAUDE.md:37` (44 スイート) / `:150-151` (49 / 50/51)、
`plans\supple-weaving-loom.md` の 50/51 予約 (:36 / :120 / :214 / :246 / :290)。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| S1 | `DemoContent.cpp` の場所 | 元計画は `DemoContent.cpp:2783-3115` としか書かないが実体は **`src\Engine\Engine\DemoContent.cpp`** (Engine 層。M67 spec S13 と同じ)。`BuildAcousticShowcaseScene` は `:2783-3117`、`Watcher` は `:3065-3090`、ランプ 3 個 `:3101-3116` が末尾 | 事実 | 触る場所を訂正。追加は全部**関数末尾** (ランプの後) |
| S2 | **波→spatial の `rolloff = 2` (逆二乗) は「聞こえる音」にならない** | `EnergyAt = amp · RolloffGain(2, cell, maxD, d) = amp·(minD/d)²` (`AcousticGrid.cpp:91-101`) は**エネルギー**。XAudio2 の volume は**振幅**で、振幅 = √エネルギー。逆二乗をそのまま振幅に掛けると 10 m で (0.5/10)² = 0.0025 = **−52 dB** (無音)。残光が同じ理由で γ=1/4 を掛けている (`AcousticGrid.h` の `kGlowGamma` 注記「5m で 1/100、20m で 1/1600」) | 裁定 | 波→spatial は **rolloff 0 (Logarithmic = minD/d)**。「波が届く所でだけ聞こえる」は rolloff に依らず成立 (`RolloffGain` は全 rolloff で `d ≥ maxD` → 厳密 0)。関係式は `gain² · amp == EnergyAt` (selftest T13 で固定)。耳で A/B できるよう `AcousticAudio.waveRolloff` (Int32 0..2、既定 0) を持つ |
| S3 | hum の `.sound.json` (元計画: inverse / minDistance 0.5 / maxDistance 30) も S2 と同根 | 廊下 (経路 15 m) で (0.5/15)² = −59 dB。デモの「戸口の方向から聞こえる」が成立しない | 裁定 | hum は **`rolloff "logarithmic"` / `minDistance 2.0` / `maxDistance 40`** (廊下 15 m で −17.5 dB、経路上限 48 m で −27.6 dB)。足音 4 本の 3D 欄は波で上書きされるので値は問わないが同じ流儀で書く |
| S4 | `minWaveVolume` 既定 0.02 では呼吸が鳴る | `WatcherFpsCamera.cpp:96` `breathLoudness = 0.07`、carpet の `acousticLoudness = 0.12` (`assets\physmats\carpet.physmat.json`)。元計画は「呼吸は minWaveVolume で無音」 | 裁定 | 既定 **0.10** (0.07 < 0.10 < 0.12)。carpet 以上の足音は鳴り、呼吸は鳴らない |
| S5 | 受け入れの「dPath ≥ dLine が常に成立」は実座標では**偽** | セル中心の量子化で両端最大 √3·cellSize、3D 角のチャンファ 19 < 11√3 = 19.05 (0.26% 短い) | 事実 | log の `dLine` は**セル中心間**のユークリッド距離 (`dReal` = 実座標間を別欄で併記)。不変条件は **`dPath ≥ 0.99·dLine`** (dPath ≥ 0 の行)。`Direct` の閾値は `detour ≤ cellSize + 0.03·dLine` (自由空間の対角を Direct にする) |
| S6 | 「箱の外 = 逆二乗で 0.001 なので Occluded と段差が無い」は S2 の rolloff 0 では成立しない | 32 m で 2/32 = 1/16 (−24 dB) → Occluded (実位置 × 0.15) へ落ちると −10 dB 級の段 | 裁定 | `probeMaxRing` 既定 **96** (= 48 m)。デモの最長経路 (部屋 A の隅 → 部屋 B の奥 ≈ 40 m) を包むので、デモでは「経路上限超え」が起きない。Dial のコストは**セル数**で決まる (デモ 16k セルで箱は常にグリッド全体) のでリング増はただ。上限超えが起きるシーンでは `smoothTicks` が 1 段のランプにする (仕様上許容、§7) |
| S7 | `AudioListener` を Watcher に直付けするとアーキタイプが変わる (元計画も認識、「動いたら子へ」) | `World::ApplySetParent` (`World.cpp:392-418`) は子・親とも `HierarchyComponent` を**無検査で deref** = 全エンティティが生成時に持つ (`World.cpp:17` の既定 4 種)。つまり子エンティティを付けても **Watcher のアーキタイプは 1 ビットも変わらない** | 裁定 | 最初から**子エンティティ `Watcher Ears`** (関数末尾に append、MeshRenderer 無し、ローカル (0,0,0)) に `AudioListener` を置く。試行→退避の 1 往復を省く。Watcher の yaw は本体に載る (`WatcherFpsCamera.cpp:134` `self.SetLocalRotation`) ので子はそのまま向きを継ぐ。y = 1.35 (グリッド y[0.25,3.25] の中、ジャンプ +0.9 でも内側) |
| S8 | 受け入れ「`--acoustic-audio-log 600` で class が Occluded → Detour → Direct と遷移」は**実機では保証できない** | `--synth-input` は 11 tick ブロックの WASD ランダムウォーク (`Input.cpp SynthLaneInput`)。600 tick の RMS 変位 ≈ 2.6 m で部屋 A を出ない。人が歩く操作を reviewer は再現できない | 裁定 | 遷移の**機械証明は selftest T9** (L 字を歩くリスナーのクラス列が Occluded* → Detour* → Direct*、dPath 単調非増加)。実機ログは**予測を書いてから照合する**: M68a は音源が Hum 1 本 (部屋 B) で Watcher は部屋 A を出ないので (a) `Detour` だけが出て `Direct` / `Occluded` / `Bypass` は **0 行** (密閉音源が無く、経路 ≤ 40 m < 48 m、リスナーは常にグリッド内 — 違えば分類のバグ)、M68b は Walker の足音 (廊下 row 4、col 2..7) が shot として `Direct` (近いタイル) と `Detour` (col 5 以降は壁 (5,1..3) が直線を遮る) の両方を出す (b) 全行 `dPath ≥ 0.99·dLine` (c) hum の tick 間 |Δgain| / |Δlpf| ≤ 0.12 (平滑化の証拠) (d) **同じコマンド 2 回で `[acaudio]` 行がバイト一致** (出力レーンの計算が合成入力の下で決定的)。廊下を歩く耳の確認はユーザー (test_checklists の M68 節) |
| S9 | Profiler の行を出す経路が無い | `ProfilerWindow.cpp` の `ctx` は `EngineContext` (`EngineLoop.h:307-`) で `audio` / `sounds` / `renderSystem` はあるが `AudioSourceSystem` は無い (`grep` で Editor 側の参照は `SceneViewWindow.cpp:21` の `MakeSourcePlay` だけ) | 事実 | `EngineContext::audioSources` (`AudioSourceSystem*`) を足し、`EngineLoop.cpp:390` の隣で埋める。Profiler と終了時 summary はこれ経由 |
| S10 | `--acoustic-audio-log N` は tick 単位だが、実時間実行では frame ≠ tick (Runtime は vsync 無しで数千 fps) | `deterministicShot` (`EngineLoop.cpp:679`) = `--screenshot` 指定 + `--shot-every` 無し → `dt = kFixedDt` (`:1135`) で frame == tick。record/verify 以外で audio は suspend されない (`:841/:952/:1187`) | 事実 | reviewer のレシピは **`--screenshot` モード**で回す (§5 A11)。`--no-audio` を付けると `IsReady()` false で 1 行も出ない (設計どおり = ヘッドレスはゼロコスト) ので付けない |
| S11 | `.sound.json` の `clip` 数値の出所 | `beep.sound.json` の `clip = 12361048743093969237 = 0xab8b44571767a155` = `beep.wav.meta` の guid。`IdForFile` = `assetkey::Resolve(NormalizePathKey)` (`AudioSystem.cpp:799-803`)。名前キー = JSON `name` (`SoundAsset.cpp:95`) | 事実 | `.wav.meta` は手書き (16 hex、既存と重複しない) でもエディタの初回スキャン生成でもよいが、**`.sound.json` の `clip` と一致**させる。デモは GUID を直書きせず `ctx.sounds->ResolveKey(HashStr("hum"))` で引く (チェックアウト先非依存) |
| S12 | 残響の平滑化「≈300 ms」を `smoothTicks` (6 = 100 ms) と共有すると reverb APO のパラメータ更新が速すぎる | `|Δt| > 0.01` でしか `SetEffectParameters` しない設計 = 更新頻度を抑えたい意図 | 裁定 | `roomSmoothTicks` (Int32、既定 18 = 300 ms) を別に持つ |
| S13 | 開放度の分母「同半径の球の総セル数」が未定義 | 占有を無視した自由空間のチャンファ球。閉形式 `chamfer(dx,dy,dz) = 11(a−b) + 16(b−c) + 19c` (a ≥ b ≥ c = |Δ| の降順) | 裁定 | 分母 = 箱内かつ**グリッド内**で閉形式 ≤ R のセル数 (占有無視)。分子 = `dist ≤ R` のセル数。閉形式 = 自由空間の Dial を selftest T11 で固定 |
| S14 | NoHash コンポーネント追加で `.rep` / snapshot の版を上げるべきか | `WorldHasher.cpp:179` が NoHash を丸ごとスキップ。前例 M45e (AudioListener/AudioSource = 28/29) は無 bump (`Components.cpp:471-473`)。snapshot 往復は `replay_verify` 7 ペア目が機械検査する | 事実 | 上げない (`kSimSnapshotVersion` 11 据え置き)。replay 7 ペア無風が証明 |
| S15 | TypeId 50 | 実コードの末尾は 49 = AgentBrain (`Components.cpp:880-950`)。`supple-weaving-loom.md` の 50/51 は計画上の予約で登録ではない | 確定済み (harness.md) | 50 に登録。予約コメント (`Components.cpp:840` / `CLAUDE.md:150-151` / `supple-weaving-loom.md` 5 箇所) を 51/52 へ同じコミットで |
| S16 | **到来方向の平均化** (元計画「最後の 2〜3 歩の平均で和らげる (任意)」) | 仮想位置は `smoothTicks` で指数平滑される = セル遷移の跳びは既に補間される。平均は角の直後 (1〜2 セル) で壁の中を指しうる。1 歩なら T7「到来方向 = 腕の向き」が厳密に書ける | 裁定 (`[ユーザーに聞ける]`) | **v1 は最後の 1 歩のみ**。逆を選ぶと: `ShapeAcousticSpatial` に「最後 k 歩の単位ベクトル平均 (k=3)」が 1 段増え、T7 が「腕の向き ± 1 歩の許容」に緩む。サブは増えない |
| S17 | **一発再生の追従整形** (元計画 v1 境界「Play 時 1 回」) | クリップは ≤ 0.45 s、その間のリスナー移動 ≤ 0.6 m (< 2 セル)。追従には voice ハンドルの表 (`SourceState` に次ぐ 3 つ目の側テーブル) が要る。既存 `PlayAtPoint` も Play 時 1 回 (`AudioSystem.cpp:1209`) | 裁定 (`[ユーザーに聞ける]`) | **v1 は Play 時 1 回・状態なし**。逆を選ぶと: 「(handle, 原点セル, 生成 tick) のリング ≤ 64」を `AudioSourceSystem` に足し、`Update` で `FindByTag` 相当の生存確認をして再整形 = sub-02 に受け入れ条件 1 本 (T22) と側テーブル 1 つが増える |
| S18 | リスナー場の再構築契機が「セル変化 or 署名変化」だけでは足りない | `HasVolume()` false→true (Sync 後) と `Grid()` の変化 (`SameGrid`) がある。`AcousticNav::Sync` (`:51-58`) は署名・比・グリッドの 3 条件 | 事実 | 契機 = `probe 無効 ∨ リスナーセル変化 ∨ 署名変化 ∨ !SameGrid`。`!HasVolume()` またはリスナーがグリッド外なら probe を無効化 (メモリは保持) |
| S19 | reverb override の受け渡し型 | `AudioSystem.h` は `xaudio2fx.h` を include できない (`Windows.h` を引き込む、`AudioSystem.cpp:59` の注記) ので `XAUDIO2FX_REVERB_I3DL2_PARAMETERS` をヘッダに出せない | 事実 | SDK 型と同名 13 フィールドの POD **`AudioReverbParams`** を `AudioSystem.h` に置き、`.cpp` で相互変換 (`static_assert` で件数を SDK に結ぶ)。`LerpReverbParams` はこの POD の純関数 = ヘッドレスで検査できる |
| S20 | (sub-01 round 1、coder 発見) **起動時の `ApplyMixer` が保留したバスグラフ再構築が、フレーム 0 の playOnAwake を殺す** | `RegisterAssetLibraries` → `ApplyMixer` は再構築をフレーム境界へ保留 (`AudioSystem.cpp:1402`)。フレーム 0 は `audioSources.Update` (playOnAwake → `Play`) → `audioSystem.Update` (`RebuildBusGraphNow` → `DestroyAllSourceVoices`) の順 (`EngineLoop.cpp:1544-1550`) なので鳴らした voice が即死し、`started` が立ったままループ音が二度と復活しない。**M45 からの既存不具合**で `--acoustic-demo` に AudioSource が 0 個だったため露見していなかった (実測: hum が 2 tick で無音) | 事実 (coder SELF_EVAL) | メインループ直前に `audioSystem.Update(0.0f)` を 1 回 (dt = 0 なのでフェードもメーターも進まない) = 起動時ミキサーを**最初のフレームより前**に確定させる。spec 側の穴として採用 (§4.1.5 に追記)。replay 7 ペア / golden 22 枚は無風 (出力レーン) |
| S21 | (sub-01 round 1、coder 発見) tick 1 の 1 行だけ場が「壁なし」で焼かれる (`dPath 20.77 / open 1.00`) | `AcousticField::Sync` の占有ベイクは静的コライダの WorldMatrix を見るが、シーン構築直後の最初の tick は行列確定前 = 壁が 1 枚も無い状態で焼く (M65a からの既存挙動、tick 2 で署名が変わり正常化)。**sim 側の性質**で、tick 1 に立つ波が無い限り実害は無い | 裁定 | v1 で許容。出力レーンは場を写すだけなので直す場所はここではない。M65 の追補候補として harness 申し送りに残す (「占有ベイクを最初の transform 更新の後にする」)。A11 の帯は t ≥ 2 で評価 |
| S22 | (sub-01 round 1) spec の経路長見積り 35.8 m は Manhattan 寄りで**誤り** | coder の独立実装 (Python の 26 近傍チャンファ Dijkstra、デモの壁配置) がエンジンのログと小数 2 位まで一致: 静止 (8,2,8)→(28,1,40) = chamfer 651 = **29.59 m**、移動中 26.64〜30.73 m。斜めの近道が 6 m 縮める | 事実 | A11 (b) の帯を **26〜32 m** に訂正、§4.2 の「≈ 35.8 m」→「≈ 29.6 m」。実装は正しい |

## 3. スコープ

- やる: 元計画 判断 1〜7 / M68a〜c の全項目 + §2 で埋めた穴 (S2 `waveRolloff` / S5 の閾値と log 欄 / S7 子エンティティ /
  S8 の selftest T9 と実機ログの 4 条件 / S9 `EngineContext::audioSources` / S12 `roomSmoothTicks` / S13 閉形式 / S19 POD)。
- やらない (元計画「v1 の境界」をそのまま採用): 材質による伝播中の吸収 / 動的コライダの遮蔽 / 部屋ごとの複数 reverb バス /
  複数リスナー / 一発再生の追従 (S17) / ABI 追加 / HRTF・高さ定位 / tone の絵への反映 / 到来方向の平均化 (S16)。
  `AcousticField.h/.cpp` は**コメント 1 行 (`Wave::bornTick` の「診断用」) 以外触らない**。`AudioSystem` の変更は
  `AudioSpatial::lpfCoefficient` + `applyLpf` の min + reverb override 3 関数 + `ApplyReverbParams` の選択 + POD だけ。
- 後回し: 調整値 (`bendFullM` / `lpfFloor` / `occludedGain` / `detourWet` / `waveVolume` / 残響のアンカー) の耳による追い込み。
  すべて Inspector で実行中に触れる (`AcousticAudio` は NoHash なので再生中に変えても replay に影響しない)。
  確定値はユーザーが M68c 後に別コミットで焼く。

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 リスナー場 (`AcousticProbe`、`Audio\AcousticAudio.h/.cpp` 新設)

- 入力: `const AcousticField&` (読むのは `HasVolume / Grid / IsSolid / Occupancy / StaticSignature` のみ)、リスナー実座標 L、
  `probeMaxRing`。出力: `AcousticField::WaveField` (公開 struct を流用。`x0..sz` の箱、`dist` (uint16、`kUnreached`)、
  `parentDir` (kNeighbors の index、`kNoParent`)、`buckets`)、原点セル、`valid`、`signature`、`grid`、`openness`。
- アルゴリズム: 原点 = L のセル。**同じ 26 近傍表 `acoustic::kNeighbors`、同じ重み、同じ「閉セルは訪れない・中間セルは見ない」**
  規則の Dial を**一気に完走** (`AcousticNav::BuildDistance` と同型の 3 本目の写し。`AdvanceWaveOneRing` には触らない)。
  `parentDir[ni] = OppositeNeighbor(i)` (= 親へ向かう向き。`AcousticField.cpp:474-475` と同じ)。
- 箱 = 原点 ± `probeMaxRing` をグリッドで clip。セル数が `kProbeCellBudget = 262144` を超えたら `probeMaxRing` を
  半分ずつ下げて収め、**1 回だけ**警告ログ。`maxDist = probeMaxRing · kFaceCost`。
- 再構築契機 (S18 + coder 追加): `!valid ∨ 原点セル変化 ∨ StaticSignature 変化 ∨ !SameGrid ∨ probeMaxRing 変化`
  (最後は Inspector で実行中に触れるため)。`!HasVolume()` またはリスナーが `WorldToCell` 失敗なら `valid = false` (配列は保持)。**AudioSourceSystem が所有** (`AudioSystem` は `AcousticField` を
  一切 include しない。include の向きは Engine/Audio → Engine/Acoustic の一方向)。
- 開放度 (S13): `R = round(roomProbeM / cellSize) · kFaceCost`。`openness = |{dist ≤ R}| / |{閉形式 chamfer ≤ R かつグリッド内}|`
  (分母 0 なら 0)。再構築時に 1 回計算し probe に保持。

#### 4.1.2 分類と整形 (`ShapeAcousticSpatial`、純関数 1 本)

```
ShapeAcousticSpatial(const AcousticField& field, const AcousticProbe&, const AcousticAudioComponent&,
                     AudioVec3 listenerPos, AudioVec3 sourcePos, AudioSpatial& io, float& gainOut,
                     AcousticShapeState* smooth, float dTicks,
                     AcousticShapeInfo* info /* log 用: class / dPath / dLine / dReal / lpf / gain */)
```
(`field` は手順 2 の nudge に `IsSolid` が要るため、`dTicks` は手順 7 が tick 基準のため。sub-01 round 1 で確定。
probe に `AcousticField*` を持たせる案は寿命の罠なので不採用)
`AudioSourceSystem::Update` の per-voice 更新 (`:421-448`) と一発再生 (波) の drain の**両方がこの 1 本**を呼ぶ
(`MakeSourcePlay` の「規則は必ずこの 1 本だけ」と同型)。

1. `!probe.valid` → **Bypass**: `io` 不変、`gainOut = 1`。
2. 音源セル: `WorldToCell(S)` 失敗 → **Bypass**。`IsSolid` なら `Emit` と同じ表順 26 近傍 nudge (`AcousticField.cpp:527-544` と
   同型)、開セルが無ければ **Occluded**。
3. 箱外 / `dist == kUnreached` → **Occluded**。
4. `dPath = ChamferToMeters(dist)`、`dLine = |center(S セル) − center(L セル)|` (S5)、`dReal = |S − L|`、
   `detour = max(0, dPath − dLine)`。`detour ≤ cellSize + 0.03·dLine` → **Direct**、それ以外 **Detour**。
5. 到来方向 (Detour のみ): S セルから `parentDir` を辿って L セルへ (上限 `probeMaxRing·2 + 8` 歩)。辿り着いた先の
   `dist` が 0 でなければ鎖が壊れている → Occluded。**最後の 1 歩** (L に入る一歩) の逆向き `kNeighbors[OppositeNeighbor(lastDir)]`
   を正規化したものが `dir` (S16)。S セル == L セルなら Direct。
6. 目標値:
   | class | position | lpf | gain | dopplerScale | reverbSend (M68b) |
   |---|---|---|---|---|---|
   | Direct | S (実座標) | 1 | 1 | 不変 | 不変 |
   | Detour | `L + dir · dPath` | `clamp(1 − detour / bendFullM, lpfFloor, 1)` | 1 | **0** | `min(1, io.reverbSend + detourWet)` |
   | Occluded | S (実座標) | `occludedLpf` | `occludedGain` | 不変 | `min(1, io.reverbSend + detourWet)` |
   | Bypass | 不変 | 不変 | 1 | 不変 | 不変 |
7. 平滑化 (`smooth != nullptr` のとき): `alpha = smoothTicks > 0 ? 1 − pow(0.5, dTicks / smoothTicks) : 1`
   (`kVelocityHalfLifeTicks` と同型の**整数 tick 基準の半減期**)。`!smooth->valid` → 目標へ**スナップ**して `valid = true`。
   gain / lpf / position の 3 つを平滑化。`dTicks = tickIndex − lastTick_` は `:340` の上書き**前**に取る (最小 1)。
   `smooth == nullptr` (一発再生) → 目標をそのまま書く。**Bypass のときは `*smooth = {}`** (次に有効になった tick で
   スナップさせる。残すとグリッドへ入り直した瞬間に前回の遮蔽値から数百 ms かけて戻る。sub-01 round 1)。
8. ブレンド: `gainOut = Lerp(1, gain, io.spatialBlend)`。lpf は `applyLpf` 側の `Lerp(1, c, blend)` に任せる (二重に掛けない)。
   `spatialBlend == 0` の音は既存ゲート (`ApplyVoiceSpatial` を呼ばない) のまま = 2D 音を遮蔽しない。
   `io.pitch` / `io.velocity` / `io.minDistance` / `io.maxDistance` / `io.rolloff` は触らない。

`AudioSourceSystem::Update` 側: `desc.volume *= gainOut` → `SetVoiceVolume`、`spatial.lpfCoefficient = lpf` → `ApplyVoiceSpatial`。
`SourceState::shape` (`AcousticShapeState`) は `st.vel = {}` の 3 箇所で一緒に `{}` へ。`stream` (BGM) の音源は既存の
`continue` で整形の前に抜ける。有効な `AcousticAudio` (enabled かつ active、最小 `entity.index`) が無い tick は全部 Bypass。

`AudioSpatial::lpfCoefficient` (float、既定 1.0、**末尾追加**): `ApplySpatialToVoice` の直達側だけ
`applyLpf(dry, min(dsp.LPFDirectCoefficient, s.lpfCoefficient))`。リバーブ送り側の LPF 行は触らない。

#### 4.1.3 部屋の残響 (M68b)

- `t_target = smoothstep(openSmall, openLarge, probe.openness)`、`roomSmoothTicks` 半減期で平滑化 (`t`)。
- `|t − appliedT| > 0.01` のときだけ `audio.SetReverbOverride(LerpReverbParams(P(reverbSmall), P(reverbLarge), t))` して
  `appliedT = t`。`P(i)` = `AudioSystem::PresetReverbParams(i)` (`kReverbPresets[i]` の POD 写し。範囲外は 0)。
- `LerpReverbParams(a, b, t)`: 13 フィールド全部を線形補間 (mB の int32 は対数域なので線形でよい。int は float で補間して
  四捨五入)。`t=0` → a と全フィールド一致、`t=1` → b と一致。
- `AudioSystem`: `SetReverbOverride(const AudioReverbParams&)` / `ClearReverbOverride()` / `bool ReverbOverrideActive() const`。
  **`ApplyReverbParams()` の 1 箇所**で「override があればそれ、無ければ `kReverbPresets[reverbPreset_]`」を選ぶ。
  `SetReverbOverride` は保持して `ApplyReverbParams()` を呼ぶ。`reverbPreset_` / `CurrentMixer()` / combo は資産値を保つ。
  `.mixer.json` ホットリロード (`ApplyMixer → RebuildBusGraphNow → BuildBusGraph 末尾`) 後も自動で再適用される。
- Clear の契機: `AudioSourceSystem::Reset()`、有効な `AcousticAudio` が無い tick、probe が無効な tick。

#### 4.1.4 鳴る波 (M68b)

- `PendingWaveShot { int32 ox, oy, oz; uint32 tone; float amplitude; uint32 maxRing; uint64 bornTick; EntityID source; }` (POD)。
- push (`TickRunner.cpp:570-576` の `if (!ts.resim)` ブロック内、`ApplyScriptAudioEvent` のループの**後**):
  `ts.acoustic != nullptr && audioSystem.IsReady() && !audioSystem.IsSuspended()` のとき `ts.acoustic->Waves()` を舐め、
  `active && bornTick == ctx.tickIndex` の波を `audioSources.PushWaveShot(...)` へ。`bornTick == tickIndex` が正しいのは
  `++ctx.tickIndex` が末尾 (`:675`) だから。**なぜ tick 側で push か**: `kMaxTicksPerFrame = 5` でフレーム単位の走査は
  4 tick で消える衝撃波を取りこぼす。**なぜ Update 側で drain か**: probe を更新した後に同じフレームで鳴らせる。
- drain (`AudioSourceSystem::Update`): **先頭で無条件に取り出して clear** (`:330` の early return より**前**。溜めると
  検証解除後に一斉に鳴る)。`Reset()` でも clear。early return を抜けた後、probe 更新の後で各 shot を処理:
  `vol = amplitude · waveVolume`; `vol < minWaveVolume` → 捨てる (数える)。`key = HashStr(toneSound[tone])` →
  `ResolveSoundKey`; 無効なら tone ごとに 1 回だけ `[audio] unknown sound key` 警告して捨てる。`asset->stream` → 捨てる。
  `MakePlayDesc(asset, PickVariationIndex(asset, rng_.NextU32()), rng_.Range(-1,1), rng_.Range(-1,1), audio)` (乱数は
  **`rng_`**。`audioScriptRng` は触らない)、`desc.volume = clamp(desc.volume · vol, 0, 1)`。生クリップなら `DefaultBus`、
  `volume = clamp(vol)`。spatial は**波から**: `position = CellToWorldCenter(o)`、`spatialBlend = 1`、
  `minDistance = cellSize`、`maxDistance = maxRing · cellSize` (= `ChamferToMeters(maxRing·11)`)、`rolloff = waveRolloff`、
  `dopplerScale = 0`、`reverbSend = waveReverbSend`、`pitch = desc.pitch` → `ShapeAcousticSpatial(..., smooth = nullptr)` →
  `desc.volume *= gainOut`、`desc.spatial = &spatial` → `audio.Play(desc)`。この組み立ては純関数 `MakeWaveShotPlay(...)`
  に切り出し、`Play` だけを呼び手が行う (T19〜T21 がデバイス無しで検査する)。
- 「波が届く所でだけ聞こえる」の根拠: `RolloffGain` は全 rolloff で `d ≥ maxD` → 0 (`SpatialMath.h:36-41`)。

#### 4.1.5 ゲート (共通)

| 状況 | 挙動 | 根拠 |
|---|---|---|
| `--no-audio` / デバイス無し | `Update` が `IsReady()` で return → probe も整形も log も走らない (ゼロコスト)。Profiler 行も出ない | `AudioSystem::Init(false)` |
| 記録 / 検証 / タイムトラベル | `IsSuspended()` で return。shot キューは return より前に clear。`Play` 自体も no-op | `EngineLoop.cpp:809/841/932/952/1187`、`AudioSystem.cpp:1166` |
| 再シム (`ts.resim`) | push しない (`!ts.resim` ブロック内) = ロールバックで二重に鳴らない | `TickRunner.cpp:570` |
| ネットロックステップ | 止めない (出力レーン) | `TickRunner.h:123-128` |
| 有効な `AcousticAudio` 無し | Bypass + override Clear + キューは捨てる | 4.1.2 / 4.1.3 |
| 起動時ミキサーの保留再構築 (S20) | メインループ直前に `audioSystem.Update(0.0f)` を 1 回 = フレーム 0 の playOnAwake が `DestroyAllSourceVoices` に殺されない | `EngineLoop.cpp` メインループ直前 |

#### 4.1.6 CLI / Profiler / ログ (reviewer が耳なしで検証する口)

- `--acoustic-audio-log N` (Editor / Runtime 共通、`--acoustic-dump` と同じ配線 = `EditorMain.cpp:382` / `RuntimeMain.cpp:370` /
  `EngineConfig` / `EngineLoop.cpp` → `audioSources.SetAcousticAudioLog(N)`): tick < N のあいだ、整形した voice と shot ごとに
  標準出力へ 1 行:
  ```
  [acaudio] t=<tick> kind=<voice|shot> src=<entity.index|-1> name=<entity name|tone<k>> class=<Direct|Detour|Occluded|Bypass> dPath=%.2f dLine=%.2f dReal=%.2f lpf=%.3f gain=%.3f open=%.2f
  ```
  (M68b で末尾に ` room=%.2f` (平滑化後の t) を足す)。`dPath` / `dLine` は Occluded (未到達) と Bypass で `-1.00`。
  値は**平滑化後**。
- 終了時 (`--acoustic-audio-log` > 0 のとき、`EngineLoop.cpp:1832` の `[rt]` 行の隣):
  `[acaudio] summary: ticks=%llu rebuilds=%d boxCells=%d probeMsAvg=%.3f shaped=%d classes D/T/O/B=%d/%d/%d/%d`
  (M68b で ` shots=%d skipped=%d unknownKey=%d` を足す)。run-to-run 比較では `probeMsAvg` を除く。
- Profiler (`ProfilerWindow.cpp:87-101` の隣、有効な `AcousticAudio` と有効な probe があるときだけ):
  `  acoustic-audio: probe %6.3f ms (rebuilds %d, box %d cells, shaped %d, open %.2f)` (M68b で `, shots %d, room t=%.2f`)。
  統計は `AudioSourceSystem::AcousticAudioStats()` (POD) から。

### 4.2 データ・保存形式・互換性

- **`AcousticAudioComponent`** (`Components.h/.cpp`、TypeId **50**、`kComponentNoHash`、`RegisterBuiltinComponents` の末尾に append)。
  全フィールド M68a で確定、後から動かさない。`MYE_JP` 付き、範囲付きは `MYE_FIELD_RANGE`:

  | フィールド | 型 | 既定 | 範囲 | 意味 |
  |---|---|---|---|---|
  | enabled | Bool | true | | 全体スイッチ |
  | probeMaxRing | Int32 | 96 | 1..256 | リスナー場の半径 [セル] (S6) |
  | bendFullM | Float | 8.0 | 0.5..64 | detour がこの長さで lpf が床に着く |
  | lpfFloor | Float | 0.25 | 0..1 | 回折 LPF の下限 |
  | occludedGain | Float | 0.15 | 0..1 | 密閉時の gain |
  | occludedLpf | Float | 0.10 | 0..1 | 密閉時の lpf |
  | smoothTicks | Int32 | 6 | 0..120 | gain/lpf/位置の半減期 [tick]。0 = スナップ |
  | roomProbeM | Float | 6.0 | 1..32 | 開放度の半径 [m] |
  | openSmall | Float | 0.2 | 0..1 | 開放度 → t の下端 |
  | openLarge | Float | 0.6 → **0.8** (sub-02 で変更) | 0..1 | 同 上端。sub-01 実測: 部屋 A (8×6 m) で 0.33〜0.62 → 0.6 では部屋 A が上端に張り付き部屋 B と区別がつかない (変更履歴 #7) |
  | roomSmoothTicks | Int32 | 18 | 0..300 | t の半減期 [tick] (S12) |
  | reverbSmall | Int32 | 3 | 0..10 | 狭い側のプリセット index (SmallRoom) |
  | reverbLarge | Int32 | 6 | 0..10 | 広い側 (Hall) |
  | detourWet | Float | 0.25 | 0..1 | Detour/Occluded に足す送り |
  | waveVolume | Float | 1.0 | 0..4 | 波の振幅 → 音量の係数 |
  | minWaveVolume | Float | 0.10 | 0..1 | これ未満は鳴らさない (S4) |
  | waveReverbSend | Float | 0.35 | 0..1 | 波の送り |
  | waveRolloff | Int32 | 0 | 0..2 | 波の減衰カーブ (S2。0=Log 1=Linear 2=Inverse) |
  | toneSound0..3 | String64 | "" | | tone → `.sound.json` 名 (または生クリップの stem) |

  最小 `entity.index` の active な 1 個が勝つ (`AcousticVolume` と同じ)。`kSimSnapshotVersion` / `.rep` 版 / `MYE_API_VERSION` は据え置き (S14)。
- `AudioSpatial::lpfCoefficient` (float、1.0、末尾)。生成箇所 6 件は default 構築なので影響なし。
- `AudioReverbParams` (POD、`AudioSystem.h`): I3DL2 と同名 13 フィールド (WetDryMix / Room / RoomHF / RoomRolloffFactor /
  DecayTime / DecayHFRatio / Reflections / ReflectionsDelay / Reverb / ReverbDelay / Diffusion / Density / HFReference)。
  `AudioSystem.cpp` に SDK 型との `static_assert(sizeof == sizeof)` と相互変換。
- `AcousticProbe` / `AcousticShapeState` / `PendingWaveShot` / `AcousticAudioStats` は ECS に置かない (`SourceState` と同じ側テーブル)。
- アセット (`assets\audio\`、U1): `hum.wav` (M68a) + `step_soft / step_wood / step_hard / step_metal.wav` (M68b)、各 `.sound.json`、
  各 `.meta` ×2 (S11)。**SynthCore で焼き、最終 `SynthParams` を実装メモに書く** (再現可能にする)。目安:
  soft = Noise 0.12 s amp 0.35 短い減衰 / wood = Triangle 180→90 Hz 0.16 s / hard = Square 900→300 Hz duty 0.3 0.10 s /
  metal = Sine 1800→1500 Hz 0.45 s 長い減衰 / hum = Sine 110 Hz **2.0 s ちょうど** (220 周期 = 継ぎ目なし、attack/decay/release 0、
  sustain 1)。合計 ~250 KB。`.sound.json`: bus `SE` / `spatialBlend 1` / `dopplerScale 0` / `reverbSend 0.35`;
  hum は `loop true` / `rolloff "logarithmic"` / `minDistance 2.0` / `maxDistance 40` (S3); 足音は `pitchRandom 0.08` /
  `volumeRandom 0.1` / `minDistance 1.0` / `maxDistance 30` / `rolloff "logarithmic"`。`default.mixer.json` は触らない。
- tone の割り当て (physmat の `acousticTone`): 0 = carpet/water/呼吸 → `step_soft`、1 = wood/敵の自発音/WavePinger → `step_wood`、
  2 = gravel → `step_hard`、3 = metal/glass → `step_metal`。
- デモ (`BuildAcousticShowcaseScene` の**末尾**、ランプ 3 個の後に append、この順): (1) `Watcher Ears` (Watcher の子、
  `AudioListenerComponent`、MeshRenderer 無し) (2) `Acoustic Audio` (`AcousticAudioComponent`。M68b で `toneSound0..3`)
  (3) `Hum` (`AudioSourceComponent` sound = `ResolveKey(HashStr("hum"))`、`playOnAwake 1`、MeshRenderer 無し、部屋 B の
  map (6, 9) = world (1.0, 1.0, 7.0))。部屋 A の隅 (Watcher 開始点) からの経路 **≈ 29.6 m** (S22。チャンファは対角で
  縮む) / セル中心間の直線 19.1 m → Detour (lpf 床、gain ≈ 0.068)。縦廊下 (9,5) で detour 数 m → lpf が開き始め、
  戸口 (9,7) で Direct。

### 4.3 UI / ビジュアル

- Inspector: `AcousticAudio` は自動生成 (FieldDesc)。追加 widget なし。
- ミキサー窓 (`AudioMixerWindow.cpp:382` の combo の後): `audio.ReverbOverrideActive()` のとき `ImGui::SameLine()` +
  `ImGui::TextDisabled("%s", Tr(StrId::Mixer_AcousticOverride))`。文字列は `LocalizationTable.inl` に
  `MYE_STR(Mixer_AcousticOverride, "(acoustics override this preset)", "(音響が上書き中)")` (`###` 不要、書式指定子なし)。
- 絵は 1 画素も変えない (golden 22 枚 maxDiff=0 が条件)。

### 4.4 非機能

- 決定論: sim 状態ゼロ。`world.Rng()` に触れない。`AcousticField` の diff はコメント 1 行のみ (`git diff -- src/Engine/Engine/Acoustic`)。
  出力レーンの計算は合成入力の下で決定的 (S8 (d))。
- コスト (sub-01 実測、箱 16224 セル): probe 再構築 **Debug 9.3 ms / Release 0.65 ms** per 回、合成入力の歩行で
  6.6 回/秒 (600 tick で 66 回) = Debug で 60 ms/s (1 フレーム 16.7 ms に 1 回 9 ms のヒッチ、Release は無視できる)。
  v1 で許容。`--no-audio` (CI) はゼロコスト。詰めるなら `assign` の毎回確保をやめてバケットの capacity を使い回す (後続)。
- include の向き: Engine/Audio → Engine/Acoustic の一方向。`AcousticField.h` から `AcousticAudio.h` を include しない。
  `AudioSourceSystem.h` は `class AcousticField;` の前方宣言だけ。
- ABI: `EngineAPI.h` / `Interop.cs` / `MYE_API_VERSION` に diff なし (v15)。スクリプトからの調整は v11 `SetComponentField`。
- ローカライズ: 新規 `Tr()` は `Text("%s", Tr(x))` 形 (規則 10)。
- 規約: コメントは日本語で「なぜ」を書く。新規 .h/.cpp は `gen_project_files.ps1`。selftest は `RunAcousticAudioSelfTest()` を
  連鎖末尾に append (45 本目)。

## 5. 受け入れ条件

全サブ共通 (各サブのコミット前に全部回す。exe は PowerShell ツールから `cmd /c`):

| # | 条件 | 検証手段 |
|---|---|---|
| A1 | selftest **45 スイート** ALL PASS (M68a から) | `cmd /c bin\x64\Debug\Editor.exe --selftest` |
| A2 | replay 7 ペア / 10 ジョブ無風 | `tools\replay_verify.bat` |
| A3 | golden **22 枚 maxDiff=0** (Release ビルド先行) | `tools\shot_verify.bat` |
| A4 | 規則検査 0 error | `pwsh -File tools\check_rules.ps1` |
| A5 | Debug / Release 0 警告 + C# 両構成 | `MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true` で 8 ビルド、`tools\build_managed.bat Debug` / `Release` |
| A6 | `src\Engine\Engine\Acoustic\` の diff が M68 全体で `AcousticField.h` のコメント 1 行のみ | `git diff 8e4272e -- src/Engine/Engine/Acoustic` |
| A7 | ABI v15 据え置き | `git diff 8e4272e -- src/Shared/EngineAPI.h src/Scripting` が空 |
| A8 | `AcousticAudioComponent::sTypeId == 50`、NoHash、`kSimSnapshotVersion == 11` | selftest T1 + コード読み |
| A9 | `AudioSpatial` / `AcousticAudioComponent` の追加フィールドは末尾 | コード読み (reviewer) |

sub-01 (M68a):

| # | 条件 | 検証手段 |
|---|---|---|
| A10 | selftest T1〜T15 (sub-01.md) が全部 PASS | A1 の中 |
| A11 | 実機ログ: 次を 2 回回し (`<scratch>` は任意)、(a) **予測** (S8): 音源は Hum 1 本で Watcher は部屋 A を出ないので `class=Detour` が 550 行以上、`Direct` / `Occluded` / `Bypass` は **0 行** (Direct の実機検証は M68b の shot 行 = A16 (b)。selftest では T8/T9) (b) `dPath ≥ 0` の全行 (t ≥ 2、S21) で `dPath ≥ 0.99·dLine`、かつ Hum の `dPath` が **26〜32 m** の帯 (S22: 静止 29.59 m、部屋 A 内の移動で 26.6〜30.7 m、経路上限 48 m 未満) (c) `name=Hum` の行で tick 間の `|Δgain| ≤ 0.12` かつ `|Δlpf| ≤ 0.12` (初回行は除く) (d) 2 run の `[acaudio] t=` 行が**バイト一致** (summary 行は除く) (e) summary の `rebuilds ≥ 2` (Watcher がセルをまたいでいる) | `cmd /c bin\x64\Release\Runtime.exe --acoustic-demo --synth-input --acoustic-audio-log 600 --screenshot <scratch>\acaudio.png --shot-frame 600 --frames 601 --font-embedded > <scratch>\acaudio_1.txt` (2 回目は `_2.txt`)。`--no-audio` は付けない |
| A12 | `--no-audio` を付けた同じ run で `[acaudio]` 行が 0 (ヘッドレスはゼロコスト) | 上のコマンド + `--no-audio` |
| A13 | Profiler 行が出る (有効な AcousticAudio があるとき) / 出ない (`--no-audio`) | コード読み + A12 |
| A14 | `hum.wav` / `hum.sound.json` / `.meta` ×2 がコミットに含まれ、起動ログに `[audio] unknown sound key` が無い | A11 のログ |

sub-02 (M68b):

| # | 条件 | 検証手段 |
|---|---|---|
| A15 | selftest T16〜T21 PASS | A1 |
| A16 | A11 と同じ run で (a) `kind=shot` ≥ 20 行 (Walker の足音 + 衝撃 + 敵の自発音) (b) shot 行の `class` に `Direct` と `Detour` の両方 (c) `room=` の tick 間 `|Δroom| ≤ 0.05` (d) summary の `shots` ≥ 20、`unknownKey == 0` (e) 2 run バイト一致 | A11 のコマンド |
| A17 | `replay_verify` 中に 1 音も鳴らず、解除後に一斉に鳴らない | `IsSuspended` return より前の clear = T18 + `Update` のコード読み |
| A18 | ミキサー窓の combo は資産値のまま、override 中は「音響が上書き中」が出る。`LocalizationSelfTest` PASS | コード読み + A1 (目視はユーザー) |
| A19 | 足音 4 本の `SynthParams` が実装メモに書いてある (再現可能) | sub-02.md |

sub-03 (M68c):

| # | 条件 | 検証手段 |
|---|---|---|
| A20 | `engine_spec.md` §10.6 末尾に `**Audible output (M68).**` 段落、`:1701/1703/1742` の six/seventeen が seven/twenty-two 系に直る | `grep -n "six replay\|seventeen golden\|six scene" engine_spec.md` が空 |
| A21 | README `## 主要機能` に音響 (M65+M68) の bullet、「6 シーン」「6 ペア」→ 7 | `grep -n "6 シーン\|6 ペア" README.md` が空 |
| A22 | `docs\adr\ADR-017-acoustic-audio.md` (ADR-016 の様式、却下案つき) | ファイル存在 + 節構成 |
| A23 | `docs\test_checklists.md` に `## M68` 節 (`- [ ] 操作 → 期待`) | ファイル |
| A24 | `CLAUDE.md` 検証表 / CLI / チェックリストが M68 後の実態と一致 | 読み合わせ |
| A25 | `plan-original.md` 進捗表 + 申し送り | ファイル |

ユーザー確認 (reviewer のゲートではない): 廊下を歩いて hum がこもり → 開き → 戸口側に定位すること、廊下と部屋 B で残響が
段差なく変わること、足音が床材で遠近が変わること (M68c の checklists の項目)。

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | リスナー場 + 遮蔽・回折の整形 + `AcousticAudio` (TypeId 50) + selftest 45 本目 + hum | なし | A1〜A14 | `M68a: 音響 × オーディオ — リスナー場 (Dial の 3 本目) + 遮蔽・回折の整形 (仮想発音位置 / LPF) + AcousticAudio (TypeId 50)` |
| sub-02 | 部屋の残響 (連続補間) + 鳴る波 (`PendingWaveShot`) + 足音 WAV 4 本 | sub-01 | A1〜A9, A15〜A19 | `M68b: 音響 × オーディオ — 部屋の残響 (2 プリセット連続補間) + 鳴る波 (PendingWaveShot) + 足音 WAV 4 本` |
| sub-03 | 仕上げ (ADR-017 / engine_spec / README / test_checklists / CLAUDE.md / 進捗表) | sub-02 | A1〜A9 (無風の再確認), A20〜A25 | `M68c: 音響 × オーディオ — 仕上げ (ADR-017 / engine_spec §10.6 / README / test_checklists / CLAUDE.md)` |

コミット本文に「ABI 変更なし / sim 状態変更なし」を明記 (元計画の手順 3)。sub-01 のコミットに `plans/m68-acoustic-audio/`
(harness.md / plan-original.md / spec.md / sub-*.md) を含める (M67 の `73a439e` と同じ)。

## 7. 未決事項・リスク

- `[ユーザーに聞ける]` S16 到来方向の平均化 — 裁定: 最後の 1 歩のみ。逆なら整形に 1 段 + T7 の許容が緩む。
- `[ユーザーに聞ける]` S17 一発再生の追従整形 — 裁定: Play 時 1 回。逆なら sub-02 に側テーブル 1 つ + T22。
- 調整値は耳で決まる (§3 後回し)。既定は S2〜S4 の計算に基づく初期値であって最終値ではない。
- 経路上限超え (S6) はデモでは起きないが、大きなシーンでは `Detour → Occluded` の段が `smoothTicks` のランプで出る。
  滑らかにしたければ「上限付近で gain を `RolloffGain(dPathMax)` へ寄せる」を後続で足す (v1 では割り切り)。
- probe の再構築が歩行中 3〜4 回/秒 × 数 ms。大グリッド (256³ 級) では `kProbeCellBudget` がリングを下げるので到達範囲が
  縮む (警告ログ 1 回)。
- `--synth-input` は Watcher を部屋 A で彷徨わせるだけ (S8)。A11 は「配管が実機で通っている」の証明であって「遷移が聞こえる」の
  証明ではない。後者は T9 とユーザーの耳。
- WAV を焼く一時プローブ (`SynthRender` → `WriteWavToFile`) はコミットしない。`SynthParams` を実装メモに残すことが再現性の担保。

## 8. 変更履歴

(確定後の変更のみ。出所と理由)

1. 2026-09-06 sub-01 round 1 (coder SELF_EVAL [追加] 1) — §2 S20 / §4.1.5: 起動時ミキサーの保留再構築がフレーム 0 の
   playOnAwake を殺す M45 の既存不具合。`audioSystem.Update(0.0f)` をメインループ直前に 1 回。spec の穴 (hum が鳴る前提だった)。
2. 同 ([追加] 2) — §4.1.2: `ShapeAcousticSpatial` の署名に `const AcousticField& field` と `float dTicks`。spec の署名は見立てで、
   nudge に `IsSolid` が要ることと半減期が tick 基準であることを署名に出していなかった。
3. 同 ([追加] 3) — §4.1.1 / S18: 再構築契機に `probeMaxRing` の変化。Inspector で実行中に触れる値なので spec の漏れ。
4. 同 ([追加] 6) — §4.1.2 手順 7: Bypass で平滑化状態を `{}` に落とす。「`valid=false` → スナップ」の意図 (密閉音源が大きく
   鳴ってから消えない) を Bypass 復帰にも及ぼす。
5. 同 (不安・質問 1) — §2 S22 / §4.2 / §5 A11 (b): 経路長 35.8 m → 29.6 m、帯 28〜40 → 26〜32 m。planner の Manhattan 寄りの
   見積りが誤り。coder の独立 Dijkstra がエンジンと小数 2 位まで一致。
6. 同 (不安・質問 2) — §2 S21: tick 1 の占有未ベイク (M65a 継承) を v1 で許容。A11 (b) は t ≥ 2 で評価。
7. 同 (申し送り) — §4.2 表: `openLarge` 既定 0.6 → 0.8 を **sub-02 で適用** (部屋 A の実測 0.33〜0.62 が上端に張り付く)。
   `openSmall` 0.2 は廊下の実測が無いので据え置き、sub-02 で廊下の `open` が ≥ 0.25 なら coder が報告する。
8. 同 (不安・質問 4) — §4.4: probe 再構築の実測 (Debug 9.3 / Release 0.65 ms) を記録。「Debug 数 ms」は過小だったが v1 で許容。
9. 同 ([逸脱] 4 / [追加] 5 / [追加] 7) — アクセサ名 `AcousticStats()`、summary の `ticks > 0` ゲート、sub-01 T10 の床到達を
   `bendFullM = 4` の複製で検査 (L 字 12 m 四方では回り込みが 5 m 弱で既定 8 では床に届かない = 既定値では書けない主張だった)。
