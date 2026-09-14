# シェーダ・Core・Platform の経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## assets/shaders/acoustic_common.hlsli — 残光の SRV 席 (t13 / Forward t8)
- (M65e) Deferred 光パスの t13 は SSR の予約席 (統合契約 予約 2) だったが、SSR (M56d) が光パスの**出力**を読む別パスになったので空いていた。M65e がここを取ったのは、gbSrvs / nullSrvs の本数 (当時 [16]) が 1 つも変わらないため — 本数を増やすと M57d/e が 3 回踏んだ「SRV 剥がし忘れ」の的が増える。
- (M65e) Forward 側は t8 を足して本数が 7 → 8 に増えた。null を張り直す側も 8 にした (剥がし忘れると次フレームまで生き残る、M57e の罠)。
- (2026-09-12「描画だけ円」) 見通しビットを Deferred t16 / Forward t9 に足し、Deferred 17 本 / Forward 9 本になった (DeferredPath 側の経緯は renderer.md)。

## assets/shaders/acoustic_common.hlsli — 波スロット数と albedo 混合の閾値
- (2026-09-14) MYE_ACOUSTIC_WAVE_SLOTS を 16 → 32 に上げた。R32_UINT のマスクなので 32 が上限。
- albedo 混合の閾値を 0.55〜0.80 にしていたときは、音源の真下 1m 未満にしか色が乗らず、音響デモで変化が 648 px しか無かった。今の閾値 (amp 0.3 の忍び足で「1.1m 以内 / 3m から先」) はこれを受けたもの。

## assets/shaders/common.hlsli — 局所影の 1 本化 (ResolveLocalShadows)
- (M54c / M54d) Deferred にライトごとの影ループを直書きしていた。M54e で経路が 4 つに増えるので 1 本に畳んだ (面選択や enabled 判定の漏れがコンパイルを通り、絵の食い違いにしか出ないため)。
- (M54c) enabled == 0 で全要素が厳密に 1.0 = 乗算が恒等になることが、受入基準「機能 off で直前コミットの PNG とビット一致」の根拠だった。テクスチャ引数を ApplyLighting に持ち込まない設計にしたので、当時の Forward 3 本 (forward_lit / forward_lit_instanced / forward_skinned) は 1 文字も変えずに済んだ。その後 Forward 3 本も ResolveLocalShadows を呼ぶようになり、影なしオーバーロードを呼ぶのは forward_terrain だけになった。
- (M54d) M17 の単一シャドウマップ用 SampleShadowPCF を削除した。M38d の CSM 化で呼び出しが消えてから 5 マイルストーン誰も呼んでおらず、SampleShadowAtlas / SampleShadowCSM と 3 つ目の「ほぼ同じ 3x3 PCF」が並ぶと、どれを直せばよいか読み手に分からなくなるため。

## assets/shaders/common.hlsli — LightSample / FogFactor
- (M63d) 距離減衰とスポットの式は ApplyLighting のループに直書きされていて、パーティクルのライティングが同じ式を手写しする形になっていた。片方だけ直すと「メッシュと粒子で光の届き方が違う」が静かに起きるので LightSample へ抽出した。out 引数 2 本なのは抽出前の代入順序 (= ビット) を保つため。
- (M57追補) 粒子用に係数だけを返す FogFactor を足したとき、ApplyFog は 1 文字も変えなかった (全 lit シェーダが通るので、1 ULP 動くと当時の golden 14 枚が全部動く)。ApplyFog との式の一本化は見送ったまま。

## assets/shaders/deferred_light.hlsl / skybox.hlsl — グリッドより奥の解析フォグ
- (M57e) 背景や空に ApplyFog が掛かる挙動は M29d 以来一度も無かった。フロクセルでそこへ足すと froxel off の絵まで動かしたくなり、空は濃霧のとき丸ごとフォグ色に潰れるので、M57e はフロクセル区間ぶんの段を消すだけにした。

## assets/shaders/forward_terrain.hlsl — PerFrame のオフセットの穴
- (M54e / M57e) M54e は forward_terrain の PerFrame を切り詰めたまま残したため、M57e でフロクセルを後ろに足すとオフセットが丸ごとずれる穴になっていた。

## assets/shaders/deferred_terrain.hlsl — RT4 に 0 を書く
- (統合 1) 地形の PS が SV_Target4 を書かずに済ませる穴を実際に踏みかけた。

## assets/shaders/froxel_clear.cs.hlsl — M57a で何を測ったか
- (M57a) ClearUnorderedAccessViewFloat でも同じクリアはできるが、注入と積分が**同じスレッド割り** (XY タイル + Z 列) で走るかをここで先に確定させたかった。フロクセルのコストはセル数がそのまま効くので、割り方の実測を設計の入力にした。M57a の成果物は「空の CS を 921,600 セルにディスパッチしたときの WARP の壁時計」。

