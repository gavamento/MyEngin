# 描画 (src/Engine/Renderer) の経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## DeferredPath.cpp — 光パスの SRV スロット (統合契約 予約 2)
- (M54 / M56 / M57) 統合契約の予約 2 で光パスの SRV を段階的に割り当てた。最終形は [16] 本の想定で、M54 が [13]、M56 が [15]、M57 が [16]。番号を前倒しして空席を詰めると、M56 のブランチと統合したときに無言で潰し合うため、空席は詰めない方針にした。
- (M56d) SSR は予約席の t13 を取らなかった。SSR はライトパスの**出力**を読むので入力として渡せず (鶏と卵)、加算合成する別パスにしたため t13 は空席のまま残った。t14 (反射プローブ) も予約 2 の空席で、M56f で埋まった。
- (M65e) 空いていた t13 に音響の残光を入れた (選定理由は下の RenderTypes.h 節)。
- (2026-09-12「描画だけ円」) t16 に見通しビットの 3D テクスチャを足し、光パスの gbSrvs / nullSrvs が 16 → 17 本になった (剥がし忘れの的を増やさないよう nullSrvs も同時に 17)。

## DeferredPath.cpp / ForwardPath.cpp — Render 末尾の SRV 剥がし
- (M57d) Deferred 光パスの t15 (フロクセル積分結果) を張ったまま残し、次フレームの積分パスが同じテクスチャを UAV に取った瞬間に D3D が片方を黙って外す罠を踏んだ。
- (M57e) Render の末尾でも t7 で同じ罠を踏んだ (光パスの剥がしより後にスカイと透明後段が t7 を張り直すため)。Forward / Deferred とも Render 末尾で t1-t7 を剥がすようにした。
- (M65e) 残光 t8 を足したとき、剥がす本数を 7 → 8 に増やした。7 のままだと t8 が張られたまま次フレームへ生き残る (張り忘れではなく剥がし忘れが実害を出す、M57e と同型)。

## DeferredPath.cpp — デカールの RT の張り方 (M56b)
- 計画では「IndependentBlendEnable=TRUE で RT2 (position) と RT4 (velocity) を RenderTargetWriteMask=0 で塞ぐ」想定だった。実装では RT2 をその場で SRV として読むので RTV に残せず、書込マスク 0 より bind しない方が強いので、M56b で足したのは RT1 (法線) と RT3 (material) だけにした。

## DeferredPath.cpp — HZB / プローブ / RT の条件
- (M56d) SSR のために hzbOn に ssrEnabled を or で足した結果、HZB 可視化の条件を hzbOn のままにしていたため、--ssr を付けただけで画面が HZB の可視化に置き換わった (実測: 518309/518400 画素が変わった)。可視化の条件を hzbDebug に分けた。
- (M56d) --render-demo で環境 BRDF LUT (t7) が無いと差分の係数が 0 になり「置いたのに絵が 1 画素も変わらない」穴を踏んだ。M56f のプローブ合成条件に LUT の有無を入れた理由。
- (M56c) HZB を 1.6) に置いた理由: 不透明 + 地形が深度を書き終えていて、まだ半透明が乗る前だから。半透明は深度を書かないので後でも同じだが、消費者の SSR (M56d) が光パス直後に入るので「SSR より前」という制約の方が強い。
- (M44b) HZB を組む CS が深度を SRV で読む前に RTV / DSV を外さないと、SSAO off の経路で深度を SRV と DSV に同時に張る罠は、計画に実際に踏んだ記録がある。
- (M55c) GBuffer RT4 (画面速度) の可視化は、RT4 を読む本番の消費者 (M55d TAA / M55e モーションブラー v2 / M55f RT の物体モーション) が入るまで唯一の目視口だった。インスタンス SRV は M55c で前フレーム world の 1 本が増えて 2 本になった。

