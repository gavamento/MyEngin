# MyEngine FBX・当たり判定一体プレハブ

`ResearchWing_Stage01_Collision.prefab.json` は、天井付き `ResearchWing_Stage01.fbx` の描画階層と当たり判定をまとめたプレハブです。互換性のためファイル名には `_Collision` を残しています。子の `ResearchWing_Stage01_Visual` に144個のMeshRenderer（74,044三角形）があり、FBXのメッシュ・描画材質・テクスチャを参照します。FBXとtexturesフォルダーは引き続き必要です。

床61・天井10・壁60・家具18の計149個の静的ボックスを含みます。家具は外接ボックスで、棚の内部や机の下には入れません。床材は床本体を分割して割り当て、表面や水面に追加の段差は作りません。配置マーカーと光に障害物はありません。

全149個のColliderに `physMaterial` を設定しています。通常床・壁・天井は `assets/physmats/tile.physmat.json`、家具は `metal`、特殊床は配置マニフェストに対応する `glass` / `water` / `carpet` / `metal` / `rubber` です。範囲が重なる場合は表示用FBXと同じくマニフェストの後の項目が優先され、金属床上のゴムを踏めます。通常タイルの音響値は暫定値（音量0.4、半径12m、音色0）です。

参照IDは各物理材質の `.meta` のGUIDを使用します。材質ファイルと `.meta` はセットで保持してください。再生成時も同じ割り当てを生成し、床面積・重複なし・床材の一致・床上面Y=0を検証します。既にシーンへ配置したプレハブのコピーは、この更新済みプレハブへ差し替えてください。FBXだけの再読み込みでは物理材質は反映されません。

MyEngineでこのプレハブを位置 `(0,0,0)`、回転なし、スケール `(1,1,1)` に配置してください。これ一つで天井付きFBXと物理材質付き当たり判定が配置されます。同じ場所にFBXを別途配置すると描画が重複します。旧プレハブとFBXを別々に置いていた場合は、このプレハブへの置き換え時に重複しないようにしてください。

**別プロジェクトへのコピーには参照の再生成が必要です。** このフォルダーのプレハブは `C:\HAL\MyEngin` 用です。FBXの参照は絶対パス、物理材質の参照はプロジェクトごとの `.meta` GUIDに依存します。ファイルだけをコピーすると見た目と物理材質が未解決になります。

配置先用の生成例:

```text
"C:\Program Files\Blender Foundation\Blender 5.1\5.1\python\bin\python.exe" tools/stage01/prepare_project_prefab.py --project C:/HAL/Shadow_Sound --stage-folder model/research_wing_stage01 --output cache/stage01_project_fix
```

このコマンドは配置先のFBXと物理材質を読み、出力フォルダーだけに修正版と `verification.json` を生成します。生成されたプレハブを配置先の同名ファイルへ反映し、不足材質が出力された場合はその `.physmat.json` と `.meta` を配置先の `assets/physmats` へ一緒に追加します。既存材質の `.meta` をMyEngin側のものへ置き換えてはいけません。保存済みシーン内の古い配置は別データなので、修正版を新しく配置して確認してください。

プレイヤーは `../player_fp_v01/Player_Researcher_FP_Collision.prefab.json` を配置し、本体の位置を開始地点のカプセル中心 `(3,0.9,3)` に設定します。見た目のFBXは `Player_FP_VisualAnchor` の子にローカル位置ゼロ・回転なし・スケール1で配置してください。アンカーは本体からY=-0.9mで、モデルの足元を床に合わせます。本体はスケール1を維持し、移動処理はCharacterControllerのmoveInputを使用します。Rigidbodyを追加するとCharacterControllerが無効になります。

このプレハブは描画と当たり判定のデータです。既存の編集中シーンへの配置、移動スクリプト・カメラ・アニメーションの接続は行っていません。しゃがみで判定を縮める処理も含みません。

再生成: エンジンルートから `tools\stage01\build_prefab.cmd`。生成元は `tools/stage01/build_collision.py` と `tools/stage01/export_prefab_visual.cpp` です。FBX参照IDはFbxLoaderと同じ正規化絶対パスから生成するため、チェックアウト先の移動やFBXの再出力後は再生成してください。

FBXをufbxで実際に読み込み、描画階層と材質ごとのメッシュ参照を生成しています。74,044三角形の一致、親子参照、物理判定149個の維持、床面積と重複なし、扉10か所の前後・中央の計30点と開始地点で立ち姿勢カプセルが障害物に重ならないことを検証済みです。MyEngineでの表示・実際の移動操作は未検証です。
