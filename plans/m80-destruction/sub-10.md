# sub-10: スキンメッシュの破壊 (骨追従から剛体化)

- 依存: sub-07, sub-09
- 状態: 未着手
- 往復: 0

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

1. スキンの梁 (または既存のスキン資産) を焼き、全破片が閉じ、破片がそれぞれ骨に割り当てられる (割り当ての結果をテストで固定) — `--selftest`
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

## 実装メモ (coder が追記)

## フィードバック履歴
