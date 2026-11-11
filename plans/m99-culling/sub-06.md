# sub-06: 距離カリングと RenderCulling コンポーネント

- 依存: sub-03 (局所影の画面外リストに「入れない」処理をここで確かめるため)
- 状態: OK (コミットは司会)
- 往復: 2

## やること
spec 4.1.3 / 4.2 / 受け入れ条件 6, 10 (と 1, 2 の距離部分)。
- `RenderCullingComponent` ("RenderCulling"): `maxDrawDistance` (float、0..100000、0 = なし)、`exemptFromSizeCulling` (bool、sub-07 が使う。SimSnapshot を 2 回上げないためにここで足す)。`kComponentNoHash`、Add Component に出す。`Components.cpp` の**末尾**に登録 (TypeId は登録順)。**着手時に TypeId の末尾 (現在 99 = CharacterInput と見ている) と `kSimSnapshotVersion` (現在 81) を実コードで確かめ**、TypeId 100 / SimSnapshot v82 (違えば実際の次の番号) にする。`SimSnapshot.h` の版の履歴コメントと、版を固定で検査している selftest (例 `AcousticAudioSelfTest.cpp:129`) を合わせる。ABI (`MYE_API_VERSION` 35) は変えない。
- 距離の判定 (純関数にして単体テストする): 実効距離 = 0 より大きい値の最小 ({物ごと, 各タグ, 既定})、距離 = カメラ位置からワールド AABB の最近点まで。ステージ 2 (並列) で判定し、理由をステージ 3 へ渡す。判定の順序は 視錐台 → 距離 (spec 4.1.4 の最後)。
  - タグ表はフレーム先頭で 64 要素の配列に展開しておく。物のタグは **`Tags::EffectiveMask` (自分 + 祖先)** で引く (round 1 で修正。spec 4.1.3 の継承)。`RenderCulling` も自分 → 祖先の最初の 1 個を使う。解決はステージ 1 (直列) で、ワールドに RenderCulling があるかタグ別の距離があるときだけ祖先を辿る。
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

SELF_EVAL: sub-06 (round 1)
実装:
  - src\Engine\Core\Ecs\Components.h / Components.cpp: RenderCullingComponent (maxDrawDistance 0..100000 / exemptFromSizeCulling / 予約 3 バイト、8 バイト)、"RenderCulling"、kComponentNoHash、登録は末尾 = TypeId 100 (着手時に CharacterInput = 99 を実コードで確認)。
  - src\Engine\Engine\Replay\SimSnapshot.h: kSimSnapshotVersion 81 → 82 と履歴コメント。AcousticAudioSelfTest.cpp の固定検査も 82 へ。ABI (MYE_API_VERSION 35) は不変。
  - src\Engine\Engine\Rendering\DistanceCull.h (新規): 純関数 EffectiveMaxDrawDistance (0 より大きい最小) / DistanceToAabb / ExceedsMaxDistance (d == Dmax は残す) / AppliesToView (viewKey 1・3 は常に、2 はトグル、0 は掛けない)。
  - src\Engine\Engine\Rendering\RenderSystem.h/.cpp: cullSceneView (セッション限り、既定 false)。CullCand に maxDrawDistance / tooFar。ステージ 1 (直列) でアーキタイプごとに RenderCulling と Tag の列を 1 回引いて実効距離を決める (タグ別の距離が 1 つも無ければ Tag の列は引かない)。ステージ 2 (並列) で視錐台の後に距離判定。ステージ 3 で、視錐台の理由が先 (culled)、次に距離 (culledDistance、prof::AddRenderStats)。距離で落とした物は キュー・shadowCasters_・offscreenCasters_ (局所影アトラス)・CSM のフィット・prevRender の記録のどれにも入れない。RT の収集 (rtInstances_) は今どおり (落とす判定より前)。
  - src\Editor\App\EditorApp.cpp / LocalizationTable.inl: Rendering > カリング に「Scene View にもゲームのカリングを掛ける」(保存しない。Menu_CullSceneView / Hint、両言語)。
  - src\Editor\Widgets\EditorComponentCatalog.cpp: Add Component に RenderCulling (Rendering 棚)。
  - src\Engine\Engine\Demo\DemoContentRender.cpp / DemoContent.h / ShowcaseScenes.cpp: `--culling-distance-demo` (別シーンを作った。--culling-demo の counts を変えないため)。太陽 + 床 + 箱 7 個 (距離 29 / 69 / 129、物ごと RenderCulling 40 m・タグ 5・何も付けない物の 3 組)。
  - src\Engine\Engine\Rendering\DistanceCullSelfTest.h/.cpp (新規、suite "distancecull" = Fast): 判定関数、NoHash (WorldHash 不変)、シーン JSON の往復、プレハブ上書き (記録・Revert)。SimSnapshotSelfTest.cpp は RenderCulling を載せたエンティティの復元を追加。SelfTestSuites.cpp に登録。