## assets/shaders/froxel_inject.cs.hlsl — 平行光を注入しない理由
- (M57b) 太陽の大気散乱は当時 common.hlsli の ApplyFog (距離フォグ + M43a の太陽インスキャッタ) と postfx_godray_* が担当しており、グリッドへ素直に足すと同じ現象が 3 回計上される。役割分担は M57d で決めることにして、M57b は「局所ライトだけをフロクセルに載せる」= 既存の霧と重ならない範囲に限定した。分担の結論は deferred_light.hlsl の「大気散乱」の表。

## assets/shaders/particle_billboard.hlsli — CPU / GPU の共有点
- (M42追補) alpha ソートのキーの式を CPU と GPU に手写ししていて、片方だけ直す事故を実際に踏んだ (ParticleAlphaSortViewZ へ寄せて決着)。四隅の回転を共有ヘッダにしたのは同じ形を避けるため。
- (M42c → M63c) タイル UV の式は M42c から CPU 経路と GPU 経路の PS へ手写しされていた。M63c のコマ間補間で写しが 4 箇所になるので SampleFlipTile へ寄せた。
- (M63b) 実測 (WARP、M63b 時点) では伸びの領域は画素一致し、golden 間の差 217→221 画素は M63a からある回転部の差のまま — EvalParticleStretch はずれを増やしていなかった。

## assets/shaders/particle_gpu_common.hlsli / particle_render_gpu.hlsl — GPU バックエンドの取りこぼし
- (M42追補まで) GpuParticleCB に中間キーの枠が 1 つも無く、GPU バックエンドは多点グラデーションの中間キーを丸ごと無視して begin→end の 2 点線形だけで色を作っていた。
- (M57e) GPU 描画の CB にフォグとフロクセルが 1 行も無かった (M57e のやり残し)。加算合成は背景の減衰を受けないので、周囲が霞むほど GPU 粒子だけがくっきり残り、GPU に切り替えると粒子だけ霧が抜けていた。M57追補で CPU 版と同じ順序 (フォグ → フロクセル → ソフトフェード) を入れた。
- (M63e) sim CS は M42e 以来、深度衝突の式を手写ししていた。M63e で ParticleClipToUv / ReflectWithFriction へ寄せ、thickness の 0.0f 固定を撤廃し、gCollParams.w の予約枠を摩擦へ回した。

## assets/shaders/particle_light.hlsli — 粒子の受光
- (M63c まで) 粒子は完全 unlit で、点光源の隣でも影の中でも同じ色で光っていた。M63d で受光を入れた。

## assets/shaders/rt_refl_restir_spatial.cs.hlsl / rt_restir_cb.hlsli — ReSTIR の実測
- (M67f) 初版は spatial の結果を reservoir へ書き戻していた。近傍の履歴が自画素の履歴に混ざり、(a) M の重いクラス (Prop) のサンプルが 1 フレームあたり半径ぶんずつ拡散して 40 フレームで画面の 94% を占拠 (実測 9988 → 40432 px、平均輝度 +8.4%)、(b) 採用サンプルの乗り換えがフリッカーになった。書き戻しを断った。
- (round 1 実測) 半径を受け側の α に比例して縮めなかったときは、粗さ 0.10 で -5.2% の暗化 / フリッカー 5 倍。タップ回転をフレームで回したときはフリッカー 2 倍。これで M67f はタップ回転のフレーム項を外し、M67h で gRsFrameIndex の枠は未使用になった。
- 候補を 1 つも採れない画素 (補間法線が視線の裏へ回ったシルエット際) は --render-demo の frame 3 で 950 テクセルあった。1spp をそのまま通す理由。

## assets/shaders/vfx_sprite.hlsl — VFX のフォグ
- (M32c → M57追補) M32c の VFX フォグは ApplyFog の手書き劣化コピーで、M43a のハイトフォグと太陽インスキャッタを持っていなかった = 同じシーンでメッシュと VFX の霧の濃さが食い違っていた。M57追補で forward_lit と同じ ApplyFog へ寄せ、距離も「VS で計算 + 線形補間」から PS の毎ピクセル計算に変えた。

## src/Engine/Core/AssetKeyResolver.h / Components.h — サブアセット ID と地形のパス
- (M51j) モデル由来のサブアセット ID が正規化絶対パスのハッシュだったせいで、シーンをコミットできなくなった。TerrainComponent がアセットを相対パス文字列で持つのはこの穴を避けるため。
- (M74 以前) 接頭辞が正規化した絶対パスそのものだったので、シーン JSON に保存したサブアセット ID がチェックアウト先に依存した。clone 先が違う 2 台で、互いが置いたモデルがログも無く描画から消えた (三校のステージで踏んだ)。M74a で .meta の GUID 由来に直した (ADR-019)。

