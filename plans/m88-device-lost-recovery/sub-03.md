# sub-03: アセットの GPU 再アップロード (ID 維持)

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
spec 4.2 の再生成レシピを `RenderResources` (`MeshLibrary` / `TextureLibrary` / `MaterialLibrary` / `SkinnedModelLibrary` の GPU 部分) に持たせ、
復旧手順の中で「GPU 側だけ解放」→「同じ AssetID のまま GPU 側だけ作り直す」を行う。

1. MeshLibrary: `Register` 時に `MeshVertex` 列 (ボーン index/weight 含む) とインデックスを保持し、`vb/ib` を作り直す。sim が読む `positions/indices/normals/uvs` と AABB は**一切書き換えない** (同じ値の再代入もしない)。既存フィールドとの重複保持で済ませるか、統合するかは coder 判断 (メモリ増を実測して記録)。
2. TextureLibrary: 作成元ごとのレシピ (ファイル + srgb / エンコード済みバイト列のコピー / RGBA8 のコピー + mips/srgb / 単色 / DDS)。`ReplaceFromFile` 等で後から差し替えられた場合はレシピも更新。非同期読み込み (`RequestLoadFileAsync` / `AsyncWorker`) は復旧前に排出し、復旧中に完了した結果を旧デバイスで作らないこと (R4)。ファイルが消えていた場合は既存の読み込み失敗時と同じ代替 (白など) にしてログ、復旧自体は続行。
3. MaterialLibrary: `SurfaceMaterialState::perMaterialGpuCB` などの GPU オブジェクトを手放し、次の描画で再ビルドされる状態に戻す (既存の再パック判定 `builtFromRevision/Generation` の仕組みを使えるか確認)。
4. SkinnedModelLibrary 等、他に GPU を持つものがあれば同様。
5. SelfTest: (a) 復旧ルーチンの前後で World の状態ハッシュが一致 (spec 9)。(b) 代表的なメッシュ・テクスチャの AssetID が復旧後も同じで、CPU 側データが変わっていない。(c) レシピからの再生成で作ったテクスチャを読み戻して元と一致 (少なくとも RGBA8 と単色)。

## やらないこと (このサブでは)
- UI/VFX/粒子/RT/Probe/compute (sub-04)、エディタ (sub-05)。