## DeferredPath.cpp / ForwardPath.cpp — 移設・集約
- (M54e) ShadowTileCB を DeferredPath.cpp (M54c) から RenderTypes.h へ引き上げた。Forward の PerFrameCB も同じ形を要求するようになったため。転置の式は FillShadowTilesCB 1 本。
- (M54e → M57e) Forward / Deferred の PerFrameCB は M54e のアトラス追加で「2 つのミラーを手で揃える」必要が出た。M57e のフロクセルでは FroxelForwardCB 1 本の型で共有して「片方だけ足す」を潰した (M65e の AcousticCB も同じ流儀)。
- (M46a) 定数バッファ生成 / CB 更新 / 構造化バッファ生成を GpuBufferUtil.h へ集約した。GpuParticleBackend / PostProcess / DeferredPath に同型の定義が三重化していた (定義は同一のまま移した)。

## ForwardPath.cpp / ForwardPath.h
- (M38b → M57e) スカイボックスの cubemap 経路が s0 を LINEAR/CLAMP へ差し替えたまま戻さない潜在バグが M38b からあった (フロクセルとは独立に効く)。M57e で Forward の透明段の前に s0 を張り直して直した。Deferred の透明後段は以前から張り直していた。
- (M57d → M57e) AppliesFroxel() は M57d の時点では false だった。合成が Deferred の光パスにしか無く、true にすると「ゴッドレイだけ消えて霧が増えない」= 霧が減るだけになるため。M57e で Forward 側の合成 (t7) が入ったので true にした。
- (2026-09-12「描画だけ円」) Forward の frameSrvs に t9 (見通しビット) を足し、本数が 8 → 9 本になった (Deferred の透明後段も 9)。

## RenderPath.h / DeferredPath.h — 消費者が居なかった時期
- (M55c) GBuffer RT4 (画面速度) は M55c の時点では誰も読まず、消費者は M55d (TAA) / M55e (モーションブラー v2) / M55f (RT の物体モーション) だった。「光パスの t0-t11 の並びは 1 つも動かさない」が M55c からの約束だった。
- (M56c) HZB は M56c の時点では view.hzbDebug != 0 のときしか組まず、本番の消費者 (SSR) は M56d。SSR が HZB を組む条件に or で入るのは M56c からの申し送りだった。
- (M57d) AppliesFroxel は v1 では Deferred だけが true で、Forward への配線 (t7) は M57e の枠だった。

## RenderTypes.h
- (M45) ボーンパレット上限 kMaxBones を 64 → 128 に拡張した。Mixamo の標準ヒューマノイドが約 65 ジョイントで 64 を超えるため。
- (M54c) 局所ライトの per-light パラメータ: 計画本文には「StructuredBuffer で t7/t13」とあったが予約表と食い違っており、予約表 (M54 に許した SRV は t12 のアトラス本体 1 本) を正として CB 渡しにした。
- (M54c) GpuLight の shadowTile / shadowFaces は旧 pad0/pad1 の枠を再利用したもの。統合契約 (plans/radiant-shimmering-lumen.md 付録 予約 2) が pad0/pad1 に予約していた枠で、64 バイトのレイアウトは不変。
- (M54e) Deferred 光パス / Forward / Deferred 透明後段の 3 箇所が同じタイル行列の変換を要求するようになったので、ShadowTileCB と FillShadowTilesCB を RenderTypes.h へ引き上げた。
- (M56c → M56d) hzbDebug の説明は M56c の時点では「ピラミッドを作るかどうかもこの値だけで決まる。本番の消費者 = SSR は M56d で、そちらが入ったら ssrOn も作る条件に加わる」だった。M56d で加わった。
- (M57d) FroxelForwardCB の viewZRow 規約 (view 行列の第 3 列そのもの。カメラ前方ベクトルの内積にしない) は M57d が Deferred 側で確定させた。
- (M57e) FroxelIsBound は消費者が 5 つに増えた時点で式を 1 本に畳んだ。
- (M57c) テンポラルの恒等性は「テンポラル off で直前コミットとビット一致」というロードマップの受入基準から来ている。
- (M63b) パーティクルの billboardParams を CPU/GPU の 2 バックエンドで手写ししていて「片方だけ直す」事故になりかけた。ParticleLightCB の形と詰め方を 1 本にした理由。
- (M57d / M57e) SRV の剥がし忘れを t15 / t7 / t3 で 3 度踏んだ (particlelight::ShadowIsBound の注意の出所)。
- (M65e) 音響の残光に t13 を選んだ理由: t13 は SSR の予約席だったが SSR (M56d) は光パスの出力を読む別パスになり空いたままだった。ここを取ると Deferred の gbSrvs[16] / nullSrvs[16] の本数が 1 つも変わらず、M57d/e が 3 回踏んだ「SRV 剥がし忘れ」を構造的に避けられた。Forward 側は t8 で本数が 7 → 8 に増え、張る側と剥がす側の 4 箇所 (ForwardPath の 2 + DeferredPath の透明後段の 2) を 8 にした。
- (2026-09-12「描画だけ円」) 見通しビットで Deferred は t16 (gbSrvs 16 → 17 本)、Forward は t9 (frameSrvs / fwdSrvs 8 → 9 本) になった。
- (2026-09-14) 波スロットを 16 → 32 本に増やし (AcousticField::kMaxWaves)、見通しビットのテクスチャを R16_UINT から R32_UINT に広げた。
- (M67d) rtReflRestir = 0 の経路は「M67d 以前の経路」と書いていた (ReSTIR 無しの uniform 分岐)。
- (M46i) 発光の受け入れ基準は「発光を使わないマテリアルは M46i 以前とビット単位で同じ絵になる」だった。

