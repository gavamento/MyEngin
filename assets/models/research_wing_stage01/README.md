# Research Wing / ステージ1 FBX

`C:\HAL\MyEngin\stage.md` の先頭の具体配置設計に基づく、静的なレベル配置モデルです。
床領域74×36m、中央実験室20×20m、LAB 01は12×10m。
既存研究室セットの実験台・棚・シンクを寸法に合わせて再配置しています。
壁・床・天井・柱等は新規作成した配置確認用の形状で、最終的な装飾済みステージではありません。

## 使うファイル

MyEngineで描画と物理材質付き当たり判定をまとめて配置する場合は、[ResearchWing_Stage01_Collision.prefab.json](ResearchWing_Stage01_Collision.prefab.json) を使用してください。天井付きFBXの描画階層を含みます。詳細と再生成方法は [COLLISION.md](COLLISION.md) を参照してください。

| ファイル | 内容 |
|---|---|
| [ResearchWing_Stage01.fbx](ResearchWing_Stage01.fbx) | 天井付きの建築、家具、床材。109メッシュ、74,044三角形 |
| [ResearchWing_Stage01_OpenTop.fbx](ResearchWing_Stage01_OpenTop.fbx) | 天井を除いた同じステージ。99メッシュ、73,924三角形 |
| [ResearchWing_Stage01_Markers.fbx](ResearchWing_Stage01_Markers.fbx) | 敵2体、開始地点、固定光、設置光候補、アイテムの目印12個と巡回点8個。720三角形 |
| [ResearchWing_Stage01.blend](ResearchWing_Stage01.blend) | 編集用。建築・天井・家具・床材・配置目印・プレビューをコレクションで分離 |
| [Stage01_Top.png](Stage01_Top.png) | 北が上の上面プレビュー。天井と扉上部の壁を非表示 |
| [Stage01_Perspective.png](Stage01_Perspective.png) | 天井を非表示にした斜め上方からの確認画像 |
| [placement_manifest.json](placement_manifest.json) | 部屋、扉、床材範囲、家具、敵、アイテム、巡回点の座標 |
| `textures/` | 既存家具から複製した相対参照テクスチャ。FBXと一緒に保持 |

通常は天井付きと天井なしのどちらか一方を読み込んでください。
敵・アイテムも配置確認する場合は、Markers.fbxを同じ原点に追加します。
赤は敵、黄はアイテム、緑は開始・光、水色はコアです。これらは円柱の目印で、敵キャラクターや拾得アイテムの完成モデルではありません。

## 座標・読み込み

- 単位m、配置位置 `(0,0,0)`、回転 `(0,0,0)`、スケール `(1,1,1)`。
- BlenderではX=東、Y=北、Z=高さ。MyEngineの左手系Y-upへの変換後はX=東、Y=高さ、Z=北。
- 壁厚0.2m。壁込み範囲はX=-0.1〜74.1、Z=-0.1〜36.1m。床下端Y=-0.2m、天井上端Y=4.66m。
- 開口9か所は幅2m・高さ2.4m。L字接続D0は壁芯幅4m、側壁間の実有効幅3.8m。
- 全メッシュ三角形化済み、UV0あり。基本床材は色・金属度・粗さによる区別。床材形状の上面同士は重複させていません。
- 棚・配管が壁芯に接する箇所は、壁厚との重複を避けるため0.1m内側へ収めています。
- `.blend` は天井をビューポートで非表示にしています。天井を見る際はCeilingsコレクションの表示を戻してください。
- カメラ、文字ラベル、確認照明はFBXに含みません。扉は開口として作成しており、扉板・開閉アニメーションは含みません。

## ゲーム側で設定するもの

当たり判定、Nav、敵AI、音波・足音、床材のゲーム内種別、光の安全範囲と誘引、拾得処理、クリア判定は未接続です。
Markersは配置を示すだけなので、ゲーム実装時は対応するエンティティに置き換えてください。
I01などの数字付きマーカーは一つの配置場所を表します。個数は名前とstage.mdを参照してください。
補給室のI04は石3個・瓶2個の配置場所、I05は光補充1個です。

## 検証・再生成

- Blenderへの3つのFBX再読み込みでメッシュ数、三角形数、UV、材質、画像参照、全体寸法、配置目印の座標を検査。
- 10接続に対して計84本のレイで、扉付近の幅・高さに障害物がないことを検査。キャラクターの実移動検証ではありません。
- リポジトリ内ufbxを使用し、MyEngineと同じ左手系Y-up・メートル変換で3ファイルを読み込み。座標、法線、UV、三角形、PBR数値の存在を検査。
- 詳細：[validation_blender.json](validation_blender.json)、[validation_ufbx.txt](validation_ufbx.txt)。
- MyEngine実行画面での表示、衝突、操作、音響、敵挙動は未検証です。

再生成はエンジンルートから `tools\stage01\build_stage.cmd`。
ufbx検証は `tools\stage01\verify_stage_fbx.cmd`。
Blender 5.1とVisual Studioのパスは各cmdに記載しています。
生成元は `tools/stage01/build_stage.py`。元の研究室アセットは変更しません。
