# M99 カリングの拡充: 設計の下書き (司会の事前調査)

harness の司会が 2026-11-11 にプランモードで調べた内容。planner の出発点で、裁定で変えてよい。
ユーザーの選択は台帳 `plans\m99-culling\harness.md` の「ユーザー判断」を参照。


### 現状 (調査済み)
- 視錐台の判定は `src\Engine\Renderer\Pipeline\FrustumCull.h` の AABB p-vertex テスト。
  - メッシュは `RenderSystem::CollectDrawables` で判定する。3 段構成で、ステージ 2 は `ParallelRanges` の並列、場所は `RenderSystem.cpp:1476-1504`。
  - 地形は `CullChunks`、CPU パーティクルはプール単位 (`CpuParticleBackend.cpp:739`)、2D はタイルマップのチャンクだけ、ライトは球で判定する。
  - 空間分割は無く、全件を線形に判定している。
- `f.cullEnabled` は「カメラがあるか」を表すフラグで、LOD・URO・地形の収集も同じフラグで止まる。視錐台判定だけを切る口は無い。
- オクルージョンの設定は `rendering.occlusionCulling`。読み書きは `TagNames.cpp:200-226`、起動時の解決は `EngineLoop.cpp:474`、CLI は `--no-occlusion`、切り替えはエディタの Rendering メニュー (`EditorApp.cpp:1274`)。Project Settings 窓 (`ProjectSettingsWindow.cpp` の Rendering 節 L59-98) には出ていない。
- 視錐台判定の無い経路: GPU パーティクル、VFX のトレイル / リボン、デカール (v1 で意図して入れていない。engine_spec §6.6)、個々の Sprite2D / Line2D / SpriteShape2D、水面。
- 局所影アトラス (`ShadowAtlas::Render`) は `queue_` から描くので、画面外のキャスターを見ない。CSM には `offscreenCasters_` の経路があるが、アトラスには無い。
- 画面サイズは `BoxScreenSize` (`MeshLod.h`) で計算済みで、今は LOD と URO にしか使っていない。
- 観測の口: ProfilerWindow、`prof::RenderStats`、`--render-stats-dump`、HZB のデバッグ表示 (落とした物の AABB を赤線で重ねる)。

### 要件案
1. **ON/OFF の切り替え**
   - 対象は視錐台・オクルージョン・距離・画面サイズの 4 種。各カリングの ON/OFF を Project Settings > Rendering に置き、切り替えたらその場で `project_settings.json` に保存する (`rendering.*`)。
   - CLI の `--no-*` はその起動中だけ OFF にし、ファイルは書き換えない。
   - 視錐台の OFF はカメラ視錐台で物を落とす判定だけを止める。ライトの選別と CSM / アトラスのライト側カリングは止めない。CSM のフィット AABB は視錐台の内側の物だけで作り、影の解像度を変えない。
2. **距離カリング**
   - 物ごとの最大描画距離 (MeshRenderer に持たせる。0 = 無制限)、プロジェクトの既定値、タグかレイヤー別の距離表 (Unity の `layerCullDistances`、UE の Max Draw Distance / Cull Distance Volume を参照)。
   - 影キャスターをどう扱うか (影だけ残すか) を決める。
3. **画面サイズカリング**
   - `BoxScreenSize` が閾値より小さい物を落とす (UE の Min Screen Radius、Unity の LODGroup の Culled 段)。
   - LOD と同じヒステリシスで点滅を防ぐ。
4. **視錐台判定の無い経路への追加**: GPU パーティクル (エミッタ単位の AABB)、VFX、デカール、Sprite2D 系。
5. **局所影の画面外キャスター**: ライトの視錐台に入る画面外の不透明物をアトラスに描く。CSM の `offscreenCasters_` と同じ考え方。
6. **BVH**
   - 先に計測する。物の数を増やしたベンチで、`CollectDrawables` のステージ 1〜2 の CPU 時間を測る。
   - 効果があると分かった場合だけ、静的物の BVH と動的物の線形判定の併用などを入れる。並列化した今の判定と結果がビット一致することを不変量にする。
7. **デバッグ機能**
   - カリング用カメラの固定 (UE の FreezeRendering): 判定は固定した視点で行い、描画カメラだけ動かす。
   - 落とした物を理由別の色で枠表示する (視錐台 / 距離 / 画面サイズ / オクルージョン)。
   - Profiler に理由別の culled 数を出す。

### 制約
- どれも描画専用で、sim、ハッシュ、Replay、ABI、SimSnapshot には触れない。ただし距離をコンポーネントに持たせる場合は、シリアライズとプレハブ上書きの扱いが要る。
- Debug / Release / WARP で `--render-stats-dump` の `counts` が一致すること。
- UI 文字列は `LocalizationTable.inl` に両言語で入れる。engine_spec §6.14 と `docs\test_checklists.md` を更新する。
- 回帰テスト: TagSelfTest (設定の往復)、EngineCliSelfTest、OcclusionSelfTest の ON/OFF 全画素一致の作りに倣った描画テスト。

## 検証 (サブ共通)
- ビルドは Debug / Release、加えて `tools\check_rules.ps1`、関連する selftest、最後に `--selftest-full` と `tools\replay_verify.bat` を流す。
- `--render-stats-dump` で、カリングの種類ごとに ON と OFF の差を数値で確かめる。
- エディタでの目視はユーザーが行う。