## RayTracing/RtTypes.h
- (M67) RtInstance::reflectionClass は旧 pad0 の枠をそのまま意味付けしたもの (レイアウト 80 バイト不変)。
- (M46i) RtMaterial.emissive は M46i まで 0 だった。
- (M67) kRtReflClassTable の向き (Hero ほど数字が小さい = 保守的) は元計画の初版と逆。RtReflRestirParams の既定は「既定のまま on にしたら元計画の表で動く」ようにそろえた。
- (M67h / S5) 2 軸 × 5 条件 (64 run) を測った。結論は「4 行は規則どおり据え置き、Hero の mCap だけユーザー判断で 8 → 16」。数値と領域の取り方は ADR-016 の「S5 の結論」節。
  - 据え置いた 4 行: mCap を上げるとフリッカーは必ず減る (片側の軸) が、動く反射像の追従が同時に落ちる。規則「フリッカー 20% 以上改善 かつ 追従の低下 0.05 未満」を満たす段差が 1 つも無かった (24→32 は軸 A が 14.0% / 9.6% で 20% にすら届かない)。
  - Hero は規則も不成立だった。追従の低下は規則の指標で +0.083 / +0.056 / +0.053 / +0.042 (4 標本中 3 つが閾値 0.05 超)、動きを分離した対照では 0.010 (= 分解能内)。つまり「遅れない」ではなく「我々の道具では判定できない」。2026-09-08 にユーザーがフリッカー 35.5〜38.9% の改善 (Ro 3.789 → 2.317 / R 4.343 → 2.802) を採り、決着しなかった遅れのリスクを引き受けて 16 を選んだ。8 へ戻すならこの数値がそのまま根拠。
  - 16 にしたことで Hero == Character == Default になった。
- (sub-06 round 2) spatial (空間再利用) の既定を 0 に決めた計測。音響デモの床、粗さ 0.5、--rt-debug 11 = 反射レーンだけ、frame 120/121 のフリッカー: off 2.943 → temporal 単独 0.181 → spatial on 0.255。temporal 単独が最良で、spatial を足すと 1.4 倍に戻った。S5 の再評価 (M67h) でも結論は「off 維持」(ADR-016「S5 の結論」の R2)。

