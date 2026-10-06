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

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Runtime.exe <アセットのみのシーン> --simulate-device-lost 30`
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
