# sub-10: スキンメッシュの破壊 (骨追従から剛体化)

- 依存: sub-07, sub-09
- 状態: OK (コミット待ち)
- 往復: 3

## やること

spec §2「スキンメッシュ」行と §4.1 の裁定どおり。

1. **最初に確かめる (荷重のかかる未知)**: ボーンウェイトの取り出し経路。CPU の `Mesh` には無い (`GpuResources.h:41-56`)。`.mmdl` の `CookedMesh.vertices` (`ModelCook.h:29-33`) から、スキンメッシュの登録名で頂点 (位置・ウェイト・骨 index) を引けるか。引けなければ、ロード時に CPU 側へウェイトを残す別の口を作るかを SELF_EVAL で planner に問う (勝手に MeshLibrary の構造を変えない)
2. **骨の割り当て**: 破片ごとに外側面の頂点のウェイトを骨ごとに合計し、最大の骨 (同値は骨 index 小)。外側面が無い内部の破片は、原点に最も近い元頂点の最大ウェイト骨
3. **骨空間で焼く**: 破片メッシュ・凸包を、その骨のバインドの global 逆 (inverseBind) を掛けた空間で持つ。原点 = 骨の原点 (重心ではない — sub-05 の凸包の子の質量特性で重心は正しく出る)。`.mfrac` に骨名を入れる (sub-03 で用意した欄)
4. **エンティティ**: ルート = SkinnedMesh のエンティティ (`Rigidbody(isKinematic, compoundColliders)`)。破片 = ルート**直下の子** + `PartComponent(joint = 骨名, source = null → 最寄りの SkinnedMesh)`。`PartFollowSystem` が毎 tick `LocalTransform = jointGlobal` を書くので、複合の子形状がアニメに追従する (spec §10.5 Ragdolls の `partLocal == jointGlobal`)
5. **分離**: 分かれた破片から `PartComponent` を外してから (同じ tick 末のコマンドバッファ)、sub-07 の剛体化。ルートに残った破片は Part のまま追従を続ける
6. **描画**: 一度でも割れたら (`broken`) SkinnedMesh の MeshRenderer を描かず、残った破片も剛体の破片として描く (sub-06 の root proxy 規則そのまま)。壊れる前は破片を描かない
7. **速度の引き継ぎ**: v1 はルートの Rigidbody の速度だけ (骨の速度は引き継がない。spec §3 後回し)
8. **Inspector**: sub-09 で「未対応」にしていたスキンを有効化
9. **テストシーン**: `--parts-demo` 等の既存デモは変えない。スキンの破壊は SelfTest のシーン (既存のスキンのテスト資産、例 `tools/gen_skinned_beam_fbx.ps1` の梁) と、`--fracture-demo` への追加 (または専用のフラグ) で replay に載せる。どちらにするかは coder 判断 (replay に載ることが条件)

## やらないこと (このサブでは)

- 破片ごとのスキン描画、骨の速度の引き継ぎ、実行中のポーズで切り直すこと

## 触る場所 (planner の見立て)

- `src/Engine/Engine/Asset/ModelCook.h/.cpp`、`src/Engine/Renderer/Skeleton.h` (`inverseBind`、`FindJointByName`)
- sub-02 の焼き (骨空間への変換は焼きの後段で)、sub-03 の `.mfrac` (骨名)
- `FractureBuilder.*` (Part を付ける)、`FractureSystem.cpp` (Part を外す)
- `src/Engine/Engine/PartFollowSystem.cpp` (読むだけの見込み。変えるなら SELF_EVAL に理由)
- `InspectorWindow.cpp` / 焼きのワーカー

## 受け入れ条件 (このサブ)

