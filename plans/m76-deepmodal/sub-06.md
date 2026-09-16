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
4. ~~**耳確認**~~ → **sub-08 へ繰り延べ** (round 1 で planner が裁定)。`assets\deepmodal\deepmodal.dmnet` は sub-08 が作るので、この時点では乱数重みの `fixture.dmnet` の音しか出せず、**聴感評価が原理的に成立しない**。代わりにこのサブでは**機構レベルの客観的証拠**で足りることとする: `--modal-demo` のログで (a) 同一メッシュでも発音元の physmat (wood/metal/glass の E/α/β) ごとに `f0`/`f1`/`modes` が変わる (b) `J` が小さいほど `len` (減衰長) が短くなる。実際の耳確認と「絶対音量に圧縮を足すか」の判断は sub-08 の受け入れ条件へ移した

## 検証コマンド
- Debug/Release ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → `pwsh -File tools\check_rules.ps1` → `tools\replay_verify.bat`
- 上の `--modal-demo` 2 run + `--no-audio`

## 実装メモ (coder が追記)

SELF_EVAL: sub-06 (round 1)
実装:
  - `src/Engine/Core/Components.h`:`ModalSoundComponent` — TypeId 61 (末尾 append)。`mesh`/`gain`/`maskThreshold`/`cooldownTicks`/`sizeScale`/`maxDistance`/`muteWave`。`kComponentNoHash` のみ (`kComponentUiAux` は付けない)
  - `src/Engine/Core/Components.cpp`:`RegisterBuiltinComponents()` — `UIToggleGroup` の直後 (末尾) に `ModalSound` を登録
  - `src/Engine/Engine/Acoustic/AcousticGrid.h`:`acoustic::RestingImpulse` 宣言を追加 (`World`/`EntityID` を使うため `EntityID.h` を include + `class World;` 前方宣言)
  - `src/Engine/Engine/Acoustic/AcousticField.cpp`:`acoustic::RestingImpulse` を定義 — `DrainImpacts` の `dynamicMass` ラムダ + `(dynamicMass(ea)+dynamicMass(eb))*gMag*dt*kImpactRestingMargin` を **1 文字も変えず** 抽出。`DrainImpacts` 側は `acoustic::RestingImpulse(world, ea, eb, gMag, dt)` を呼ぶだけに置換 (評価順・定数の参照順は不変)
  - 新規 `src/Engine/Engine/Audio/ModalAudio.h/.cpp`:
    - `PendingModalImpact`(source/mesh/worldPoint/localPoint/k/excessImpulse/tick/key)、`ModalAudioStats`、`ModalShotResult`、`ModalShotInfo`
    - `ResolveModalMesh` (ModalSound.mesh 空 → 同 entity の MeshRenderer.mesh。CollectModalImpacts と ResolveWaveShotSound の口封じが共有する唯一の規則)
    - `ModalWorldScaleOfLongestAxis` (Physics/Shapes.cpp::MakePoseFromMatrix と同じ行ベクトル長でスケール近似)
    - `ModalOnCooldown` (cooldown 判定の純関数。デバイス非依存でテストできるよう `AudioSourceSystem::Update` から切り出した)
    - `CollectModalImpacts` — index→EntityID 表は `AcousticField::DrainImpacts` (923-940) と同じ作り方を複製。両側 ModalSound は両方 (小→大の順)。法線は「大 index→小 index」、E が小なら `nE=n`、大なら `nE=-n` (`XMVector3TransformNormal` + 再正規化)。局所点は `XMVector3TransformCoord`
    - `MakeModalShotPlay` — `modal::LocalPointToCell` → `ModalFeatureMap::CellFeature` → `BuildModes`(mat==null は `post.density=hdr.refDensity`/`alpha=hdr.refAlpha`/`beta=hdr.refBeta` を渡して σ2=1・c=cRef にする。σ1 は BuildModes 内の `young<=0` 特別扱いに任せる) → `ModalSynthRender` → `AudioSpatial`(position/spatialBlend/minDistance/maxDistance/rolloff/dopplerScale/pitch を埋め、reverbSend は 0 のまま呼び出し側へ委譲)
  - `src/Engine/Engine/TickRunner.cpp`:`!ts.resim` ブロック、wave push の直後に `CollectModalImpacts` → `audioSources.PushModalImpact` (門は `audioSystem.IsReady() && !IsSuspended()` のみ。`ts.acoustic` の有無には依存しない)
  - `src/Engine/Engine/Audio/AudioSourceSystem.h/.cpp`:`SetModalLibrary`/`SetModalAudioLog`/`SetModalSyncBake`/`ModalStats`/`PushModalImpact`(上限 64、溢れは `dropped`)/`ModalEntityState`(sorted vector + 二分探索、EntityID の index→generation で比較)/`ModalClipSlotState`(32 スロット、`endTick` 予約)/`kMaxModalShotsPerTick=4`。`Update()`: キュー swap を wave shot と**同じ場所**(IsReady 判定より前) に追加。drain は wave shot ループの直後、**acOn を要求しない**独立ブロック。cooldown→Request/BakeSync→PhysMat解決→`MakeModalShotPlay`→ラウンドロビンでスロット選択(埋まっていれば`PoolFull`)→`RegisterClip`→`PlayDesc{bus=SE,volume=1,priority=128}`→acOn なら `ShapeAcousticSpatial` で `desc.volume` に gain を掛ける→`Play`。`[modal] t=`/`summary` のログ出力
  - `src/Engine/Engine/Audio/AcousticAudio.cpp`:`ResolveWaveShotSound` 先頭に (0) `ModalSound.muteWave!=0 && modalsound::IsReady(mesh)` なら `mute=1;return;`
  - `src/Engine/Engine/EngineCli.cpp`:`--modal-audio-log N` / `--modal-sync-bake` を Deep-Modal 節に追加。`src/Engine/Engine/EngineLoop.h`:`modalAudioLogTicks`/`modalSyncBake` フィールド追加。`src/Engine/Engine/EngineLoop.cpp`:`SetModalLibrary(&modalSounds)` 等の配線 + 終了時 `[modal] summary` (`[acaudio] summary` の隣、`impacts>0` のときだけ出す)
  - `src/Engine/Engine/EngineCliSelfTest.cpp`:上記 2 フラグのパーステスト追加
  - `src/Engine/Engine/ShowcaseScenes.cpp` / `src/Engine/Engine/DemoContent.h/.cpp`:`--modal-demo` — 木/金属/ガラスの箱 3 個が「左半分=木・右半分=金属」の床に落ちるシーン。**AcousticAudio を置かない** (Bypass 経路)。`RegisterModalShowcaseContent` を `src/Editor/EditorApp.cpp` と `src/Runtime/RuntimeMain.cpp` に配線
  - 新規 `src/Engine/Engine/Audio/ModalAudioSelfTest.h/.cpp`、`src/Editor/EditorMain.cpp` の selftest 連鎖末尾 (`RunModalSelfTest()` の後) に追加
  - `pwsh -File tools/gen_project_files.ps1` を実行 (`build/Engine.vcxproj`/`.filters` に新規 4 ファイルが載る)