## RayTracing/RtPasses.cpp / RtPasses.h
- (M67f) 空間タップ回転のフレーム項を外した。回すと候補集合が毎フレーム入れ替わり、乗り換えがそのままフリッカーになる (実測 2 倍: 1.21 → 0.61)。
- (M67h) RtRestirCB の frameIndex の枠を pad1 にし、RenderReflection の rs.frameIndex への代入を削除した (読み手がいないため)。枠は 240 B / offsetof(classTable) == 160 を動かさないために残した。
- (M67f) gRsRadiusAlphaRef を足したとき、明示パディング float3 を添えて 240 B にした (足さないと C++ は 148、HLSL は 160 から配列を始めて 12 バイトずれる)。
- (M67f) reservoir スロットの初版は「set[0] 固定 = spatial が書き戻す」だったが、近傍の履歴が自画素の履歴へ混ざり、M の重いクラスのサンプルが 1 フレームに半径ぶんずつ拡散した (sub-06 round 1 実測: Prop が 40 フレームで画面の 94% を占拠)。RtHistory と同じ ping-pong に変えた。
- (M67h) reservoir の組 A/B の呼び方は、M67f で ping-pong を入れた時点で意味が変わった (A = 前フレームに書いた面)。
- (M55f / M67d) 画面速度が null のときの縮退や ReSTIR のコンパイル失敗時は、それぞれ「M55f 以前」「M67d 以前」と同じ絵になる、と書いていた。

## RayTracing/RtMath.h
- (M67) ReSTIR 節の配置: sub-03 の見立ては M46h 節の直後だったが、RtLuminance への前方参照になるので SVGF 節より後ろに置いた。

## FroxelPass.h / FroxelPass.cpp / VolumeTexture.h / VolumeTexture.cpp / RenderSelfTest.cpp
- (M57a) 設計より先に計測した理由: shot_verify.bat が「RT デモは WARP では重すぎる」と明記していて、その RT GI は 960x540 の半解像度 = 約 130k ピクセル。フロクセル 160x90x64 はセル数だけで 7 倍の 921,600 ある。ここが CI 予算に載らないなら M57b 以降の設計 (解像度・パス数・golden を CI に入れるか) が全部変わるので、実装を始める前に「空の CS を回した壁時計」と「typed 3D UAV が WARP で動くか」を数字で確定させた。この数字を出すこと自体が M57a の成果物だった。候補解像度の先頭 (160x90x64) は計画が想定した値。
- (M57b) 注入パスは、積分 (M57c) と最終画像への合成 (M57e) より先に作った。M57b の時点では 1 パスしか無く、書いた結果を読む者も居ない (絵は 1 ビットも変わらない) が、「注入のコストが WARP で許容範囲か」が M57c の設計 (テンポラルを入れるか / golden を CI に載せるか) の入力になるため。M57a が測ったのは「空の CS」= 下限であって、注入のコストではなかった。
- (M57b) FroxelSettings は M57b の時点ではコンポーネントを触らなかった。統合契約 予約 4 が M57c の枠と決めていて、先回りして末尾 append すると M56 と番号を取り合うため。既定値は予約表に書かれた値にそろえ、M57c で CameraPostFxComponent の froxelDensity / froxelAnisotropy から供給されるようにした。
- (M57b / M57c) 消費者 (積分 = M57c、合成 = M57d / M57e) が居ない間は、selftest と --froxel-dump / --froxel-probe の読み戻しが「絵からは 1 画素も分からない」値を確かめる唯一の手段だった。「透過率が 1 未満になった」で済ませない、は M57b の教訓。
- (M57c) VRAM の数字は、計画が「3D テクスチャは VRAM を増やす」と名指ししている箇所の根拠。
- (M65d) VolumeTexture に withUav = false (SRV だけの 3D テクスチャ) を足した。
- (M57c) フロクセルの履歴を viewKey キーで持つのは TaaPass / RtPasses::kHistorySlots に続いて 3 度目。

## HzbPass.h
- (M56c) HZB は M56c の時点では消費者 (SSR = M56d) がおらず、デバッグ表示だけが唯一の目視口だった (M55c の velocity と同じ立ち位置)。