仕様との差分:
  - [追加] `--culling-demo` へ足さず別シーン `--culling-distance-demo` を作った — 足すと --culling-demo の counts (受け入れ 5) が変わるため。sub-09 の cull_verify では既定距離なしの `--culling-demo` のまま比べられる。
  - [追加] タグの出所は「その物自身の TagComponent::mask」(sub 本文どおり)。祖先のタグは継承しない (RT の適用範囲は EffectiveMask で継承する)。モデルを読み込むとメッシュは子孫エンティティに分かれるので、ルートに付けたタグは子のメッシュに効かない — 下の「不安・質問」。
  - [追加] 箱を持てない物 (alwaysVisible = ラグドール作動中・箱を求められないスキン) は距離でも落とさない (視錐台と同じ保守側)。
  - [追加] RenderCulling に `reserved[3]` を明示 (padding の生バイトを SimSnapshot のバイト一致に載せないため)。
  - [未実装] sub 本文の「exemptFromSizeCulling は sub-07 が使う」: このサブでは欄を足しただけで、読む側は sub-07。
検証:
  - Debug / Release ビルド (MyEngine.sln) → どちらも成功。tools\gen_project_files.ps1 は実行 (build\Engine.vcxproj と .filters が更新)。
  - `MYE_SELFTEST_ONLY=distancecull,simsnapshot,sceneserializer,acousticaudio,enginecli,engineindexdump,renderstats,worldhasher,localization,tag,ecs,decal` で Debug `Editor.exe --selftest` → 12 suites 0 failed。distancecull / simsnapshot は Release でも PASS。1 回目の distancecull は私のテストの期待値の算術ミス (3-4-5 の座標) で FAIL → 直して PASS。
  - `--culling-distance-demo --warp` Debug の `--render-stats-dump`: 既定 (距離設定なし) は culledDistance 1 (PerObject_Far)、triangles 84、shadowCasterCandidates 7。`--cull-max-distance 40` は culledDistance 4、triangles 48、shadowCasterCandidates 4、shadowCascadeCasters [1,4,6] → [1,4,4]。`--cull-max-distance 40 --no-distance-culling` は culledDistance 0、triangles 96、candidates 8 (全部描く)。Release --warp の base / d40 の counts は Debug と JSON 完全一致。
  - Editor.exe (Scene View = viewKey 2) の `--culling-distance-demo --cull-max-distance 40 --render-stats-dump` → viewKey 2 のみ、culledDistance 0 (既定で Scene View は落とさない)。トグル ON で落ちる経路は AppliesToView の単体テストだけ (UI の操作は未確認)。
  - 受け入れ 5: 距離設定なしの `--culling-demo --particle-backend gpu` と `--render-bench-demo` (Runtime、--warp) の counts が、`--no-distance-culling` と Debug / Release の 4 通りで JSON 完全一致。値は sub-04 / sub-03 の実装メモの数字と一致 (culling-demo: culledNonMesh 4・unculledGpuEmitters 3、bench: drawCalls 23・culled 308・shadowCasterCandidates 3686)。
  - `tools\check_rules.ps1` → 0 error (60 warning は既存の絶対パス警告)。
  - `tools\replay_verify.bat` → PASS (24 jobs、Debug/Release の 17 シーン一致 + snapshot 往復 + check_rules 0 error)。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜6 を検証。Scene View のトグル ON の実描画と UI は未確認。
  正しさ: 4 — 3 出所・最小値・境界・影の除去・viewKey 2 の既定を dump と単体テストで確認。タグ列を引く経路 (RenderSystem 内) は実描画で未確認 (project_settings.json を書き換えない方針)。
  コード品質: 4 — 判定は純関数に分離、ステージの規約 (直列で解決・並列は純関数) を保った。
  テスト: 4 — 判定・NoHash・保存・プレハブ・SimSnapshot 往復を追加。RenderSystem 統合は dump で確認、自動テスト化はしていない。
不安・質問:
  - タグの出所を自分のタグだけにした (sub 本文どおり)。FBX / glTF のモデルはメッシュが子孫エンティティなので、ルートに付けたタグが効かない。RT のフィルタ (Tags::EffectiveMask = 祖先を辿る) に揃えるかは planner の判断 (揃えるなら祖先を辿るコストがステージ 1 に載るので、タグ別の距離が 1 つでもあるときだけ辿る形になる)。
触ったファイル:
  src\Engine\Core\Ecs\Components.h, src\Engine\Core\Ecs\Components.cpp, src\Engine\Engine\Replay\SimSnapshot.h, src\Engine\Engine\Replay\SimSnapshotSelfTest.cpp, src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp, src\Engine\Engine\Rendering\DistanceCull.h (新規), src\Engine\Engine\Rendering\DistanceCullSelfTest.h (新規), src\Engine\Engine\Rendering\DistanceCullSelfTest.cpp (新規), src\Engine\Engine\Rendering\RenderSystem.h, src\Engine\Engine\Rendering\RenderSystem.cpp, src\Engine\Engine\Demo\DemoContentRender.cpp, src\Engine\Engine\Demo\DemoContent.h, src\Engine\Engine\Demo\ShowcaseScenes.cpp, src\Editor\App\EditorApp.cpp, src\Editor\Widgets\EditorComponentCatalog.cpp, src\Editor\SelfTest\SelfTestSuites.cpp, src\Engine\Core\Localization\LocalizationTable.inl, plans\m99-culling\sub-06.md (実装メモ)
