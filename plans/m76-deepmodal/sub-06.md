# sub-06 (M76f): ランタイム接続 — ModalSound / 接触 → 音 / wave 口封じ / CLI / demo

- 依存: sub-01 (合成器)、sub-05 (ライブラリ)
- 状態: 未着手
- 往復: 0

## やること
spec §4.1「経路 (ランタイム)」全部 (ユーザー計画 Phase 12–14 / Checkpoint J・K・L)。

- `Core/Components.h` + `Core/Components.cpp` の `RegisterBuiltinComponents()` **末尾** (UIToggleGroup = 60 の後): `ModalSoundComponent` (TypeId 61、`kComponentNoHash`。`kComponentUiAux` は付けない)。
  フィールド: `AssetID mesh` (空 = 同 entity の MeshRenderer.mesh) / `gain=1` / `maskThreshold=0` / `cooldownTicks=3` / `sizeScale=1` / `maxDistance=30` / `muteWave=1`。`MYE_JP` 表示名。
- `Acoustic/AcousticGrid.h` (宣言) / `AcousticField.cpp` (定義): `acoustic::RestingImpulse(World&, EntityID ea, EntityID eb, float gMag, float dt)` を AcousticField.cpp:951-957 の `dynamicMass` + 984-985 の式から抽出し、`DrainImpacts` はそれを呼ぶ。**評価順を 1 文字も変えない** (浮動小数の結合順も)。
- 新規 `Audio/ModalAudio.h/.cpp`: `PendingModalImpact` POD (source entity / mesh / 接触点ワールド / ローカル k[3] / excessImpulse / tick / key) / `ModalAudioStats` / `ModalShotResult { Played, NotReady, NoModel, Cooldown, BelowMin, PoolFull, PlayFailed }` /
  純関数 `CollectModalImpacts(World&, const vector<SolidContact>&, float dt, uint64_t tick, vector<PendingModalImpact>&)` (index → EntityID の表は `DrainImpacts` 923-940 と同じ作り方。key 昇順、両側 ModalSound は両方) /
  `MakeModalShotPlay(const ModalFeatureMap&, const DmNetHeader&, const PendingModalImpact&, const ModalSoundComponent&, const PhysMat* /*null = 参照材質*/, float worldScaleOfLongestAxis, AudioClip&, AudioSpatial&, ModalShotInfo*)` (cell 選択 → `BuildModes` → `ModalSynthRender`。**Inspector のプレビューもこれを呼ぶ**)。
  PhysMat は entity の `ColliderComponent.physMaterial` → `physmat::Resolve`。σ3 の `L_obj` = ローカル最長辺 × その軸のワールドスケール × `sizeScale`。
- `TickRunner.cpp`: 608-645 の `!ts.resim` ブロック内、wave の push の後に `CollectModalImpacts(world, solidContacts, ctx.fixedDt, ctx.tickIndex, tmp)` → `audioSources.PushModalImpact` (同じ門 `audioSystem.IsReady() && !IsSuspended()`)。
- `Audio/AudioSourceSystem.h/.cpp`: `SetModalLibrary` / `PushModalImpact` (上限 `kMaxPendingModalImpacts = 64`、溢れは数える) / drain を wave shot の後ろ (621-676 の隣) /
  側テーブル `ModalEntityState { EntityID; lastShotTick; }` (sorted vector) / クリップ池 (`kModalClipSlots = 32`、`HashStr("modal://slot#k")`、ラウンドロビン、`endTick` で「鳴っている音を切らない」) / `kMaxModalShotsPerTick = 4` / `ModalStats()` / `Reset` でキューと側テーブルを空に。
  drain 1 件: cooldown → `lib->Request(mesh)` (Ready 以外は結果を数えて続行しない = wave がそのまま鳴る) → PhysMat → `MakeModalShotPlay` → slot → `RegisterClip` → `PlayDesc { bus=SE, volume=1, priority=128 }` + `AudioSpatial { position=接触点, minDistance=1, maxDistance=comp, rolloff=0, dopplerScale=0, reverbSend=AcousticAudio.waveReverbSend or 0 }` → acOn なら `ShapeAcousticSpatial(..., nullptr, 1, &info)` (**規則は 1 本**) → `Play`。初回 `NotReady` で「`--modal-bake` を促す WARN」を 1 回。
