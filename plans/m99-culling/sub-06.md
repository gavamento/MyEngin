# sub-06: 距離カリングと RenderCulling コンポーネント

- 依存: sub-03 (局所影の画面外リストに「入れない」処理をここで確かめるため)
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.3 / 4.2 / 受け入れ条件 6, 10 (と 1, 2 の距離部分)。
- `RenderCullingComponent` ("RenderCulling"): `maxDrawDistance` (float、0..100000、0 = なし)、`exemptFromSizeCulling` (bool、sub-07 が使う。SimSnapshot を 2 回上げないためにここで足す)。`kComponentNoHash`、Add Component に出す。`Components.cpp` の**末尾**に登録 (TypeId は登録順)。**着手時に TypeId の末尾 (現在 99 = CharacterInput と見ている) と `kSimSnapshotVersion` (現在 81) を実コードで確かめ**、TypeId 100 / SimSnapshot v82 (違えば実際の次の番号) にする。`SimSnapshot.h` の版の履歴コメントと、版を固定で検査している selftest (例 `AcousticAudioSelfTest.cpp:129`) を合わせる。ABI (`MYE_API_VERSION` 35) は変えない。
- 距離の判定 (純関数にして単体テストする): 実効距離 = 0 より大きい値の最小 ({物ごと, 各タグ, 既定})、距離 = カメラ位置からワールド AABB の最近点まで。ステージ 2 (並列) で判定し、理由をステージ 3 へ渡す。判定の順序は 視錐台 → 距離 (spec 4.1.4 の最後)。
  - タグ表はフレーム先頭で 64 要素の配列に展開しておき、物の `TagComponent::mask` のビットを引く (ステージ 1 で 1 回)。
- 掛けるビュー: viewKey 1 / 3、2 は「Scene View にもゲームのカリングを掛ける」(Rendering > カリング、セッション限り、既定 OFF) が ON のときだけ、0 は掛けない。
- 落とした物: キューに入れない、CSM / アトラスのキャスター (画面外リスト含む) に入れない、CSM のフィットに入れない、`culledDistance` に数える。RT の収集 (`rtInstances_`) は今どおり。
- `--cull-max-distance D` を既定距離へ反映 (sub-02 で配線済みの値を使う)。
- Project Settings のタグ別距離の表・既定距離が効くようになる (UI は sub-02 で作成済み)。
- `--culling-demo` の**末尾に**、距離の検証用の物 (既定距離・タグ・物ごとの 3 出所) を足すか、dump 用の別シーンを作る (どちらにしたか実装メモ)。

## やらないこと (このサブでは)
- 画面サイズの判定 (sub-07)
- メッシュ以外への距離カリング

## 触る場所 (planner の見立て)
- `src\Engine\Core\Ecs\Components.{h,cpp}`、`src\Engine\Engine\Replay\SimSnapshot.h` と SimSnapshot の selftest
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (`CullCand` に理由と距離の上限、ステージ 1〜3)
- `src\Editor\App\EditorApp.cpp` (Scene View のトグル)、`ProjectSettingsWindow.cpp`
- シリアライズ / プレハブ上書きの selftest (既存のリフレクション経路に載るか確かめる)
- `src\Engine\Core\Localization\LocalizationTable.inl` (フィールド名は `MYE_JP` で日本語名を付ける)

## 受け入れ条件 (このサブ)
1. 判定関数の単体テスト: 3 出所それぞれ、最小値の規則、全部 0 で落とさない、カメラが AABB の内側で 0、境界ちょうど (d == Dmax は残す)
2. ビュー: viewKey 2 は既定で落とさずトグルで落とす、viewKey 0 は落とさない
3. 落とした物の影が消える (CSM / アトラスの描画数が減る)、`culledDistance` が増える (dump)
4. RenderCulling の保存 → 読み込み、プレハブ上書き、WorldHash 不変、SimSnapshot 新版の往復 (既存 selftest に追加)
5. 距離が 1 つも設定されていない既定状態で、既定ベンチと `--culling-demo` の `counts` が sub-05 / sub-04 後と一致
6. `counts` が Debug / Release / `--warp` で一致

## 検証コマンド
- Debug / Release ビルド、`tools\gen_project_files.ps1` (ファイル追加時)
- `MYE_SELFTEST_ONLY=SimSnapshot,Serialization,Prefab,<距離のテスト>,...` で `Editor.exe --selftest`
- `--render-stats-dump` を `--cull-max-distance` あり / なしで
- `tools\check_rules.ps1`、`tools\replay_verify.bat` (SimSnapshot を上げたため)

## 実装メモ (coder が追記)

## フィードバック履歴