仕様との差分:
  - [追加] `PendingModalImpact` に `localPoint`(発音元ローカルの接触点) を追加。sub-06.md の prose 列挙 (「接触点ワールド / ローカル k[3] / …」) には無いが、`MakeModalShotPlay` の signature に WorldMatrix が無い (§4.1 通り) ため、cell 選択に使うローカル座標は「今 tick の WorldMatrix が手に入る」`CollectModalImpacts` の時点で計算して保持する以外に作りようが無かった。ワールド座標 (`worldPoint`) は元の prose 通り AudioSpatial.position 用に保持
  - [追加] `ResolveModalMesh` / `ModalWorldScaleOfLongestAxis` / `ModalOnCooldown` を公開ヘルパーとして `ModalAudio.h` に置いた。spec 本文には無いが、(a) mesh 解決規則を `CollectModalImpacts` と `ResolveWaveShotSound` の口封じで**1 本化**する (b) cooldown 判定をデバイス非依存でテストできる形に切り出す、の 2 つの目的。いずれも「規則は 1 本」の既存方針に沿う補い
  - [追加] drain 中に発音元の `ModalSoundComponent` が (push 後に) 消えている稀なケースを `NotReady` バケットへ畳んだ (spec 未規定のエッジケース。専用の結果値が無いため)
  - [追加] `--modal-sync-bake` 使用時、`Request()` が既に `NoModel` を返している場合は `BakeSync()` を呼ばない (呼ぶと `Failed` に上書きされ `NotReady`/`NoModel` の区別が診断ログ上で失われるため)。spec は明記していないが、`r=` ログの意味を壊さないための実装判断