- `Audio/AcousticAudio.cpp:514` `ResolveWaveShotSound` の先頭に (0): 発音元が `ModalSound` を持ち `muteWave != 0` かつ `modalsound::IsReady(mesh)` → `shot.mute = 1; return;`。
- `EngineCli.cpp` の表に `--modal-audio-log N` / `--modal-sync-bake` (両 Main)。`EngineLoop`: 配線 + 終了 summary (`[acaudio] summary` の隣、2577 付近)。
  log 行: `[modal] t=<tick> src=<idx> mesh=<16hex> cell=<cx>,<cy>,<cz> slot=<n> k=<kx>,<ky>,<kz> J=<excess> modes=<n> f0=<Hz> f1=<Hz> peak=<dBFS> len=<sec> class=<Direct|Detour|Occluded|Bypass> gain=<g> r=<result>`。
  summary: `[modal] summary impacts= played= notReady= cooldown= belowMin= dropped= poolFull= playFailed= bakes= bakeMsAvg=` (**追加は末尾**)。`--no-audio` で 0 行。
- `ShowcaseScenes.cpp` の表 + `DemoContent.cpp`: `--modal-demo` — 木 / 金属 / ガラスの箱 3 個が金属板と木の床に落ちる小シーン (builtin メッシュ + physmat + ModalSound + AcousticAudio 無し = Bypass 経路も踏む)。golden / replay ペアは足さない。★物を足すなら関数の末尾 (粒子 RNG のストリーム、このシーンに粒子は無いが流儀を揃える)。
- 新規 `Audio/ModalAudioSelfTest.h/.cpp`、selftest 連鎖末尾。

## やらないこと (このサブでは)
Inspector / カタログ / ローカライズ (sub-07)。合成のワーカー化。絶対音量の圧縮は**耳確認の結果を「不安・質問」に書くまで**入れない。

## 触る場所 (planner の見立て)
- `src\Engine\Core\Components.h` / `Components.cpp:1157` の後
- `src\Engine\Engine\Acoustic\AcousticGrid.h:120-132` 付近 / `AcousticField.cpp:914-998`
- 新規 `src\Engine\Engine\Audio\ModalAudio.h/.cpp`、`ModalAudioSelfTest.h/.cpp`
- `src\Engine\Engine\TickRunner.cpp:608-645`、`Audio\AudioSourceSystem.h/.cpp` (260-267 push の型、385-406 drain 入口、621-676 shot 再生)、`Audio\AcousticAudio.cpp:514-540`
- `src\Engine\Engine\EngineCli.cpp` / `EngineLoop.h/.cpp` (2577 summary)、`EngineCliSelfTest.cpp`
- `src\Engine\Engine\ShowcaseScenes.cpp:47` 付近 / `DemoContent.h/.cpp`
- `src\Editor\EditorMain.cpp` (selftest 連鎖)
- 参考: `PhysicsSystem.h:17-34` SolidContact (normal は大 index → 小 index、key = 小<<32 | 大、entity.index のみ)、`TransformSystem` は 409 で確定済み = WorldMatrix は今 tick の値
- ソース追加後 `pwsh -File tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
spec §5 の 15, 16, 17。
1. ModalAudioSelfTest: (1) 箱 (ModalSound) と床の接触 1 件 → 1 impact、箱を Y 90° 回転すると k がローカル軸に乗る、`excessImpulse = impulse − RestingImpulse` / (2) 両側 ModalSound → 2 impact、key 昇順 / (3) `RestingImpulse` が抽出前の式 (テスト内に写した inline 式) とビット一致 / (4) push 65 個目は dropped / (5) 合成マップ (全帯域 mask on) → k=(1,0,0) で count 32、k=0 で BelowMin、`maxDistance` = comp 値 / (6) cooldown 内の 2 発目は Cooldown / (7) `ResolveWaveShotSound`: Ready → mute=1、未 Ready → 従来 soundKey / (8) suspend 中の Update でキューが空になる / (9) CLI 2 本のパース
2. `cmd /c "bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --frames 300 > a.txt"` を 2 回 → `[modal] t=` 行が byte 一致、`played > 0 && playFailed == 0`。`--no-audio` で 0 行
3. `replay_verify.bat` 全ペア緑 (RestingImpulse 抽出 + TypeId 61 の証明)、`--selftest` 全緑、`check_rules.ps1` 緑、Debug/Release 0 警告
4. **耳確認** (coder が実機で。結果は SELF_EVAL に文章で): 面で音が変わる / 強く落とすと大きい / physmat を metal ↔ wood で変えると減衰が変わる。「軽い接触が聞こえない / 重い衝突が張り付く」なら「不安・質問」に書く (planner が圧縮を足すか裁定)

## 検証コマンド
- Debug/Release ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → `pwsh -File tools\check_rules.ps1` → `tools\replay_verify.bat`
- 上の `--modal-demo` 2 run + `--no-audio`

## 実装メモ (coder が追記)

## フィードバック履歴