## src/Engine/Core/Components.cpp — IsEntityActive が祖先を見る理由
- (M64a → M64b) M64a まで自エンティティしか見ていなかった。ドッグフーディングで実際に踏んだ: 車に乗っているあいだプレイヤーを親ごと止めたのに、体の箱が地面に立ったままだった。祖先を辿る形は PhysicsSystem の車輪→剛体探索、Parts::RaycastParts の root 判定で既に使っていた。

## src/Engine/Core/Components.cpp / Components.h — フィールドのまとめ確保
- (M61a / M63a) 後続サブが消費するフィールドを最初のサブでまとめて確保した。5 サブに分けて足すと sizeof が 5 回変わり、snapshot 版 bump と golden .rep 再記録が 5 回要るため (M63a で snapshot 版は v7)。
- (M65a) 音響 5 コンポーネント (TypeId 45〜49) を、M65f までしか使わないフィールドも含めて 1 コミットで確保した (途中のサブで足すと snapshot 版が 5 回上がる)。
- (M68a) AcousticAudio も M68b でしか読まない残響と波の欄まで確保した (後から足すと Inspector のレイアウトとシーン JSON が 2 度動く)。
- (M60a) Joint はリミット / モータ / 破断 (M60c/d) の分まで先に切った (M59a1 で確立した「スキーマを先に切る」流儀) が、それはリミット/モータ/破断しか見ておらず、restRotation (M60b) が漏れた (申し送り M60b-1)。
- (M18 追補 / M65h 追補) 既存の型へ末尾フィールドを足して生バイトが変わり、kSimSnapshotVersion を上げた (M65h 追補で v11、M18 追補で v16)。

## src/Engine/Core/Components.cpp — M60' の Cloth / SoftBody の TypeId
- M60' は 45=Cloth / 46=SoftBody を予約していたが、M65a の音響 5 本 (45〜49) が先に取ったので 50/51 へ、M68a が 50 = AcousticAudio を取ったので 51/52 へ繰り下げた (M60'h/k はどちらも未登録なのでデータは壊れていない。plans\supple-weaving-loom.md の予約表も同時に書き換えた)。その後 WaveSound (51) と M75 の UI コンポーネント群 (52〜) が先に埋めることになり、見込みは 62/63 になった。

## src/Engine/Core/Components.h — Joint の接触除外 / Skybox の cubemap
- (M60g2 まで) 繋がったペアの接触を外す手段が無く、隣り合う骨などのコライダーを見た目より縮める幾何の逃げで凌いでいた。M60j で disableCollision (直接繋がった 1 ペアだけ) を足した。
- dogfooding #8 に cubemap 未実装とあったが、M38b で実装済みだった (記述が古かった)。

## src/Engine/Core/JobSystem.cpp — ParallelRanges のフィールド書き込み
- 以前はフィールドを素の書き込みにしていて、drain 中のワーカーが破棄済みの fn_ を読んで落ちていた。

## src/Engine/Platform/Input.h / Input.cpp — ゲーム面の記録形式
- (M70b) EngineLoop が uilayout::CanvasSize を解いて渡し (InputCanvas)、CaptureSnapshot がキャンバス座標へ正規化した UI キャンバス 4 値を記録していた。M75c でキャンバスが複数になると倍率がキャンバスごとに違い「正規化済みの座標 1 個」は記録できないので、M75b で換算前のゲーム面 px と面の寸法の記録へ変え、換算を sim 側の uilayout::CanvasOfInput / SurfaceToCanvas に寄せた。
- (M75b) 既定キャンバスでのキャンバス座標は M70b の mouseCanvasX と同ビット — どちらも float(mouseX) を同じ CanvasSize(面).scale で割るだけ (UISelfTest が 960x540 / 1600x900 などで memcmp する)。面が未確定のときの値も M70b の退化扱い (CanvasSize(0,0) の scale 1 で割る) と同じになる。
- InputSnapshot のサイズは M64a で 64 → 72、M70b で 72 → 88、M75b で 88 → 112 (ゲーム面 + 文字キュー)。そのたびに kReplayFileVersion / kSimSnapshotVersion / kNetProtoVersion を同時に上げた。

## src/Engine/Platform/InputActionsSelfTest.cpp — 合成入力の偏り
- 不合格だった旧実装は `(h & 15) - 7` の非対称な範囲で、平均がちょうど +0.5 だった。

## src/Engine/Platform/Net/UdpSocket.h — IPv6
- 2 人 P2P に限定したので、IPv6 は M53 送りにした (計画で「見送り」)。
