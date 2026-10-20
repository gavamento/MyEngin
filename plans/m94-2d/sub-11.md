# sub-11: タイル衝突 + セル書き換えの sim 状態 + 破壊

- 依存: sub-08, sub-10
- 状態: OK (commit 1eaee36)
- 往復: 1
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #7, #8, #9, #29、4.1.7、4.2.3、7. R2

## やること
1. **override 表** (`TilemapSystem::OverrideStore`): `std::vector<Entry{ uint32 entityIndex; uint32 generation; uint8 layer; uint32 cellKey; uint32 value; }>` をキー昇順で保つ。`SetCell(e, layer, cx, cy, v)`: v がアセット値と等しければ項を削除、違えば upsert。`GetCell` は override → アセット。`FillRect`、`DamageCell` (spec 4.1.7)。Tilemap エンティティが消えたら項を捨てる (tick 末尾の構造変更の後、`World` の生存判定で掃除)。チャンク dirty を描画側へ通知 (dirty 集合は描画側が持つ。sim 側は「変更したチャンクの列挙」を tick ごとに出すだけ)。
2. **hash**: `WorldHasher` の `SimSources` 末尾に `const TilemapSystem*` を足し、`HasLiveState()` (表が空でない) のときだけ `HashTilemapOverrides` を畳む (`HashComputeState` の形)。`SimSourcesOf` / `HashSources()` の引数も足す。
3. **snapshot**: 'TM2D' 節 (Compute 節の次、World の前)。`WriteTilemap / ReadTilemap` (`WriteCompute / ReadCompute` の 2 段 Restore に合わせる)。空なら Count(0)。復元後に全チャンク dirty + 衝突キャッシュ無効化。`SimRefs` と `TickServices` の配線 (`EngineLoop.cpp:696`、`HeadlessSim.cpp:210/391`、`TickRunner.cpp:637`)。`kSimSnapshotVersion` +1。
4. **衝突**: `collision` レイヤーのセルで `palette.collider != 0` のものを、チャンク単位で行方向に結合して箱 (`oneWay` のセルは別の箔列)。`StaticShapeSource` (sub-08) の供給元として `TilemapSystem` を PhysicsSystem に登録 (layer / mask / friction / physMaterial は TilemapComponent から)。キャッシュは chunk 単位、dirty で作り直し。箔の z 半厚は cellSize の半分。**計測**: 100×50 の床の箱数と、`--physics-demo` 相当の物理 1 tick 時間をログ。`--physics-demo` の 2 倍を超えたらチャンク AABB の前カリング (ブロードフェーズ前に、動的ボディの AABB と交差するチャンクの箔だけ収集) を足す。
5. 破壊: `DamageCell` の規則 (hp 0 は無傷、damage 加算、到達で `destroyedTile`、`destroyEffect` を tick 末尾のエフェクト生成に積む — M89i の `effectQueue` と同じ口)。
6. selftest (`TilemapSelfTest` に節追加): (a) plane2D 箔がタイル床に乗る (b) 一方通行タイルを下から抜ける (c) `SetCell` で床を消すと落ちる (d) 3 セル書き換え → capture → 新ワールドへ restore → 100 tick 連続実行と一致 (`FractureSelfTest.cpp:2276-2324` の前例) (e) Tilemap の無いシーンの `HashWorld` と snapshot バイト列が着手前と同一 (着手前に採って焼く) (f) hp 3 のタイルが 3 回で壊れ、エフェクトが 1 個生成 (g) 2 ワールド並走の HashWorld 一致、Debug / Release で焼いたハッシュ同値。