## PostFxMath.h / RenderSelfTest.cpp
- (M55a) 深度線形化は M55a 以前、同じ式が 5 つのシェーダにローカルコピーで散っていて、CPU 側の検査もパーティクル文脈 (LinearizeParticleDepth の端点 2 点) しか無かった。共有版 (PostFxMath.h::LinearizeDepth) にした。LinearizeDepth は当時「今後の HZB・SSR・froxel が共有する」予定だった。
- (M44d → M55e) モーションブラー v1 (M44d) はカメラのみの深度再投影で、静止カメラでは回る物体がブレなかった。M55e で速度源を画素ごとに選ぶ (ジオメトリ画素は GBuffer RT4) ようにしたのが v1 から変わった唯一の点。
- (M55c) velocity は M55c の時点で誰も読んでおらず、ピクセル回帰では 1 ミリも被覆できなかった。

## GpuResources.h
- (M70d, dogfooding #12) 組込みプリミティブは以前は全部遅延生成で、Runtime で生きているのは cube / sphere だけだった (RuntimeMain がショーケースの材質登録を呼ぶときの副作用)。結果、「エディタで作った円柱を含むシーンが Runtime では黙って描画されない」が成立していた。Init で 6 種を登録し切るようにした。
- (M67) M67 以前の Material はちょうど 56 バイトで、暗黙パディングの穴が無かった。reflectionClass を足して 60 → 64 バイトに丸められ、明示的な pad0 を置いた。

## ReflectionClassJson.h
- (M67 → M67h) M67 の実装は reflectionClass を JSON の型で弾いていたため、`3.0` のように float で書かれた値が無言で無視される静かなデータ損失があった。M67h で「非整数」を値で判定するようにした。

## PostProcess.cpp / PostProcess.h
- (M42d / M43b / M44b) PostFxCB の distortEnabled は旧 pad[0]、godrayEnabled は旧 pad[1]、autoExposure は旧 lutPad[0] を転用した。
- (M38a) enablePostFx=false のとき OETF が掛からない見た目は、リニアパイプライン以前の「旧来の見た目」。

## TerrainPass.h — 地形の SRV スロット (M58d)
- 地形専用の SRV を t20 以降へ逃がしたのは、当時 Deferred の t12-t15 / Forward の t6-t7 が他マイルストーンの予約席 (計画の付録「予約 2」) で、ブランチ統合で番号がぶつかっても *コンパイルは通る* という一番静かな壊れ方を避けるため。
- M58c の時点では、地形のテクスチャ 8 枚 (4 レイヤ x albedo+normal) は M58d の予定だった。

## ShadowAtlas.cpp
- (M54d) M54c はスポット 2 本 = 2 パスだったのでタイル毎カリング無しでも問題にならなかったが、点光源 (6 タイル) が入ると一気に 6 倍になり、WARP 撮影が計測不能に遅くなる (計画 M54d の★罠)。タイル毎の視錐台カリングを入れた。

## SsrPass.cpp
- (M56b) CopyResource を RTV に bind されたままのリソースへ当てられない順序の罠は、M56b の RT1 (法線) コピーで先に踏んだ。

## ImGuiTheme.h / ImGuiRenderer.cpp
- (テーマ第 3 世代) M27a の UE5 風テーマ (灰色の浮き上がる入力欄) を置き換えた。
- 配色ルール 2 (ImVec4 リテラルでの着色禁止) は、かつて Warn 系だけで 4 通りの黄色が散っていたことの再発防止。

## EnvMapBaker.h / Skeleton.h / GpuTimer.h / RenderSelfTest.h
- (M56e) EnvMapBaker::BakedEnv を public にしたのは M56e (反射プローブ)。
- (M18) SkinnedModelLibrary::Enumerate が無く、SkinnedMesh.model のピッカーが混合リストへ落ちていたのは M18 の積み残し。
- GpuTimer のリングは当初 3 フレームだった記述が残っていた (現在は kFrames = 6)。
- RenderSelfTest は M16 の時点では視錐台カリングだけを検証していた。