検証:
  - Debug/Release ビルド (`/p:MyeWarnAsError=true`) → 0 警告 0 エラー (2 回。最終の NoModel 分岐修正後に再ビルドして確認)
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 全緑 (ModalAudioSelfTest の (1)〜(8) 含む。CLI 2 本は EngineCliSelfTest 側で確認 — 理由は下記「不安・質問」)
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `MYE_REPLAY_JOBS=3` で `tools\replay_verify.bat` → `[PASS] replay consistency` (8 シーンチェーン全 PASS + タイムトラベル/What-if/rule check 込み 13 job 全 PASS)。RestingImpulse 抽出と TypeId 61 追加が sim を 1 ビットも動かしていないことの証明
  - `--modal-demo` の手動検証: リポジトリに `.dmnet` が未コミット (sub-08 未着手のため) なので、`tests\deepmodal\fixture.dmnet` を一時的に `assets\deepmodal\deepmodal.dmnet` へコピーして検証し、**検証後に削除した** (git 差分はクリーンなことを確認済み)。
    - `--modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --screenshot tmp.png --frames 300` を 2 回 → `Engine loop finished (300 frames, 300 ticks)` (frame==tick が保証される)、`[modal] t=` 20 行が **byte 一致** (`bakeMsAvg` のみ異なる。spec の「probeMsAvg だけ比較から除く」と同型)。`impacts=20 played=20 notReady=0 dropped=0 poolFull=0 playFailed=0`
    - `--no-audio` 併用 → `[modal] t=`/`summary` 行 0 (`[modal] loaded .dmnet` という**別の**起動ログが 1 行出るだけ。これは sub-05 由来の既存メッセージで本サブの契約対象外)
    - 一時ファイル (`assets\deepmodal\deepmodal.dmnet`・`.meta`、`tmp.png` 等) は全て削除し `git status` がクリーンであることを確認した
自己採点 (1-5):
  仕様適合: 4 — 経路・フィールド・ログ書式・CLI・TypeId append・RestingImpulse 抽出は仕様通り。上記「仕様との差分」の 4 件は spec の記述密度が届いていなかった箇所を最小限の判断で埋めたもので、恣意的な仕様変更ではない
  正しさ: 5 — selftest 全緑・check_rules 緑・replay_verify 全 13 job 緑 (sim 非破壊の証明)・`--modal-demo` の実機実行で played>0/playFailed==0/byte 一致を確認済み
  コード品質: 4 — 既存パターン (MakeWaveShotPlay / ResolveWaveShotSound / kMaxImpactsPerTick の break 規律) を踏襲し「規則は 1 本」を守った。`AudioSourceSystem::Update` の新規ブロックはネストがやや深い (cooldown→Request→BakeSync→PhysMat→BuildModes→pool→Play の直列分岐) — コメントで補っているが、将来触るなら小関数への分割余地あり
  テスト: 4 — `ModalAudioSelfTest` で (1)〜(8) を自動化、CLI 2 本は `EngineCliSelfTest` に追加、`--modal-demo` 実機検証も実施。**唯一「耳確認」(受け入れ条件 4) は文字通りには実施できていない** — 詳細は下記
不安・質問:
  1. **耳確認 (受け入れ条件 4) を文字通りには実施できていない**。理由は 2 つ: (a) このセッションに音声出力/聴取の手段が無い (b) `assets\deepmodal\deepmodal.dmnet` がまだ存在しない (M76h/sub-08 が作る) ため、たとえ人間が耳で聞いても現時点では「ランダム重みの小規模テストネット (fixture.dmnet)」の出力にしかならず、意味のある聴感評価 (面で音が変わる/強く落とすと大きい/材質で減衰が変わる、を実際の音として確認する) が原理的にできない。代わりに **機構レベルの客観的証拠** を集めた: fixture.dmnet を使った `--modal-demo` の実行ログで、同一メッシュ (Cube) でも発音元ごとに `f0/f1/modes` が異なること (= 材質 (wood/metal/glass の E/α/β) の違いが `BuildModes` の出力に反映されている)、および `excessImpulse (J)` が小さくなるにつれ `len` (減衰長) が短くなること (= 衝撃の強さが聴感の長さ/エネルギーに反映される機構が働いている) を確認した。**この項目の最終確認は sub-08 で実モデルが揃ってから改めて行う必要がある** — planner に判断を委ねたい (このサブの合否に含めるか、sub-08/sub-07 側の受け入れへ繰り延べるか)
  2. **受け入れ条件 2 の手動検証コマンド (`--frames 300` のみ、`--screenshot` 無し) は tick 数が実時間依存で非決定的** であることが判明した (EngineLoop.cpp の既存挙動: `dt` が `kFixedDt` に固定されるのは `--screenshot`/`--timetravel-selftest`/`--whatif-selftest` のときだけで、それ以外は `clock.BeginFrame()` の実時間が使われる。実測で同じコマンドが 25 tick から 110 tick まで揺れた)。これは本サブの変更とは無関係な既存の性質。`--screenshot <path>` を足すと `frame==tick` が保証され、`[modal] t=` 行は `bakeMsAvg` を除いて byte 一致することを確認済み (上記「検証」参照)。**この既存の非決定性は sub-06 の実装に起因しないので直さなかった**が、この手動検証コマンドを再利用する後続 sub (07/08) やレビュアが同じ罠を踏まないよう、`--screenshot` を足すことを推奨する。仕様 (spec §5 受け入れ条件 16 / sub-06.md 受け入れ条件 2) の文言を直すかどうかは planner の判断を仰ぎたい
  3. cooldown の更新タイミング (`est.lastShotTick` を更新するのは `Played` のときだけで、`NotReady`/`BelowMin`/`PoolFull`/`PlayFailed` では更新しない) は spec に明記が無かったため、`AcousticField::DrainEmitters` の cooldown 設計 (Emit 成功時のみ更新) に倣った。却下されるべき理由があれば教えてほしい