## やらないこと (このサブでは)
- ABI (sub-14)、パレット窓 (sub-12)、ルール / アニメ (sub-13)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}`、`TilemapSelfTest.cpp`
- `src\Engine\Core\Ecs\WorldHasher.{h,cpp}` (:36 SimSources、:615-645 大域の項目、:434-458 の形)
- `src\Engine\Engine\Replay\SimSnapshot.{h,cpp}` (:89-96 SimRefs、:562-624 節、:702-735 Restore)
- `src\Engine\Engine\Loop\EngineLoop.cpp:696`、`HeadlessSim.cpp:210/391`、`TickRunner.{h,cpp}` (TickServices、物理の前後)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp:1701` (供給元の登録)
- `AcousticAudioSelfTest.cpp:129`
- 参考: `src\Engine\Engine\Compute\` の State 節 (M91d / M91k)、`plans\m82-navmesh\spec.md` 2. #16 (presence gate)

## 受け入れ条件 (このサブ)
1. (spec 21) 箱数と物理 1 tick 時間のログ (SELF_EVAL に数値)。(a)(b)(c) PASS。
2. (spec 22) (d)(e)(g) PASS。
3. (spec 23) (f) PASS。
4. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` の Tilemap / Physics suite が両構成で PASS、snapshot +1 のみ。既存ジョブの hash は Tilemap 無しなので不変 = (e) で確認。重い 3 本は sub-15。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --demo --replay-record` → `--hash-diff` (presence gate の 1 本確認)
チェックポイントではない (重い 3 本は sub-15)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-11 (round 1)
実装:
  - TilemapSystem.h/.cpp: 静的関数だけだった TilemapSystem をインスタンスに。override 表 (`std::vector<TileOverride>` を (entity, 世代, レイヤー, cellKey) 昇順で保つ)、GetCell / GetChunkCells / EnumerateChunks (override を重ねた実効セル)、SetCell / FillRect / DamageCell、DrainEffects、Sweep、Reset、SaveSnapshot / ReadSnapshot / ApplySnapshot、HasLiveState。アセットだけを引く静的関数 (AssetOf / AssetChunkCells / AssetEnumerateChunks / OriginOf / WorldToCell / CellToWorld) と MarkAllDirty / DirtyEpoch は静的のまま。
  - TilemapSystem.cpp: StaticShapeSource を実装。collision レイヤーのセルで palette.collider != 0 のものをチャンク内で行方向に結合した箱にし (通常の箱 → 一方通行の箱の順)、チャンク単位でキャッシュ (SetCell が該当チャンクだけ捨てる、ライブラリ epoch が変われば全捨て)。ColliderComponent 相当は持ち主ごとに solid / oneWay の 2 つを map の要素として持ち、layer / mask / friction / physMaterial は TilemapComponent から毎 tick 写す。z 半厚 = max(cellSize) / 2。原点は ComposeEntityWorldPose (物理と同じ式)。順序は持ち主 index 昇順 → レイヤー → チャンク (cy, cx) → 通常 → 一方通行。AppendCollisionShapes (showCollision 用)。
  - TilemapSystem.cpp: WakeBodiesNear — セルのタイル id が変わったとき、近くで眠っている剛体を起こす (SetCell / FillRect / DamageCell の破壊)。
  - StaticShapeSource.h / PhysicsSystem.cpp: 供給口に CollectWithin(world, MotionRegions&, out) を追加 (既定は Collect)。PhysicsSystem が「剛体 (眠り含む・kinematic 除く) / 複合の子 / XPBD 粒子 / キャラクターが速度 2 tick ぶん + 2 m で届く範囲」を遅延で組み、TilemapSystem がそれに届くチャンクの箱だけ返す (R2)。
  - Broadphase.h/.cpp + PhysicsSystem.cpp: BroadphaseEntry.staticOnly (Rigidbody を持たない静的同士) のペアを候補から落とす。静的同士は接触も起床も島も作らないので結果は変わらない (既存の焼いたハッシュが全部不動)。
  - WorldHasher.h/.cpp: SimSources 末尾に tilemap、HashTilemapOverrides (内容ゲート = HasLiveState)。
  - SimSnapshot.h/.cpp: SimSourcesOf と SimRefs に tilemap、'TM2D' 節 (CMP 節の次・World 節の前、WriteTilemap / ReadTilemap、復元は World 差し替え後に ApplySnapshot)、kSimSnapshotVersion 56 -> 57。AcousticAudioSelfTest.cpp の版照合も 57。
  - TickRunner.h/.cpp: TickServices.tilemap。構造変更適用の直後に Sweep、エフェクト生成の直前に DrainEffects で破壊エフェクトを effectQueue へ、シーン遷移で Reset、hashSources に渡す。エフェクト 1 件の生成を SpawnEffectRequest として関数化 (挙動不変)。EffectSpawnRequest.prefabHash (非 0 ならパス解決を飛ばす)。
  - EngineLoop.cpp / HeadlessSim.cpp / SimInit.h/.cpp: TilemapSystem を所有、physicsSystem.AddStaticShapeSource、simRefs.tilemap、tickServices.tilemap。SimLibraries.tilemapSystem と tilemap::InstallSystem / System() (描画が実効セルを引く口)。
  - Sprite2DRenderer.cpp: チャンクの焼きと列挙を tilemap::System() 経由の実効セルに (未接続ならアセットだけ)。override だけで埋まったチャンクも描く。
  - SceneViewWindow.cpp: showCollision の衝突の箱を緑 (kCollider) で引く (DrawTilemapGizmos)。
  - TilemapSelfTest.cpp: 節追加 (床に乗る / チャンク境界を転がる / 箱数 / FillRect で床を消す・眠っている球が落ちる / 元値に戻すと override が消える / override だけのチャンク / 一方通行 (剛体と CC) / 3+1 セル書き換え -> capture -> 新ワールド restore -> 100 tick 一致 / 壊れた blob / Sweep / 破壊 3 回 + エフェクト 1 個を実際に生成 / Tilemap 無しの HashWorld と snapshot バイト列が着手前と同一 / 空の TilemapSystem を配線しても動かない / 2 ワールド 600 tick 並走 + 焼き値 / 絞り込みの有無で結果が同一 / 100x50 の箱数と物理 1 tick の時間)。sub-10 の既存節は GetCell 等をインスタンス呼びに直しただけ。
仕様との差分:
  - [追加] 眠っている剛体の起床 (WakeBodiesNear)。眠りは「覚醒中の剛体に触られたら起きる」だけなので、床のタイルを消しても眠っている球が宙に浮いた (selftest で再現: 球は 90 tick 後も y = 1.25 のまま)。受け入れ条件 21 の「SetCell で床を消すと箔が落ちる」を成立させるのに必要。通常の静的 Collider を消した場合に眠りが起きない既存の挙動には触れていない。
  - [追加] StaticShapeSource::CollectWithin / MotionRegions と PhysicsSystem の範囲の組み立て、Broadphase の staticOnly。spec 7. R2 / 2. #8 が「`--physics-demo` の 2 倍を超えたら前カリング」と定めており、実測が 4.6 倍 (0.54 ms / 0.116 ms) だったので足した。staticOnly だけで 2.8 倍、絞り込みで 0.82 倍。Broadphase / PhysicsSystem は sub-11 の「触る場所」に無い。
  - [追加] SpawnEffectRequest の関数化と EffectSpawnRequest.prefabHash。destroyEffect は PrefabLibrary のキー (AssetID) でパスを持たないため、また受け入れ条件 23 の「プレハブが 1 個生成される」を TickRunner と同じ経路で selftest するため。
  - [追加] showCollision の緑の箱 (SceneViewWindow)。sub-10 申し送りと spec 4.3 に有り、sub-11 の本文には無い。ビルドのみで画面確認はしていない。
  - [逸脱] cellKey = cy * 65536 + cx ではなく、各軸を +32768 して 16 bit に詰めた (cy が上位)。仕様の式は負のセル座標で衝突する (cx = -1, cy = 0 と cx = 65535, cy = -1)。並びは (cy, cx) 昇順のまま。セル座標は -32768..32767 に制限し、範囲外の書き込みは false。
  - [逸脱] 描画への dirty 通知は「変更チャンクの列挙」ではなく、sub-10 の申し送りどおり MarkAllDirty (全チャンクの焼き直し)。列挙を足しても消費者がいない。衝突キャッシュだけは該当チャンクを個別に捨てる。
  - [逸脱] DamageCell の壊れた後の値は、destroyedTile の id に向き (flip / 回転 / 予約ビット) を残す。destroyedTile = 0 のときは全ビット 0。
  - [未実装] なし。ただし Raycast / OverlapBox / BoxCast (PhysicsQueries) はタイルの箱を見ない (供給口は物理ソルバの収集だけ)。ABI の TilemapSetCell 等は sub-14。
検証:
  - Debug / Release `MyEngine.sln /p:MyeWarnAsError=true` -> 0 警告 0 エラー (最終コードで両構成)。gen_project_files は未実行 (ファイルの追加・削除なし)。
  - `pwsh tools\check_rules.ps1` -> 3 error / 50 warning (着手前と同じ 3 / 50。error は既存の Editor.bat / Setup.bat の LF と同じ種類で M94 の外)。
  - `MYE_SELFTEST_ONLY=m94` の Editor.exe --selftest -> Debug / Release とも exit 0、FAIL 0 (Physics / Fracture / Tilemap / UI / Sprite2DAsset / SpriteEditor / AnimatorController / AnimatorControllerEdit / AssetDatabase / ReloadHub / AssetOps)。
  - 受け入れ条件 21 (箱数と時間): 100x50 の全面床 = 350 箱 (50 行 x 7 チャンク列)。20 球を載せた物理 1 tick: Release 0.093 ms (対照の 1 枚箱の床 0.033 ms / `--physics-demo` のシーン 0.115 ms = 比 0.82、絞り込み後の箱 108)、Debug 1.19 ms (demo 1.14 ms = 比 1.05)。絞り込み前は 0.54 ms (比 4.6)、staticOnly だけで 0.33 ms (比 2.8)。(a) 球が床に乗る y = 1.2495、チャンク境界 x = 0 を 6 m/s で渡って seam の y が 1.2495..1.2499、(b) 剛体は max y 7.955 まで抜けて y = 4.2495 に乗る / CC は台の下から跳んで y = 4.9 に乗り groundEntity = Tilemap エンティティ・ignoreOneWay で床まで降りる、(c) 眠っている球が床を消すと y = -9.8 まで落ち、元値に戻すと override が 0 件、PASS。
  - 受け入れ条件 22: (d) 4 override で capture -> 新ワールドへ restore -> 100 tick が毎 tick 一致 (CC の groundEntity も同じ)、(e) Tilemap 無しの HashWorld 548FCE3EEBED7EBC と、版の語 + 空 TM2D 節を除いた snapshot の FNV 指紋 CBA0926CD0BA0425 (2141 byte) が、着手前の v56 の実装で採った値と一致 (Debug == Release)、空の TilemapSystem を配線しても HashWorld が動かない、(g) 2 ワールド 600 tick 並走が毎 tick 一致、最終 HashWorld 326BA3CD14F4BBB8 (Debug == Release、焼いた)。
  - 受け入れ条件 23: hp 3 の木箱は 1・2 回目が damage 1・2、3 回目で destroyedTile (rubble) に置換、エフェクト要求がちょうど 1 件 (prefab ハッシュ・セル中心)、SpawnEffectRequest で RubbleFx が (15.5, 1.5) に生成、壊れた後は箱が消える、PASS。
  - 既存の焼いた値は不動: plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 / CC2D C04B7FFA3E31E5D0 (m94 の Physics suite が焼き値と照合して PASS)。NavAgent 庭 8581BEE4A676337C は Release の全体 selftest (下記) で PASS。
  - `Runtime.exe --replay-record` (600 tick) -> `Editor.exe --hash-diff` の前後比較: 着手前に採った cache\m94\s11\before_demo.dump と変更後の after_demo.dump が identical (7596 行、total 6834D7A80D7B38D8)。変更後の .rep を同じ Runtime で --replay-verify -> 600 tick PASS。
  - おまけ: Release の Editor.exe --selftest 全体 (m94 の絞りなし) -> exit 0、FAIL 0 (約 1.6 分)。Debug の全体、replay_verify.bat、shot_verify.bat は未実行 (ユーザー判断 9)。showCollision の画面、Scene View での実表示は未確認。
自己採点 (1-5):
  仕様適合: 4 — 本文 1〜6 と受け入れ条件 21〜23 を満たし、箱数・時間も実測。cellKey の式、dirty の通知の形、DamageCell の向き保持の 3 点が仕様の字面と違う (理由は上)。
  正しさ: 4 — Debug / Release の両方で selftest と焼き値が一致し、着手前との差分 (HashWorld・snapshot バイト列・--hash-diff) が 0。未確認: showCollision の実画面、スクリプト (ABI) 経由の書き込み、車輪のレイが絞り込みの余裕 2 m を超える車両。
  コード品質: 4 — 責務は TilemapSystem に集約し、導出値 (キャッシュ) と sim 状態 (override 表) を分けた。TilemapSystem.cpp が長め。PhysicsSystem の範囲の組み立ては Update の中のラムダ (Update 自体が巨大な既存構造に合わせた)。
  テスト: 4 — 受け入れ条件ごとに節があり、足場は 2 ワールド / 絞り込みなしとの比較 / 着手前の指紋と、反証側の検査も入れた。時間比は環境依存なのでログのみで、assert は箱数の絞り込み (半分未満) に留めた。
不安・質問:
  - [planner] WakeBodiesNear は spec に無い追加。「タイルの書き換えで眠っている剛体を起こす」を sim の規則として認めてよいか。逆を選ぶと、床を消した球が眠ったまま宙に残る (通常の静的 Collider を消したときと同じ既存の挙動)。
  - [planner] 絞り込みの余裕 (速度 2 tick + 2 m、チャンク単位) より遠くまで届く探り (車輪のサスペンションのレイなど) はタイルの箱を見落とす。v1 の限界として engine_spec (sub-24) に書くか、余裕を広げるか。
  - [planner] PhysicsQueries (Raycast / OverlapBox / BoxCast) にタイルの箱を見せるのは sub-14 (ABI) の範囲か、別サブか。地面判定をスクリプトで Raycast している 2D ゲームはタイルに当たらない。
  - [planner] 範囲の組み立て (キャラクター / 剛体 / 粒子) と staticOnly は Broadphase / PhysicsSystem の変更なので、reviewer には PhysicsSelfTest と全 replay の確認を重めに頼みたい (チェックポイント 3 の sub-15 で回る)。
触ったファイル:
  src/Engine/Engine/Tilemap/TilemapSystem.h
  src/Engine/Engine/Tilemap/TilemapSystem.cpp
  src/Engine/Engine/Tilemap/TilemapSelfTest.cpp
  src/Engine/Engine/Tilemap/TilemapSelfTest.h
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.cpp
  src/Engine/Engine/Replay/WorldHasher.h
  src/Engine/Engine/Replay/WorldHasher.cpp
  src/Engine/Engine/Replay/SimSnapshot.h
  src/Engine/Engine/Replay/SimSnapshot.cpp
  src/Engine/Engine/Loop/EngineLoop.cpp
  src/Engine/Engine/Loop/HeadlessSim.cpp
  src/Engine/Engine/Loop/SimInit.h
  src/Engine/Engine/Loop/SimInit.cpp
  src/Engine/Engine/Loop/TickRunner.h
  src/Engine/Engine/Loop/TickRunner.cpp
  src/Engine/Engine/Script/EngineApiTable.h
  src/Engine/Engine/Physics/Rigid/PhysicsSystem.cpp
  src/Engine/Engine/Physics/Rigid/StaticShapeSource.h
  src/Engine/Engine/Physics/Rigid/Broadphase.h
  src/Engine/Engine/Physics/Rigid/Broadphase.cpp
  src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  src/Editor/Windows/Scene/SceneViewWindow.cpp
  plans/m94-2d/sub-11.md (実装メモ節)
申し送り:
  - 版: SimSnapshot 56 -> 57 のみ (spec 4.4 の宣言どおり)。ABI / Scene kDocVersion / kCookVersion は不変。焼き直した WorldHash 定数は無し (既存型へのフィールド追加なし、既存の焼き値は不動)。新しい焼き値: タイル並走シナリオ 326BA3CD14F4BBB8 (TilemapSelfTest.cpp の kTileScenarioExpectedHash)。
  - sub-12: パレット窓がタイルを塗るときは TilemapLayer::SetCell + TilemapLibrary::Register (アセット編集)。Play 中の sim の書き換えは TilemapSystem::SetCell / FillRect / DamageCell (World と持ち主 EntityID を取る)。tilemap::System() が Editor からも引ける。
  - sub-13: BakeTileChunk は tilemap::System() の実効セルを読む。damage ビット (bit24-31) は実効セルに入っているので、ひび割れの見た目は評価時に Damage(value) を見ればよい。
  - sub-14 (ABI): TilemapSetCell / GetCell / DamageCell は TilemapSystem のインスタンス (EngineApiTable の文脈に TilemapSystem* を足す) の SetCell / GetCell / DamageCell に 1 対 1。座標変換は CellToWorld / WorldToCell (WorldMatrix 基準、1 tick 古い)。セル座標は -32768..32767、範囲外は false。
  - sub-21 (GridAgent2D): 歩行可否は TilemapSystem::GetCell(...) と palette の collider / oneWay で引く。
  - sub-22 / sub-23: タイルの箱は PhysicsSystem が動かす物体の範囲 (速度 2 tick + 2 m) に届くものだけ受け取る。動かさない面にだけ使う Tilemap でも、近づいた物体がいる tick に箱が現れる。
  - タイルの箱どうしのチャンク境界 (16 セル) は別の箱。転がる球で x = 0 の境界を渡る検査は通った (引っかからない)。カプセルや箱が境界の縁を踏んで引っかかる報告が出たら、行方向の結合をチャンクをまたいで行う余地がある。
  - 通常の静的 Collider を消しても眠っている剛体が起きない既存の挙動は残したまま (別件)。
  - cache\m94\s11\ に着手前後のダンプ・ログ・.rep (git 管理外)。一時シーン・一時アセットは使っていない。


## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。受け入れ 21 (350 箱、物理 1 tick 比 0.82、床に乗る / 境界を渡る / 一方通行 剛体 + CC / 床を消すと落ちる)、22 (restore 後 100 tick 一致、Tilemap 無しの HashWorld 548FCE3EEBED7EBC と snapshot 指紋 CBA0926CD0BA0425 が着手前と一致、並走 600 tick + 焼き値 326BA3CD14F4BBB8 両構成同値)、23 (hp 3 → rubble + エフェクト 1 個) を根拠欄で確認。`--demo` の hash-diff が着手前後で identical、既存の焼き値 3 つ不動。[planner] 4 件の裁定: WakeBodiesNear は sim の規則として採用 (spec 4.1.7)、絞り込みの余裕は v1 の限界 (R12 (a))、クエリとタイルは sub-14 の やること 6 (R12 (b))、Broadphase / 範囲の変更は reviewer に重点依頼 (司会へ)。[逸脱] 3 件 (cellKey の詰め方、MarkAllDirty、destroyedTile の向き保持) は採用して spec に反映。
