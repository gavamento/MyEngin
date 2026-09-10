# MyEngine 当たり判定

`Player_Researcher_FP_Collision.prefab.json` を追加しました。半径0.3m・全高1.8mのCharacterControllerと同寸法のソリッドなカプセルColliderを持ちます。重力あり、最大接地傾斜45度、スキン幅0.02mです。本体の原点はカプセル中心で、足元基準の見た目用子アンカーをY=-0.9mに設けています。

配置方法と検証範囲は [ステージの説明](../research_wing_stage01/COLLISION.md) を参照してください。FBX単体には自動適用されません。移動・カメラ・アニメーションの接続、しゃがみ時の寸法変更、MyEngine実機確認は含みません。