触ったファイル:
  - src/Engine/Core/Components.h
  - src/Engine/Core/Components.cpp
  - src/Engine/Engine/Acoustic/AcousticGrid.h
  - src/Engine/Engine/Acoustic/AcousticField.cpp
  - src/Engine/Engine/Audio/ModalAudio.h (新規)
  - src/Engine/Engine/Audio/ModalAudio.cpp (新規)
  - src/Engine/Engine/Audio/ModalAudioSelfTest.h (新規)
  - src/Engine/Engine/Audio/ModalAudioSelfTest.cpp (新規)
  - src/Engine/Engine/Audio/AudioSourceSystem.h
  - src/Engine/Engine/Audio/AudioSourceSystem.cpp
  - src/Engine/Engine/Audio/AcousticAudio.cpp
  - src/Engine/Engine/TickRunner.cpp
  - src/Engine/Engine/EngineCli.cpp
  - src/Engine/Engine/EngineCliSelfTest.cpp
  - src/Engine/Engine/EngineLoop.h
  - src/Engine/Engine/EngineLoop.cpp
  - src/Engine/Engine/ShowcaseScenes.cpp
  - src/Engine/Engine/DemoContent.h
  - src/Engine/Engine/DemoContent.cpp
  - src/Editor/EditorApp.cpp
  - src/Editor/EditorMain.cpp
  - src/Runtime/RuntimeMain.cpp
  - build/Engine.vcxproj
  - build/Engine.vcxproj.filters
申し送り:
  - 次サブ (sub-07、Inspector プレビュー) は `MakeModalShotPlay` をそのまま呼ぶ設計にしてある。局所座標 (`impact.localPoint`) が必要な形なので、6 面ボタンのプレビューは「AABB 面中心のローカル座標」をそのまま `PendingModalImpact.localPoint` に、ワールド座標は (プレビューでは音源位置を厳密に問わないはずなので) 適当なダミーで良いはず — ただし `ModalWorldScaleOfLongestAxis` はプレビュー対象エンティティの実際の WorldMatrix を渡すこと
  - sub-08 (本学習 + `.dmnet` コミット) が終わったら、上記不安・質問 1 の「耳確認」をこのサブに立ち戻って完了させる (もしくは sub-08/sub-07 側の受け入れ条件として吸収する) ことを推奨
  - `AudioSourceSystem::Update` の Deep-Modal drain ブロックは 1 関数の中でネストが深い。将来 sub-07/sub-08 で触るときは、cooldown 判定より後の「Request/BakeSync → PhysMat → MakeModalShotPlay → pool → Play」を 1 つの private メソッドへ切り出す余地がある (今回は「規則は 1 本」の維持を優先し、既存の `AudioSourceSystem::Update` 内 wave shot 処理と対称な構造のまま実装した)

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: OK** (planner、2026-09-16)。**エンジンで実際に音が鳴った最初のサブ** (接触 20 件 → 20 発再生、`playFailed=0`)。planner が実コードで確認した要点: (i) **`RestingImpulse` の抽出は忠実** — `git diff` で被演算子の順序と結合 (`((((mA+mB)*gMag)*dt)*kImpactRestingMargin)`) が一致、`namespace acoustic` 内なので `kImpactRestingMargin` の解決先も同じ、`dynamicMass` ラムダも同一。★さらに **`ModalAudioSelfTest` (3) が抽出前の式をインラインで再現して `expect == actual` の完全一致で比較**している = **replay に依存しない独立した証人**があるのが良い (replay 緑は必要条件だが「acoustic ペアがその式を通る入力を踏んでいるか」は別問題、という司会の指摘への回答がこれ)。(ii) **法線の向きは `SolidContact` の規約と一致** — `ea = key>>32` (小 index)、`eb = key & 0xFFFFFFFF` (大 index)、normal は大→小なので `emitSide(ea, +n)` / `emitSide(eb, -n)` が正しい。(iii) cooldown を `Played` のときだけ進める判断は `DrainEmitters` の流儀と揃っており妥当 (鳴らなかった発を理由に次を抑止すると無音が伸びるだけ)。