申し送り:
  - build\Engine.vcxproj と build\Engine.vcxproj.filters は gen_project_files.ps1 で再生成されている (新規 3 ファイルを含む)。コミットに含めること。
  - sub-07 は RenderCullingComponent::exemptFromSizeCulling をステージ 1 で読む (CullCand にはまだ載せていない)。距離カリングの viewKey 規則は distancecull::AppliesToView / RenderSystem::cullSceneView を画面サイズにも使う。ステージ 3 の「視錐台 → 距離 → 画面サイズ」の順は `if (c.tooFar)` の後ろへ足す。
  - sub-08: 理由別の枠表示は CullCand::tooFar を使える。カメラ固定は cameraPos / 距離判定の入力 (view.cameraPos) を控えへ差し替える。
  - Scene View のトグル ON と UI は画面で未確認 (ユーザーの目視)。

SELF_EVAL: sub-06 (round 2)
実装:
  - #1 (a)(b)(d): DistanceCull.h に ResolveMaxDrawDistance(World, e, useTags, tagTable, default, outExempt) を追加。タグは自分 + 祖先の OR、RenderCulling は自分 → 祖先で最初の 1 個 (子が祖先を丸ごと上書き)。exemptFromSizeCulling も同じ 1 個から返す (sub-07 用)。
  - #1 (c): RenderSystem.cpp ステージ 1 を差し替え。存在の門 = RenderCulling がワールドに 1 つでもある or タグ別の距離が 1 つでもある。門が閉じていれば祖先を辿らず既定距離だけを使う。
  - #1 (d): DistanceCullSelfTest に階層テスト (ルートのタグが孫に効く / ルートの RenderCulling が孫に効く / 子が祖先を上書き / 何も無ければ 0 など) を追加。
  - #1 (e): --culling-distance-demo の末尾に Inherit_Root (RenderCulling 40 m) と子 Inherit_ChildFar (距離およそ 69) を追加。
仕様との差分:
  - なし
検証:
  - Debug ビルド成功。MYE_SELFTEST_ONLY=distancecull,simsnapshot,tag,sceneserializer → 4 suites 0 failed。
  - --culling-distance-demo --warp (Debug) dump: 既定は culledDistance 2 (PerObject_Far と Inherit_ChildFar)、triangles 84。--cull-max-distance 40 は culledDistance 5、triangles 48、shadowCasterCandidates 4。子のメッシュが描画経路でも祖先の距離で落ちる。
  - (f) 門が閉じた状態の --culling-demo (gpu) と --render-bench-demo の counts は round 1 と JSON 完全一致 (Debug --warp)。check_rules 0 error。
  - 未実施: round 2 の変更後の Release ビルド・replay_verify の再実行 (round 1 では PASS。変更は描画側の距離解決のみで sim に触れない)。
自己採点 (1-5):
  仕様適合: 4 — 指摘 (a)-(f) に対応。
  正しさ: 4 — 継承を単体テストと dump で確認。タグ別距離の実描画は未確認のまま。
  コード品質: 4 — 解決を純関数に切り出した。
  テスト: 4 — 階層 4 項目を追加。
不安・質問: なし
触ったファイル: src\Engine\Engine\Rendering\DistanceCull.h, src\Engine\Engine\Rendering\DistanceCullSelfTest.cpp, src\Engine\Engine\Rendering\RenderSystem.cpp, src\Engine\Engine\Demo\DemoContentRender.cpp, plans\m99-culling\sub-06.md
申し送り: sub-07 は ResolveMaxDrawDistance の outExempt を使える (ただし門が閉じると呼ばれないので、exempt を読むなら門の条件に画面サイズ ON を足すこと)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-11)。must 1 件 = タグと RenderCulling の継承 (仕様側の誤り。spec 4.1.3 / 4.2 を修正済み)。それ以外 (TypeId 100 / SimSnapshot v82、判定関数、影の除去、viewKey、別シーン、replay_verify PASS) は確認済みで変更不要。
- round 2: VERDICT OK (planner、2026-11-11)。must #1 の (a)〜(f) を確認: `ResolveMaxDrawDistance` (DistanceCull.h:45-68) が RenderCulling を自分 → 祖先の最初の 1 個、タグを祖先まで OR で解決。階層テスト 7 項目、dump で Inherit_ChildFar が描画経路でも落ちる、門が閉じた状態の counts が round 1 と一致。should 1 件 (タグの継承を `Tags::OwnMask` の OR で書き直しており、`Tags.h` の「規則はここの 1 本きり」に反する) は sub-07 で直す (sub-07.md に転記)。Release ビルドと replay_verify の再実行は sub-07 の検証で兼ねる。