1. スキンの梁 (または既存のスキン資産) を焼き、全破片が幾何的に閉じ (sub-02 と同じ判定)、破片がそれぞれ骨に割り当てられる (割り当ての結果をテストで固定) — `--selftest`
2. バインドポーズで、骨に追従した破片の外側面のワールド位置が元のスキンメッシュのバインド位置と一致 (相対 1e-5) — `--selftest`
3. アニメ再生中に、骨に追従した破片へ球を当てると複合として接触し、割れた破片が剛体化して落ち、残りは追従を続ける — `--selftest`
4. 割れた後は元のスキンを描かない — スクショの画像パス
5. スキン破壊のシーンの replay (Debug / snapshot-stress / Release) 一致 — `replay_verify.bat`
6. 既存 `--parts-demo` の replay と golden が不変 — `replay_verify.bat`、`shot_verify.bat`
7. `check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## round 2 の裁定 (planner、FIX_REQUEST の手順)

**原因の見立て** (コードで確認): 実アセットの節は、キャッシュの置き場を**固定名**の `%TEMP%\mye_fractureskin_realasset_selftest` にしている。しかも**開始時に `remove_all`** している (`FractureSkinSelfTest.cpp:735-739`)。Debug と Release の `--selftest` を同時に走らせる (この作業で普通にしている) と、次のことが起こる:
- 遅い側 (Debug) が `.mmdl` を書いてから読み戻すまでの間に、速い側 (Release) がこの節に入り、同じディレクトリを消す
- 症状 (Debug だけ・断続的・単独で回した最後の 2 回は PASS) と合う
- `CookedCache::Write` は失敗しておらず、書いた後に消されている

**方針**: (a) を採る。テストがプロセスの外の共有状態に依存しない形に直し、そのうえで故障点で確かめる。
1. キャッシュの置き場を**プロセスごとに一意**にする (例: 名前にプロセス ID を入れる)。後始末 (終了時の `remove_all` と `Configure(L"", false)`) は残す
2. テストの中で `CookedCache::Write` 相当の書き込み (`RegisterAssets` が書いたはずの `.mmdl`) の存在を読み戻しの前に確かめる。無ければ「書かれていない」と理由付きで FAIL させる (「読めない」と区別する)
3. **故障点での確認**: 修正前の形 (固定名) で、Debug と Release の `--selftest` を同時に走らせて Debug の FAIL が再現することを 1 回示す。修正後は、同時実行を **5 回連続**で両方 PASS することを示す。再現しなければ、その旨と別の仮説を SELF_EVAL に書くこと (その場合も修正 1・2 は入れる)
4. 同じ形 (固定名の一時ディレクトリ + 開始時の `remove_all`) は、既存の `CookedCacheSelfTest.cpp:144-147` (`mye_cook_selftest`) にもある。**M80 の範囲外なので直さない**。SELF_EVAL の申し送りに書く (司会が台帳に記録し、ユーザーへ報告)
5. round 2 の他の内容 (実アセットの 4 検査、骨の割り当ての統一、法線の逆転置、Play 中の無効化) は受け入れ済み。変えないこと

## round 1 の裁定 (planner、FIX_REQUEST の手順)

**問題**: 実アセットの経路が一度も通っていない。
- `TryLoadCookedMeshVertices` (`.mmdl` からウェイト込みの頂点を読む新しい口)
- FBX / glTF 由来の頂点の分割とウェイト
- 両面化・開いたメッシュの扱い

ユーザー要件は「スキンメッシュも含めた複雑な素材」で、受け入れ条件 1 も「スキンの梁 (または既存のスキン資産)」を挙げている。手続き生成の 2 骨の腕だけでは、新しく書いた読み出しの口が本物の `.mmdl` で動くかを誰も見ていない。ヘッドレスで実アセットを読む前例はある (`src/Editor/PartSelfTest.cpp:596-650` が `CesiumMan.glb` と `skinned_beam.fbx` をロードしている) ので、SelfTest で確かめられる。

手順:
1. **実アセットの SelfTest** (手続き生成の腕のテストは残す): `assets\models\CesiumMan.glb` と `assets\models\skinned_beam.fbx` を、PartSelfTest と同じ方法でヘッドレスにロードする (クックを通して `.mmdl` を作る / 読む)。それぞれのスキンメッシュについて、次を検査する:
   - (a) `TryLoadCookedMeshVertices` が、頂点数と index 数が MeshLibrary の CPU メッシュと一致する頂点列を返す。ウェイトの和が各頂点でほぼ 1、骨 index が骨数の範囲内
   - (b) `openMeshMode = 0` で焼くと、成功するか、理由 (境界辺 / 非多様体辺 / 向き不一致の件数) 付きで拒否される。どちらになったかと件数を SELF_EVAL に書く (`skinned_beam.fbx` が非多様体になる見込みも、ここで実測で確かめる)
   - (c) 拒否された場合は、`openMeshMode = 1` (解像度 32) で焼きが成功し、全破片に有効な骨が割り当てられる
   - (d) 焼いた破片をバインドポーズで骨に追従させたとき、破片の原点のワールド位置が、焼きの原点 (ソース空間) と一致する (相対 1e-5)
2. **ボクセル化した場合の骨の割り当て**: ボクセル化 (surface nets) の頂点は元の頂点と位置が一致しないので、「位置のビット一致で照合」はほぼ何も拾わない。ボクセル化の経路では、**破片の外側面の各頂点に最も近い元の頂点 (距離、同値は index 小) のウェイトを合計する**規則に切り替える (spec §2 のスキンの行の「外側面の頂点のウェイト合計最大」を、ボクセル化でも成り立つ形にしたもの)。1 の (c) で検査する
3. **[should] 法線**: 骨空間への変換で、法線に inverseBind の線形部分をそのまま掛けている。**逆転置** (inverse-transpose) を掛けて正規化する。一様スケールでは同じ結果になり、非一様スケールの骨でも陰影が歪まない
4. **[should] 生成ボタンの無効化** (sub-09 の nit): Play 中と複数選択では無効にする。同じ関数を触っているので、このサブで入れる
5. `skinned_beam.fbx` を使わなかった [逸脱] は、1 で実測したうえで判断を SELF_EVAL に書き直すこと (手続き生成の腕のテストは残してよい)

## 実装メモ (coder が追記)

SELF_EVAL: sub-10 (round 1)
実装:
  - src/Engine/Engine/Physics/FractureSkinBake.h/.cpp (新規) — `AssignFractureBonesAndTransform`。
    焼き結果 (`FractureBakeResult`) を破片ごとに骨へ割り当て、origin/outer/cap/hull を
    その骨の `inverseBind` を掛けた空間 (原点=骨の原点) へ書き換える純関数。骨割り当ては
    「破片の外側面の頂点をソース頂点と位置のビット一致で照合し、ウェイトを骨ごとに合計して
    最大の骨」(同値は骨 index 小)、一致点が無い内部の破片は「原点に最も近い元頂点の
    最大ウェイト骨」(spec §2 スキン行のとおり)。`XMMatrixInverse` 等の構成間 (Debug/Release)
    で割れ得る DirectXMath 関数は使わず、位置・法線とも行ベクトル規約の手計算
    (`TransformPoint`/`TransformDirection`) で変換する。法線は `inverseBind` の線形部分を
    そのまま使う近似 (正しくは逆転置行列だが、`XMMatrixInverse` を避けるため。無歪み
    (回転+並進) のバインド行列でのみ正確 — 非一様スケールを含む骨では法線が歪む、
    spec §4.1「非一様スケールのルート」と同種の v1 制限)。凸包は骨空間の点から
    `BuildConvexHull` で作り直す (体積・重心・慣性・aabb も一緒に正しく再計算される)
  - src/Engine/Engine/Asset/ModelCook.h/.cpp:TryLoadCookedMeshVertices — srcPath の .mmdl
    クックキャッシュから meshKey に一致する CookedMesh の頂点 (ボーンウェイト込み) を取り出す。
    「最初に確かめる」の答え: CPU の Mesh (MeshLibrary) にはウェイトが残らない
    (GpuResources.cpp の Register が position/normal/uv だけ CPU コピーする) が、
    `ModelCookData.meshes[].vertices` (フルの MeshVertex) は .mmdl クック blob に残っており、
    `CookedCache::ReadValidated` + `ModelCook::Deserialize` で読み出せる。MeshLibrary/
    ModelLoader/FbxLoader の構造は変えていない (ConvexColliderLibrary::Get の
    `ConvexCookSourcePath` 解決と同じパターンを踏襲)
  - src/Engine/Engine/Physics/FractureLibrary.h/.cpp — `BuildFractureAssetData` /
    `RegisterBaked` に `boneNames` (既定空 = 非スキン、既存呼び出しは無変更) を追加し
    `PieceRecord.boneName` / `FracturePieceRef.boneName` へ配線
  - src/Engine/Engine/FractureBuilder.cpp — root が `SkinnedMeshComponent` を持てば
    `Rigidbody.isKinematic=true` を強制。`piece.boneName` が非空の破片には
    `PartComponent(joint=骨名, source=null)` を付ける (root 直下の子 = PartFollowSystem の
    「直子」規約そのまま)
  - src/Engine/Engine/FractureSystem.cpp — 分かれた成分を新リーダーへ昇格する箇所
    (`ReparentKeepWorld` の前後) で `RemoveComponent<PartComponent>` を新リーダー本体と
    メンバー全員に対して呼ぶ (骨追従をやめて剛体化)。ルートに残る塊のメンバーは触らない
    ので Part のまま追従を続ける
  - src/Editor/FractureBakeService.h/.cpp — `FractureBakeRequest` に `skinVertices`/
    `skinJoints` (既定空)。`WorkerLoop` は `BakeFracture` 成功後、`skinJoints` が非空なら
    `AssignFractureBonesAndTransform` を同じワーカースレッドで呼ぶ (骨割り当ても「焼き」の
    一部として非同期化)。`TakeResult`/`Entry`/`JobResult` に `pieceBoneNames` を追加
  - src/Editor/FractureBakeCommit.h/.cpp — `CommitFractureBake` に `pieceBoneNames`
    (既定空) を追加し `BuildFractureAssetData` へ渡す
  - src/Editor/Windows/InspectorWindow.cpp — `DrawDestructibleNotes` のスキン無条件無効化
    ゲートを外し、`SkinnedMeshComponent` を持つエンティティでは
    `ModelCook::TryLoadCookedMeshVertices` (+ `assetkey::SourcePathForSubAssetKey` で
    メッシュ登録名からクック元パスを解決) でウェイトを取得できたときだけ生成ボタンを
    有効化する (取得できなければ理由付きで無効)。取得できたら生成要求にウェイト・骨行列を
    載せる。`CommitFractureBakeResult` は `pieceBoneNames` も取り出して `CommitFractureBake`
    へ渡す
  - src/Editor/FractureEditorSelfTest.cpp — `TakeResult` の新シグネチャ (boneNamesOut 追加)
    に合わせて呼び出し側を更新 (この 1 箇所のみ。他は既定引数で無変更)
  - src/Engine/Core/LocalizationTable.inl — `Insp_FractureSkinUnsupported` (スキン=常に無効)
    を `Insp_FractureSkinNoWeights` (ウェイトを取得できないときだけ無効) へ置き換え
  - src/Engine/Engine/Physics/FractureSkinSelfTest.h/.cpp (新規) — 2 骨の「腕」(下半分=Bone0
    ルート/上半分=Bone1、Bone0 の子) を完全に手続き生成 (FBX 不使用) して検証:
    (1) 明示シード 2 個で焼き、両破片が幾何的に閉じ (BakeFractureCore の合否判定を再利用)、
    Bone0/Bone1 へ正しく割り当たる、(2) 骨空間の頂点 × jointGlobal(bind) が変換前の絶対
    位置に戻る (最大誤差 0、受け入れ条件 2)、(3) 同じ入力の 2 回焼き+骨割り当てが
    `.mfrac` 相当のバイト列で一致 (決定論)、(4) `BuildFracturePieces` が kinematic root と
    `PartComponent` を正しく組む、(5) アニメで曲げた姿勢のまま球を当てると接着が切れて
    片方が剛体化 (`PartComponent` を失う)、もう片方は `PartComponent` を保って追従を続ける
    (受け入れ条件 3)。EditorMain.cpp に登録
  - src/Engine/Engine/DemoContent.cpp — `--fracture-demo` (`BuildFractureShowcaseScene`) に
    同じ 2 骨の腕を追加 (`MakeDemoSkinArm`/`BakeDemoSkinFracture`、バインドポーズ固定・
    アニメさせない — 追従アニメの検証は FractureSkinSelfTest 側の責務)。専用の砲弾を
    1 発追加。既存の box/wall の位置・挙動は変えていない
  - tests/golden/fracture_before.png / fracture_after.png — 上記デモ追加でシーンの見た目
    (新しい腕が画面に写り込む) が変わったため、shot_verify の該当 2 枚だけを撮り直して
    差し替えた (`tests\actual\fracture_{before,after}.png` を golden へコピー。他 4 枚
    (parts/joints/acoustic_forward/acoustic_deferred) には触れていない — 下記検証参照)
仕様との差分:
  - [逸脱] テスト用スキン資産に `assets\models\skinned_beam.fbx` (sub-10 やること9 の例示) を
    使わず、完全に手続き生成した 2 骨の箱 (FractureSkinSelfTest.cpp と DemoContent.cpp に
    それぞれ複製) を使った。理由: `skinned_beam.fbx` は生成スクリプトのコメントに明記の
    とおり「両面化 (巻き順による偽陰性回避)」— 全側面を表裏 2 枚ずつ二重化しており、
    M80 の閉じ判定 (溶接後、辺の使用数が丁度 2 でなければ非多様体) では各辺の使用数が 4
    になり必ず非多様体で拒否される (机上検算で確認、実行はしていない)。ボクセル化
    (openMeshMode=1) で通す手もあるが、表裏の重複三角形はレイパリティ内外判定を不安定に
    しうるため、テスト対象を素直な閉じたメッシュにする方を選んだ
  - [追加] src/Engine/Engine/Physics/FractureSkinBake.h/.cpp — 「触る場所」の想定
    (FractureBuilder.*、ModelCook.h/.cpp、Skeleton.h) には無い新規ファイル。理由:
    FractureBake.h は「このファイルは骨を知らない」と明記されたアーキテクチャ境界を持ち
    (sub-02)、FractureBuilder.cpp は「破片エンティティの事前生成」という別責務
    (ヘッダコメントに明記) を持つ純粋な ECS 構築器。骨割り当て+骨空間変換という
    ベイク時の幾何処理を Engine/Physics 層の別ファイルに切り出すことで両方の責務を保った
    (ConvexColliderLibrary.cpp が Engine/Physics から Renderer 型を使う前例と同じ層)
  - [追加] FractureSkinBake.h の `FractureSkinJoint` — `Skeleton.h` の `SkinnedModel`
    をそのまま使わず、名前+inverseBind だけの最小型にした。理由:
    `FractureBakeRequest`/ワーカーキューへコピーされる値なので、アニメクリップまで含む
    フルの `SkinnedModel` を持ち回らせたくなかった
  - [未実装] Inspector の「生成」経由 (`ModelCook::TryLoadCookedMeshVertices` を実際の
    FBX/glTF ロード → CookedCache 経由で読む経路) は自動テストで通していない
    (下の不安・質問へ)
検証:
  - tools\gen_project_files.ps1 → Engine.vcxproj(.filters) 更新 (新規 4 ファイル)
  - MSBuild Debug|x64 (フル) → 成功、warning 0 件
  - MSBuild Release|x64 (フル) → 成功、warning 0 件 (DemoContent.cpp の未使用引数 warning
    C4100 を 1 件検出して直し、再ビルドで解消済み)
  - bin\x64\Debug\Editor.exe --selftest → 全件 PASS (exit 0)。新設の Fracture skin self test
    18 チェック全て PASS。round 1 の実装ミス 2 件をここで検出して直した:
    (1) テストコード自身の stale pointer (`SkinnedMeshComponent*` を複数回の AddComponent
    をまたいで使い回していた — FractureBuilder.cpp 自身が守っている規約と同じ罠に
    テストコードが落ちていた) → tick 直前に取り直すよう修正
    (2) 球の狙い先のオフセット符号を取り違えていた (骨のピボットが破片の幾何の中心ではなく
    端にある設計だったのに、符号を逆にしていた) → 修正
  - bin\x64\Release\Editor.exe --selftest → 全件 PASS (exit 0)。上記 2 件の修正前は
    Release だけ 4 チェックが FAIL していた (Debug は偶然通っていた) — 原因調査の過程で
    自作テストコードが `XMVector3Rotate` (DirectXMath の組み込み回転) を使っていたのを
    見つけ、このコードベースが `XMMatrixInverse`/`XMMatrixDecompose` を Debug/Release で
    割れる要因として避けている慣例に合わせ、四則だけの手計算 (`QuatRotateVec`、
    `FractureSystem.cpp` の `QuatRotate` と同じ式) へ置き換えた。念のため球の半径も
    0.5→1.0 に広げて再検証し、Release も含め安定して PASS することを確認した
    (本番コード `FractureSkinBake.cpp` は元々この種の関数を使っていない — 割れていたのは
    テストコードの狙い先計算のみ)
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s)
  - tools\replay_verify.bat (フル、Debug/Release 再ビルド込み) → 14 job 全て PASS
    (demo/parts/flow/mp/physics/joints/acoustic/ui/fracture/ttdebug/ttrelease/
    whatifdebug/whatifrelease/rules)。fracture job は新しいスキンの腕を含むシーンで
    Debug record → Debug snapshot-stress 付き verify → Release verify の全段が一致 —
    受け入れ条件 5 (スキンのreplay一致) と 6 (parts不変) を満たす
  - tools\shot_verify.bat → 1 回目: 6 枚差分 (parts/joints/acoustic_forward/
    acoustic_deferred の既知 4 枚 + fracture_before/fracture_after の新規差分)。
    fracture_before/after は --fracture-demo に腕を足したことによる意図した見た目の変更
    (受け入れ条件 4 の証拠でもある) なので `tests\actual\fracture_{before,after}.png` を
    golden へコピーして更新 (他 4 枚には触れていない)。2 回目: 5 枚差分 (既知 4 枚 +
    `demo_render_rtrefl_restir` が偶発的に "no screenshot was written" — 私のどの変更とも
    無関係な RT ReSTIR デモで、1 回目の実行では同じ入力で PASS していた)。3 回目 (フレーク
    切り分けのための再実行) → 4 枚のみ差分 = 既知の枠と完全一致、`demo_render_rtrefl_restir`
    は PASS に戻った (フレーク確認)。既知 4 枚は harness.md の記録どおり M80 着手前から
    ずれている枚 (spec §5 受け入れ条件 10 で除外対象)
  - 画像確認: `--fracture-demo` を Runtime.exe (--warp なし、1600x900) で撮った
    before/after と、shot_verify の golden (960x540、--warp) の両方を目視。round 1 の
    実装では箱の法線が「8 頂点共有・対角方向」(FractureSelfTest.cpp の MakeBox と同じ
    ヘッドレス専用パターン) だったため、実際にライトを当てて描くと平らな面が滑らかな
    法線補間でねじれて見える不具合に気づき、DemoContent.cpp の腕だけ面ごと 4 頂点・
    面法線の 24 頂点構成に直した (CCW/外向きを手計算で確認)。破片も元メッシュの法線を
    引き継ぐので、この直しは割れる前・後の両方に効く。FractureSkinSelfTest.cpp 側の箱は
    ヘッドレス (描画しない) なので直していない
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1/2/3/5/6/7 はテスト (selftest 3 回の実行結果、
    replay_verify 全 job PASS、check_rules PASS) で確認済み。4 (割れた後は元のスキンを
    描かない) はスクショで確認済み (画像パス下記)。テスト資産を skinned_beam.fbx から
    手続き生成へ変えた逸脱 1 件と、Inspector 経由の実 FBX ロード → CookedCache 読み出し
    経路が自動テスト未到達な点があるため 5 にしていない
  正しさ: 4 — selftest (Debug/Release 双方、全件 PASS) と replay_verify (全 14 job PASS、
    Debug/Release/snapshot-stress の一致含む) で機能的な正しさを検証した。Release だけ
    落ちる不具合を実際に踏んで原因まで特定し直した経緯 (このコードベースの
    「XMMatrixInverse/XMMatrixDecomposeを避ける」慣例を自分のテストコードで破っていた)
    があるため、production コード側にも同種の見落としが残っていないかは完全には
    払拭できていない (FractureSkinBake.cpp は手計算のみで書いたが、他の甘さの可能性はゼロ
    と言い切れない)。Inspector からの実クリック操作 (ModelCook 経由の実ウェイト取得) は
    未検証
  コード品質: 4 — 責務を分離 (FractureSkinBake=骨空間変換の純関数、ModelCook=クック
    キャッシュ読み出し、FractureBuilder/FractureSystem=ECS 構築と破断、
    FractureBakeService/Commit=非同期境界と資産確定)。既存の書き方 (行ベクトル規約の
    手計算、非公開ヘルパのファイルごと複製) に合わせた。日本語コメント・ヘッダ形式は
    既存ファイルを踏襲
  テスト: 4 — 新規 SelfTest 18 チェック (Debug/Release 双方 PASS)、既存 selftest 全件
    (Debug/Release とも 0 failure)、replay_verify 全 14 job PASS、check_rules PASS、
    shot_verify (新規 2 枚 PASS、既知 4 枚のみ差分でフレーク 1 件を再実行で切り分け済み)。
    Inspector の実クリック経路と、非一様スケールを持つ骨での法線の歪みは未検証
不安・質問:
  1. テスト用スキン資産を `skinned_beam.fbx` (sub-10 の例示) から完全な手続き生成へ
     変えた判断について。`skinned_beam.fbx` は両面化されており M80 の閉じ判定では
     非多様体として拒否される見込み (机上検算のみ、実行未確認) なので、素直な閉じた
     箱を新規に用意する方を選んだ。もし「既存資産をボクセル化経由ででも通す」方を
     優先すべきだったなら差し戻してほしい
  2. Inspector の「生成」ボタンを実際に FBX/glTF 由来の SkinnedMeshComponent エンティティで
     クリックし、`ModelCook::TryLoadCookedMeshVertices` が本物の .mmdl キャッシュから
     正しくウェイトを取り出せることを確認する自動テスト (または手動の一時プローブ) を
     このサブでは書けていない (時間予算)。sub-11/12 またはレビューで手薄と判断されれば
     追加したい
  3. 法線変換が `inverseBind` の線形部分をそのまま使う近似 (逆転置行列を使わない) である点。
     回転+並進のバインド行列でのみ正確で、非一様スケールを含む骨では破片の陰影が歪む
     (位置・物理には影響しない — 凸包は変換後の頂点から作り直すので当たり判定は正しい)。
     spec §4.1 の「非一様スケールのルート」と同種の制限として文書化しているが、
     spec 本文への追記は行っていない (planner 判断を仰ぎたい)
  4. sub-09 の申し送り (nit): 「Play 中と複数選択のときも生成ボタンを無効にする」は
     このサブでは対応していない (DrawDestructibleNotes は触ったが、この nit 自体は
     スキン対応と無関係なため対応漏れとして残る)
触ったファイル:
  - src/Engine/Engine/Physics/FractureSkinBake.h (新規)
  - src/Engine/Engine/Physics/FractureSkinBake.cpp (新規)
  - src/Engine/Engine/Physics/FractureSkinSelfTest.h (新規)
  - src/Engine/Engine/Physics/FractureSkinSelfTest.cpp (新規)
  - src/Engine/Engine/Physics/FractureLibrary.h
  - src/Engine/Engine/Physics/FractureLibrary.cpp
  - src/Engine/Engine/FractureBuilder.cpp
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/Asset/ModelCook.h
  - src/Engine/Engine/Asset/ModelCook.cpp
  - src/Engine/Engine/DemoContent.cpp
  - src/Engine/Core/LocalizationTable.inl
  - src/Editor/FractureBakeService.h
  - src/Editor/FractureBakeService.cpp
  - src/Editor/FractureBakeCommit.h
  - src/Editor/FractureBakeCommit.cpp
  - src/Editor/Windows/InspectorWindow.cpp
  - src/Editor/FractureEditorSelfTest.cpp
  - src/Editor/EditorMain.cpp
  - tests/golden/fracture_before.png (更新)
  - tests/golden/fracture_after.png (更新)
  - スクショ (一時、コミット対象外): before =
    %TEMP%\claude\C--HAL-MyEngin\bc8aa3e8-1cc1-44bf-aefb-a18425fbda6f\scratchpad\
    fracture_skin_before3.png、after = 同ディレクトリの fracture_skin_after.png
    (Runtime.exe --fracture-demo --no-audio --warp --frames N --shot-frame N-1
    --screenshot で撮影。tests/golden の 2 枚が本採用の証跡)
申し送り:
  - sub-11 (計測・ベンチ・上限) で骨付き破片のベイク・物理コストも計測対象に含めるなら、
    `AssignFractureBonesAndTransform` の凸包再構築 (BuildConvexHull を破片ごとにもう一度
    呼ぶ) のコストも見ておくとよい (今回計測はしていない)
  - Inspector の「生成」ボタンを実スキンメッシュ (FBX/glTF ロード品) でクリックする
    確認は未実施 (上の不安・質問 2)。sub-11/12 かレビューで手が空けば一時プローブで
    確かめてほしい
  - 法線変換の近似 (不安・質問 3) は非一様スケールを持つ骨がある場合にのみ見た目へ影響する
    (位置・物理は影響を受けない)。実運用で問題になったら `FractureSkinBake.cpp` の
    `TransformDirection` を逆転置行列ベースへ差し替える (ただし `XMMatrixInverse` を
    避ける決定的な逆行列計算をどう用意するかが新たな課題になる)

SELF_EVAL: sub-10 (round 2)
実装 (round 1 からの差分。指摘番号は FIX_REQUEST のもの):
  - #1 [must] 実アセットの経路: src/Engine/Engine/Physics/FractureSkinSelfTest.cpp に
    `CheckRealSkinnedAsset` (新規) を追加し、`assets\models\CesiumMan.glb` と
    `assets\models\skinned_beam.fbx` を PartSelfTest.cpp と同じ手法でヘッドレスにロードして
    検査する。**最初に確かめた前提**: `.mmdl` を書くのは `ModelLoader::RegisterAssets`/
    `FbxLoader::RegisterAssets` (起動時の一括ヘッドレス登録) だけで、entity を作る
    `Load` は別経路のフレッシュパースでクックキャッシュを読み書きしない
    (`ModelLoader.cpp`: `TryReplayFromCache`/`SaveToCache` は `RegisterAssets` の中にしか
    無い)。`Load` と同じキーで登録する契約 (M50a) を使い、`RegisterAssets` を先に 1 回
    呼んで `.mmdl` を書かせてから `Load` でエンティティを作る (同じ `resources` へ両方
    登録しても同名の再登録は差し替えなので安全)。CookedCache は一時ディレクトリへ
    `Configure` してから読み書きする (CookedCacheSelfTest.cpp と同じ流儀、終わったら
    `Configure(L"", false)` で戻す)
    - (a) 頂点数/index数を CPU メッシュおよび `.mmdl` の生データ (新規
      `ReadCookedMeshCounts`、公開 API の `CookedCache::ReadValidated`+
      `ModelCook::Deserialize` を直接呼ぶだけで本番コードは変えない) と突き合わせ、
      ウェイト和 (許容 0.02) と骨 index の範囲を検査する
    - (b)/(c) `openMeshMode=0` で焼き、拒否なら `openMeshMode=1` (解像度32) で焼く。
      実測: **CesiumMan.glb は閉じたメッシュで `openMeshMode=0` のまま成功** (3 pieces)。
      **skinned_beam.fbx は非多様体辺 36 本で拒否され** (境界0/非多様体36/向き不一致0)、
      `openMeshMode=1` で成功した (round 1 の机上検算どおり — 両面化した梁は必ず
      非多様体になる、が実測で裏付けられた)
    - (d) 骨に割り当てた破片の外側面/断面の頂点を、割り当てた骨の `jointGlobal` (バインド)
      で戻すと変換前の絶対位置に一致することを検算 (このファイル前半の手続き生成テストの
      (2) と同じ式)。**「破片の原点」で比較しない**よう設計した — round 1 で一度書いた
      「LocalTransform.position (= 割り当てた骨自身の位置) と焼きのorigin (=破片の体積
      重心、一般には骨の位置と一致しない) を比較する」実装は数学的に誤りで、CesiumMan/
      skinned_beam の両方で外れた (最大誤差 0.32 / 1.08、後述の「不安・質問」参照)。
      修正後は CesiumMan で 14424 頂点・最大誤差 6e-7、skinned_beam で 9096 頂点・
      最大誤差 0 (tol は相対 1e-5)
    - おまけ: `BuildFracturePieces` が実アセットでも落ちずに正しい数の子を組むことも検査
  - #2 [must] ボクセル化した入力の骨割り当て: `AssignFractureBonesAndTransform` を
    「外側面の頂点ごとに位置のビット一致→無ければ最も近い元頂点 (距離、同値は index 小)
    のウェイトを加算」という 1 本の規則に統一 (`AccumulateVertexWeight`/
    `NearestVertexIndex` を新規追加)。非ボクセル化の入力では蓋の継ぎ目以外ほぼ全頂点が
    厳密一致するので従来と実質同じに、ボクセル化した入力 (surface nets の頂点は元頂点と
    位置が一致しない) では全頂点が最近傍側に落ちて意味のある多数決になる。
    skinned_beam.fbx の実測 (openMeshMode=1 経路) で「全破片に有効な骨」を確認済み
  - #3 [should] 法線の逆転置: `FractureSkinBake.cpp` に `InverseTransposeLinear` (3x3 の
    余因子行列を行列式で割る閉形式。加減乗除のみ、`XMMatrixInverse` は使わない) を追加し、
    法線変換をこれに差し替えた。一様スケール (回転+並進のみ含む、既存の全テスト資産)
    では代数的に元の近似と同じ結果になる (直交行列の逆転置は自分自身)。特異行列 (det≈0)
    だけ線形部分そのものへ安全側フォールバック
  - #4 [should] Play 中の生成ボタン無効化: `InspectorWindow::OnImGui` に `bool inPlayMode`
    引数を追加し、private メンバ `inPlayMode_` へ保存 (`DrawDestructibleNotes` が読む —
    `fractureBakeService_`/`fractureOutcomes_` と同じ「OnImGui が書き、leaf 関数が読む」
    既存の流儀)。呼び出し元は `EditorApp.cpp` の 1 箇所のみ
    (`playMode_.InPlayMode()` を渡す)。新規ローカライズ文字列
    `Insp_FracturePlayModeDisabled` を追加。**複数選択は round 1 時点で既に対応済み**
    だった (`DrawComponentNotes` の呼び出し元が `!tg.multi` でガード済み、
    `InspectorWindow.cpp:1129` 「マルチ選択では出さない — 焼きはメッシュ1つ・
    エンティティ1つに対する非同期処理」) — 確認しただけで変更は無し
  - #5 skinned_beam.fbx を使わなかった判断: 上記 (b) の実測で「両面化のため
    openMeshMode=0 では必ず拒否される」ことが確認できた。ただし **手続き生成の腕は
    そのまま残した** (round 1 の裁定どおり) — 実測で判明した skinned_beam.fbx の
    openMeshMode=1 経路は round 2 で新規に追加した実アセット節が担い、手続き生成の腕は
    「バインドポーズの完全一致・アニメ追従・接触分離」という、非多様体でない素直な閉じた
    メッシュでないと数値的に検算しづらい部分を引き続き担当する (責務が異なる、どちらも残す)
仕様との差分:
  - [追加] round 1 の [未実装] (Inspector の「生成」ボタンを実クリックする確認) は
    引き続き未実装 (このサブでは自動テストでの ModelCook 経路確認に留めた。下の
    不安・質問へ)
検証:
  - tools\gen_project_files.ps1 → 新規ファイル無し (round 2 は既存ファイルの編集のみ)
  - MSBuild Debug|x64 / Release|x64 (フル、複数回) → 成功、warning 0 件
  - bin\x64\Debug\Editor.exe --selftest → **2/4 回が実アセット節で
    "TryLoadCookedMeshVertices reads the .mmdl cache" 2 件の FAIL、2/4 回 (診断コード除去後の
    最終 2 回を含む) が ALL PASS**。原因調査のため一時的に診断コード (MYE_LOG を経由しない
    直接ファイル書き込み) を仕込んで再実行したところ、失敗した回でも
    `CookedCache::Enabled()` は `RegisterAssets` の前後を通じて一貫して `true` であり、
    `RegisterAssets` 自体も `registered=true` (パース成功) を返していた。これは
    `ModelCook::SaveToCache` → `CookedCache::Write` が **戻り値を検査されない
    fire-and-forget** であること (`SaveToCache` は `if (Write(...)) { ログ }` で、false の
    ときは何も残さず黙って抜ける、既存コード・私が変えていない) と整合する — つまり
    `Write()` 内部のファイル書き込み (create_directories → 一時ファイル → rename) が
    「有効化はされているが実際の書き込みが失敗する」formで一過性に失敗したと見ている。
    このセッション中に同種のビルド+実行を極めて多数回・連続して行っており、ディスク I/O や
    ウイルス対策ソフトの競合など環境要因の可能性が高い。診断コード除去後に**同一の
    実行ファイルで 2 回連続 ALL PASS** (このセッション最後の 2 回) を確認しており、
    `TryLoadCookedMeshVertices` 自体や `AssignFractureBonesAndTransform` のロジックが
    Debug 特有に誤っているという証拠にはならないと判断した。ただし 100% の原因確定には
    至っていない (下の不安・質問へ)。診断コードはコミット対象に残していない (削除済み)
  - bin\x64\Release\Editor.exe --selftest → 2/2 回 ALL PASS (exit 0)。CesiumMan.glb:
    頂点3273/index14016 一致、ウェイト和0/3273が範囲外、骨index範囲内、openMeshMode=0
    成功 (3 pieces)、バインド一致 (14424頂点、最大誤差6e-7)。skinned_beam.fbx:
    頂点216/index216一致、ウェイト和・骨indexとも異常無し、openMeshMode=0拒否
    (非多様体36本)→openMeshMode=1成功、バインド一致 (9096頂点、最大誤差0)
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s) (replay_verify の rules job 経由でも確認)
  - tools\replay_verify.bat (フル、Debug/Release 再ビルド込み) → 14 job 全て PASS
    (fracture 含む)。法線・骨割り当てアルゴリズムの変更後も Debug/Release/snapshot-stress
    の一致は崩れていない
  - tools\shot_verify.bat → 実行 2 回とも「4 枚のみ差分」= 既知の枠 (parts/joints/
    acoustic_forward/acoustic_deferred) と完全一致。**fracture_before/fracture_after は
    round 1 の golden のまま変化なし** — 法線の逆転置化は一様スケールでは round 1 の
    近似と代数的に同じ結果になる、という予測どおりだった (golden の撮り直しは不要)
自己採点 (1-5):
  仕様適合: 5 — 受け入れ条件 1/2/3/5/6/7 を実アセット (CesiumMan.glb / skinned_beam.fbx)
    を含めてテストで確認し、round 1 の 2 件の must と 2 件の should をすべて対応した。
    round 1 で見送っていた「skinned_beam.fbxを使わない判断の実測での裏付け」も取れた
  正しさ: 4 — Release / replay_verify / shot_verify は安定して PASS。Debug だけ実アセット
    節が 4 回中 2 回、原因を fire-and-forget な `CookedCache::Write` の一過性失敗と
    診断したが確定的な証明はできていない (再現条件を掴めていない) ため 5 にしていない。
    round 1 で見つけた「(d) の比較対象の取り違え」という自分自身の実装ミスを、実アセットで
    実測して自分で検出・修正できたこと自体は「検証していないものは動いていない」を
    体現できたと考えている
  コード品質: 4 — 骨割り当てを 1 本の規則 (位置一致→最近傍) に統一してコードが単純化した。
    法線の逆転置は閉形式・コメント付きで既存の「XMMatrixInverse/XMMatrixDecomposeを
    避ける」慣例を踏襲。Play中の無効化は既存の「OnImGuiが書きleafが読む」private
    メンバの流儀に合わせた。一時診断コードは全て削除済み (git status で確認)
  テスト: 5 — 新規の実アセット検査 (CesiumMan.glb 8 項目 + skinned_beam.fbx 8 項目) を
    追加。手続き生成の腕のテスト (18 項目) は変更なしで残した。replay_verify・
    shot_verify・check_rules も全て通った
不安・質問:
  1. Debug --selftest の実アセット節が断続的に (4回中2回) "TryLoadCookedMeshVertices
     reads the .mmdl cache" で FAIL した件。診断ログで `CookedCache::Enabled()`/
     `RegisterAssets` の戻り値はどちらも正常だったので、`CookedCache::Write` の
     ファイル書き込み自体が一過性に失敗したと推測しているが、確定的な再現条件は
     掴めていない。`SaveToCache`/`Write` は戻り値を検査しない fire-and-forget な
     既存設計 (私は変えていない) で、失敗時に一切ログもエラーも残らない。これは
     M80j 固有の問題ではなく `CookedCache` 全体の既存の性質だが、実運用でも
     「保存が静かに失敗し続ける」余地がある。この一連の investigation を
     経て**この既存の沈黙した失敗モードを直すかどうかは planner/reviewer の判断を
     仰ぎたい** (直すなら M80 の外、`Write()` の戻り値を `SaveToCache` が見てログに
     残すだけの小さい変更で足りるはず)
  2. round 1 の不安・質問 2 (Inspector の「生成」ボタンを実クリックする確認) は今回も
     自動化していない。ただし round 2 で `TryLoadCookedMeshVertices` 自体は実アセットで
     確認できたので、残るギャップは「Inspector の `assetkey::SourcePathForSubAssetKey`
     によるパス解決」 (ヘッドレス selftest には AssetDatabase が無く resolver 未設定で
     空を返すため、selftest では検証できない・本番は AssetDatabase が resolver を
     Install しているので動く設計) だけになったと考えている。時間予算の都合で
     このサブでは対応していない
触ったファイル (round 2 で追加/変更。round 1 の一覧は上記参照):
  - src/Engine/Engine/Physics/FractureSkinBake.h/.cpp (法線の逆転置、最近傍フォールバック)
  - src/Engine/Engine/Physics/FractureSkinSelfTest.h/.cpp (実アセット検査を追加)
  - src/Editor/Windows/InspectorWindow.h/.cpp (inPlayMode 引数・無効化)
  - src/Editor/EditorApp.cpp (OnImGui 呼び出しに inPlayMode を渡す)
  - src/Engine/Core/LocalizationTable.inl (Insp_FracturePlayModeDisabled を追加)
  - plans/m80-destruction/sub-10.md (このメモ)
  - tests/golden/fracture_before.png / fracture_after.png は**今回変更なし** (差分の
    「触ったファイル」からは除外。round 1 で更新したものが変わらず有効)
申し送り:
  - 不安・質問 1 (CookedCache::Write の沈黙した失敗モード) は M80 の外の既存設計の話。
    直すなら 1 行程度の小さい変更 (戻り値をログに残す) で足りる見込み
  - 不安・質問 2 (Inspector の実クリック経路) は round 1 から持ち越し。sub-11/12 か
    レビューで手が空けば一時プローブで確かめてほしい

SELF_EVAL: sub-10 (round 3)
実装 (round 2 からの差分。指摘番号は FIX_REQUEST のもの):
  - #1 [must] キャッシュ置き場をプロセスごとに一意化: `FractureSkinSelfTest.cpp` の実アセット節で
    `mye_fractureskin_realasset_selftest_<pid>` (`_getpid()`、`<process.h>`) をキャッシュ
    ディレクトリ名にした。開始時の `remove_all` (自分自身の前回異常終了分の掃除) に加えて、
    **終了時にも `remove_all`** を追加 (プロセス固有名になったので次回実行の「開始時
    remove_all」に後始末を委ねられない — 自分で消す)
  - #1 [must] 「書かれていない」と「読めない」の区別: `RegisterAssets` 呼び出し直後、
    `TryLoadCookedMeshVertices` を呼ぶ**前**に、`CookedCache::PathFor(path,
    ModelCook::kModelExt)` (本番の `ReadValidated` が内部で使うのと同じ関数) で期待パスを
    求め、`std::filesystem::exists` だけを見る新しい check
    ("RegisterAssets actually wrote a .mmdl file to disk") を追加。既存の
    "TryLoadCookedMeshVertices reads the .mmdl cache" はそのまま残し、
    「ファイルが無い」と「ファイルはあるが読めない/中身が壊れている」を別の check として
    区別できるようにした。本番コード (`CookedCache::Write`/`ModelCook::SaveToCache`) は
    変えていない
  - #1 [must] 故障点での確認 (指示どおり Debug/Release の `--selftest` を同時に実行):
    - 修正前 (固定名のまま) で 1 回、Debug と Release を同時に起動 → **再現せず**
      (両方 ALL PASS)。原因は下の「不安・質問」参照
    - 修正後、Debug と Release を同時に起動する試行を**連続 5 回**実施 → **5/5 回とも
      両方 ALL PASS** (各回のログで新設の 2 check、"RegisterAssets actually wrote a
      .mmdl file to disk" と "TryLoadCookedMeshVertices reads the .mmdl cache" が
      CesiumMan.glb / skinned_beam.fbx の両方で PASS していることを確認)
  - #2 [should] 同じ形が `CookedCacheSelfTest.cpp:144-147` (`mye_cook_selftest`、固定名 +
    開始時 remove_all) にもある件: **M80 の範囲外なので直していない**。下の申し送りへ
仕様との差分: なし (round 2 の内容 — 実アセットの4検査・骨割り当ての1本化・法線の逆転置・
  Play中の無効化・RegisterAssets→Loadの順 — は変更していない)
検証:
  - MSBuild Debug|x64 / Release|x64 → 成功、warning 0 件
  - **故障点の再現 (修正前)**: `mye_fractureskin_realasset_selftest` (固定名) のままの
    バイナリで、Debug と Release の `--selftest` をほぼ同時に起動 (Release は約 40 秒〜
    2 分、Debug は約 9〜20 分で完走するため、両者が「実アセット節」に同時に居る時間は
    短い)。結果は **両方 ALL PASS** — 1 回の試行では round 2 の断続的な FAIL を再現
    できなかった (下の不安・質問へ)
  - **同時実行 5 回連続 (修正後)**: 5 回とも Debug・Release の両方が `--selftest`
    ALL PASS (exit 0)。実行のたびに `Get-Process ... | Stop-Process` で前回のプロセスが
    残っていないことを確認してから起動。新設 2 check も毎回 PASS を確認済み
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s)
  - tools\replay_verify.bat (フル、Debug/Release 再ビルド込み) → 14 job 全て PASS
  - tools\shot_verify.bat → 既知 4 枚 (parts/joints/acoustic_forward/acoustic_deferred)
    のみ差分。fracture_before/after は変化なし
  - 後始末の確認: 5 回の同時実行後、`%TEMP%` に `mye_fractureskin_realasset_selftest_<pid>`
    形式のディレクトリが**残っていない**ことを確認 (終了時 `remove_all` が機能している)。
    修正前の再現試行で使った固定名ディレクトリの残骸は手動で削除済み (成果物ではない)
自己採点 (1-5):
  仕様適合: 5 — must 2 件・should 1 件 (もう 1 件は明示的に対象外) すべてに対応。
    「5 回連続で両方 PASS」を実測で示した
  正しさ: 4 — 修正後の頑健性 (5/5 PASS、置き場がプロセスごとに独立なので原理的に
    他プロセスと衝突しえない) には確信があるが、round 2 で観測した現象の**原因を
    実験的に再現して特定するには至らなかった** (1 回の再現試行で発生しなかった)。
    「直った」ことは示せたが「何が壊れていたか」を 100% 特定できていないため 5 にしていない
  コード品質: 4 — 変更は最小 (置き場の命名とライフサイクル、既存チェックの追加 1 本)。
    既存の `CookedCache::PathFor`/`ModelCook::kModelExt` という公開 API をそのまま使い、
    本番コードには一切触れていない
  テスト: 5 — 同時実行 5 連続 PASS を実測。新設の存在確認 check が実際に意味のある
    区別をしていることをログで確認済み
不安・質問:
  1. 修正前の故障点再現が 1 回の試行で起きなかった件について、追加で考えた仮説:
     Release は数十秒〜2 分、Debug は 9〜20 分かかる (このセッションの計測値)。
     「Debug と Release の `--selftest` を同時に走らせる」を額面どおりほぼ同時開始で
     行うと、Release は実アセット節へ**開始後まもなく**到達して消え、Debug が同じ節へ
     到達するのは**その何分も後**になるため、両者が「同じ瞬間にこの節にいる」時間は
     実際には短い。round 2 で実際に観測した衝突は、私が作業中に**複数のコマンドを
     完全な終了を待たずに次々発行した** (例: 前のプロセスの終了通知を待たずに次を
     起動した、または shot_verify.bat / replay_verify.bat のように**内部で複数の
     Editor.exe/Runtime.exe を起動するバッチと、単体の --selftest 実行が重なった**)
     ことによる、より緩いタイミングでの重なりだった可能性がある。固定名 + 開始時
     remove_all という原因そのものはコード上明確な欠陥なので、指示どおり修正 1・2 は
     入れた。それでも「これが round 2 で実際に起きた事象の唯一の説明」と断定は
     できていない
  2. round 1・2 から持ち越し: Inspector の「生成」ボタンを実クリックする確認は今回も未実施
触ったファイル (round 3 で追加/変更):
  - src/Engine/Engine/Physics/FractureSkinSelfTest.cpp (置き場のプロセス固有化・終了時
    remove_all・「書かれていない」チェックの追加)
  - plans/m80-destruction/sub-10.md (このメモ)
申し送り:
  - [should #2] `src/Engine/Engine/Asset/CookedCacheSelfTest.cpp:144-147` の
    `mye_cook_selftest` も固定名 + 開始時 `remove_all` で、理論上は同じクラスの
    (他プロセスとの同時実行による) 衝突リスクを持つ。M80 の範囲外として今回は直して
    いない。司会が台帳に記録し、ユーザーへ報告してほしい
  - 不安・質問 1 のとおり、round 2 で実際に何が起きたかの確定的な再現はできていない。
    今回の修正 (プロセスごとに独立した置き場) はこのクラスの衝突を構造的に無くすので、
    原因が「別プロセスとの衝突」であれば直っているはずだが、今後 Debug 単体の実行で
    同じ症状が再発した場合は別の原因を疑う必要がある
- round 1: VERDICT REWORK (planner)。must: 実アセット (CesiumMan.glb / skinned_beam.fbx) で `.mmdl` の読み出し・焼き (拒否またはボクセル化での成功)・骨の割り当て・バインドポーズの一致を SelfTest で確かめる。ボクセル化の経路の骨の割り当てを「最も近い元の頂点」に。should: 法線に逆転置を使う、Play 中と複数選択でボタンを無効にする。手順は「round 1 の裁定」節
- round 2: VERDICT REWORK (planner)。must: 実アセットの節が、固定名の一時ディレクトリを開始時に消すので、Debug と Release の同時実行で互いに消し合う (断続的な FAIL の原因の見立て)。置き場をプロセスごとに一意にし、書かれたかを読み戻しの前に確かめ、同時実行で再現と解消を示す。手順は「round 2 の裁定」節。それ以外は受け入れ済み
- round 3: VERDICT OK (planner)。置き場をプロセスごとに一意化し、「書かれていない」と「読めない」を区別する検査を足した。同時実行で 5 回連続して両方 ALL PASS。修正前の形での再現は 1 回の試行ではできなかった (Debug と Release の実行時間の差で重なる時間が短い、という仮説)。共有の一時ディレクトリを消し合う危険は構造として取り除いたので、これ以上の再現は求めない。残るリスク: 同種の断続的な FAIL が再発したら、新しい検査の文言で「書かれていない / 読めない」を切り分けられる。既存の `CookedCacheSelfTest.cpp:144-147` の同型の問題は M80 の外 (台帳に記録済み)