## 触る場所 (planner の見立て)
- `C:\HAL\MyEngin\src\Engine\Renderer\Device\GpuResources.h:47-200, 229-260, 329-343`、`GpuResources.cpp`
- モデル読み込み経路 (GLB の埋め込みテクスチャ → `CreateFromEncoded`)、地形などの生成テクスチャ (`CreateFromRgba8` 呼び出し元 — レシピ側で吸収するので呼び出し元は原則変えない)
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp` の復旧手順に組み込む

## 受け入れ条件 (このサブ)
1. (spec 9) SelfTest: 復旧前後の World ハッシュ一致。
2. SelfTest: AssetID 不変・CPU 側データ不変・テクスチャ再生成の一致。
3. ゲートの残参照数が sub-02 時点より減っていること (代表シーン、ログの数値を前後で記録)。アセットだけのシーン (メッシュ + テクスチャ + マテリアル、粒子/UI/RT なし) なら Runtime で復旧成功・ゲート合格。
4. メモリ増の実測 (demo シーン + 三校の代表シーン 1 つ) を実装メモに記録。
5. (spec 14) selftest (Debug/Release)、check_rules、replay_verify。
6. 検証専用の `--simulate-device-lost-drop-assets` を丸ごと削除し (EngineConfig::simulateDeviceLostDropAssets、DiscardAssetsForTest / MeshLibrary::DiscardAll / TextureLibrary::DiscardAll、EngineLoop の呼び出し、EngineCli、EngineCliSelfTest)、フラグ無しの Runtime 空シーン (デモ資産あり) で復旧が成功し、ゲートが合格する (sub-02 時点の残参照は 6)。
7. 既存の入口 `RenderResources::ReleaseBuiltinGpu / RecreateBuiltins` (sub-02) を、組込み以外のメッシュ・テクスチャ・マテリアルにも広げる。別の入口を並べない。
8. 読み込み中の非同期テクスチャ (プレースホルダ) を抱えたまま復旧しても、完了後に正しいテクスチャへ差し替わる (R4)。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Runtime.exe <アセットのみのシーン> --simulate-device-lost 30`
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要点)
- 入口は `RenderResources::ReleaseGpu / RecreateGpu(device)` の 1 組。メッシュ / テクスチャ / マテリアルを同じ入口で扱う (sub-02 の `ReleaseBuiltinGpu / RecreateBuiltins` を改名して範囲を広げた。別の入口は無い)。`RecreateGpu` は作れなかった数を返し、EngineLoop が所要 ms と失敗数をログに出す。
- メッシュ: 組込みも含め全メッシュを同じ経路 (CPU 側 positions/normals/uvs + 新設の `Mesh::skin` から MeshVertex を復元 → `UploadBuffers`) で作り直す。positions/indices/normals/uvs/AABB は読むだけで書かない。スキン属性が全頂点 0 の非スキンは `skin` を持たない。
- テクスチャ: `TextureRecipe` (File / Encoded / Rgba8。単色は Rgba8)。LoadFile / CreateFromEncoded / CreateSolid / CreateFromRgba8 / ReplaceFromFile / 非同期完了で記録。ファイルは `LoadFileInto` (DDS・配布用 sibling DDS・画像の共通隘路) で現ディスクから読み直す。エントリは消さずに tex/srv だけ手放すので `Get()` のポインタは復旧をまたいで有効。読み込み中 (pending_) とレシピ無し (非同期デコード失敗) と作り直し失敗は、新しい White を共有するプレースホルダにする。
- マテリアル: `MaterialLibrary::ReleaseGpu` が surface 状態の `perMaterialGpuCB` を手放し `builtFromRevision = 0` にして、次の `GetOrBuildSurfaceState` が必ず再パックして CB を作り直す。
- SkinnedModelLibrary は GPU を持たない (確認済み: Mesh/ 配下に D3D 型は MeshInstancing / MeshBind のみで、前者は RenderSystem 側の所有)。
- 削除したもの: `--simulate-device-lost-drop-assets` 一式 (EngineConfig / EngineCli / EngineCliSelfTest / EngineLoop / DiscardAssetsForTest / DiscardAll)。
- 検証:
  - DeviceRecoverySelfTest に `CheckAssetRecovery` を追加 (WARP): スキン付き・非スキンメッシュと組込み、各作成元のテクスチャ (白 / RGBA8 srgb+mips / RGBA8 mips 無し / 単色 / エンコード済み BMP / ファイル / 読み込み中の非同期) を登録 → `ReleaseGpu` → `RecycleDevice` (ゲート合格を確認) → `RecreateGpu`。メッシュは CPU 側をビット比較 + vb/ib を読み戻して比較、テクスチャは mip0 画素 + フォーマット + mip 数を読み戻して比較、`Get()` ポインタ不変、Enumerate 件数不変、Material 不変、World の `HashWorld` 不変、読み込み中は復旧直後に有効なプレースホルダで `WaitForAsyncLoads` 後にデコード済み画素と一致、ファイルを消すと失敗 1 件で白に落ちて残りは復旧。Debug / Release とも ALL PASS。
  - ゲートの残参照数 (旧デバイスの外部参照): sub-02 時点の Runtime 空シーン (デモ資産あり) は 6 → フラグ無しで 0 (ゲート合格、復旧成功、exit 0)。HW / `--warp`、`--simulate-device-lost 30,60` の 2 回とも成功。既定のデモ (ラボのモデル込み、390 メッシュ・56 テクスチャ) も 0 で成功、三校 `main.scene.json` も 0 で成功。いずれも World ハッシュ unchanged=1。
  - 所要時間 (Release、ログの `assets recreated`): 空シーン 1.5 ms (デモ資産のみ)、三校 2.6 ms、既定デモ 約 4.0 s (2048x2048 の PNG をディスクから 56 枚デコードし直す。1 枚 70〜100 ms)。Debug は既定デモで 12.7 s (stb が最適化なしのため)。復旧全体は Release で既定デモ 4.4 s、空シーン 0.3 s。
  - メモリ増 (CPU 保持の追加分、Release 実測): 既定デモ = テクスチャのレシピ 3.6 MB (GLB 埋め込みのエンコード済みバイト列。ファイル由来はパスだけ) + スキン属性 1.2 MB (スキンメッシュ分だけ。非スキンは 0)、既存の positions/normals/uvs/indices は 957,663 頂点で約 30 MB なので増分は 1 割強。三校 main = テクスチャ 0.28 MB + スキン 0。
  - `Editor.exe --selftest` Debug / Release → exit 0 (実装直後の Debug 1 回目は既知 flake の net V1 LoadPersist/LoadGame 2 件だけで exit 1。最終ビルドの Debug / Release は 1 回目から exit 0)。`tools\check_rules.ps1` → 0 error (rule 7 警告は GpuResources.cpp に 5 行増 = いずれも tex/mesh/material の全件リセットか、ID を集めてソートする走査で順序非依存)。`tools\replay_verify.bat` → 17 job 全 PASS。

## フィードバック履歴
- round 1: VERDICT OK (planner)。復旧時間 (Release 約 4 s / Debug 約 13 s) を許容、アセットの作り直しに失敗しても続行、を spec 8. に記録した。surface CB の検証と、vb/ib が null のときの描画の安全確認は sub-04 へ移した。
