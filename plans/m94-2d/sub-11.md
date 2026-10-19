# sub-11: タイル衝突 + セル書き換えの sim 状態 + 破壊

- 依存: sub-08, sub-10
- 状態: 未着手
- 往復: 0
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
4. (spec 1-5) golden 不変、replay_verify 全 PASS (既存 14 ジョブの hash は Tilemap 無しなので不変 = (e) で確認)、0 警告、check_rules 0、snapshot +1 のみ。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`pwsh tools\check_rules.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
