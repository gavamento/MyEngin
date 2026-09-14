# 粒子・アセット・RT シーン・VFX・selftest の経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## Asset/CookedCache.h / Asset/CookedCache.cpp — 封印キャッシュとサブアセット ID
- (M51j) 封印キャッシュを入れた主因は、当時モデルのサブアセット AssetID が正規化絶対パス由来だったこと。配布物を移設すると再クックが別 ID を登録し、シーン参照が全部空振りした。
- (M74a) サブアセット ID を .meta の GUID 由来にしたので、ID の正しさは封印に依存しなくなった。それでも封印を残すのは「移設で pathKey / mtime が必ずずれる」「DDS 一括後は元画像そのものが無い」の 2 点のため。
- (M74a 以前) srcPathKey 不一致 (ファイル移動) で再クックする理由も、主因は ID がパス由来だったことだった。今の理由は、blob が外部テクスチャを解決済み絶対パスで持っていること。

## Asset/TerrainAsset.cpp — Reader の境界検査
- 破損 blob の巨大な要素数をそのまま信じて resize すると bad_alloc で即死する。ModelCook が実際にそれで落ちたので、TerrainAsset の Reader は長さを残量で検算してから resize する。

## Asset/TerrainAsset.h / Asset/TerrainEdit.h — 地形と sim レーン
- 地形は最初、描画専用レーン (sim に 1 バイトも触らない) だった。地形コリジョンは engine_spec §6.5 で M59 送りと決めてあり、そのとき blob をハッシュレーンへ持ち込む予定だったので、クックのバイト決定論を先に契約にした。
- 同じ理由で TerrainEdit の高さ問い合わせを sim から呼ぶことを禁じた (tick から触ると地形がワールドハッシュのレーンに入り、`replay_verify.bat` に 5 ペア目が必要になる)。M59i で sim の地形コリジョンは TerrainColliderLibrary が自分でロードしたデータで行う形になり、TerrainEdit の問い合わせはエディタ専用のまま残った。

## Particles/ParticleCurves.h / Particles/CpuParticleBackend.cpp / Particles/GpuParticleBackend.* — 描画順ソート (M42追補)
- (M57追補の申し送り) GPU バックエンドは alive list を particle_sim.cs.hlsl の gAliveOut.IncrementCounter() が返す圧縮順 (view と無関係かつ非決定) のまま描いていた。加算はビット一致するのに alpha だけ CPU と 610 画素割れていた。M42追補で GPU 上のビットニックソートを入れた。
- 最初は alpha (blendMode==1) だけを並べていたので、fog ショーケースの炎に 8 画素 / maxDiff=1 が最後まで残った。コミット① はこれを「評価場所の差」と誤診していた。正体は加算合成の丸めの順序依存 (ブレンドはクォッド 1 枚ごとに RT の精度へ丸めながら積む) で、CPU が SoA 順・GPU が圧縮順で描いていたことによる。両バックエンドが加算も同じキーで並べた時点で 0 画素になった。ParticleNeedsDrawSort(0) を false に戻すと 8 画素が戻る。

## Particles/GpuParticleBackend.cpp / Particles/CpuParticleBackend.cpp — GPU のプリウォーム (M42追補)
- (M61e) 当初 GPU バックエンドはプリウォームしない (spec 7.5 の例外) としていた。誕生直後だけ GPU 側の粒子が age≒0 に揃って CPU と別の絵になるため、M42追補で GPU も CPU と同じ上限・同じ順序で先回しするようにした。

## Particles/GpuParticleBackend.* / Particles/ParticleCurves.h — invLife を CPU から渡す (M42追補)
- 以前は emit CS が invLife を 1/life から作り直していたので、subframe のとき age 曲線が最大 1 tick ずれる (alpha のフェードが CPU と食い違う) のを許容誤差としていた。EmitData に invLife を載せて CPU の 1/lifetime をそのまま渡すようにし、ステージングは 32B → 48B/粒 (1M バーストで 32MB → 48MB) になった。

## Particles/ParticleCurves.h / Particles/GpuParticleBackend.cpp — GPU 放出数クランプ (M61f)
- 以前は 1 tick の GPU 放出数を capacity/4 で静かにクランプしていた (1 tick 暴発ガード)。maxParticles まで積んだはずのバーストが 25% で切られる罠だったので、容量全量まで緩めた。dead list 枯渇分は particle_emit.cs.hlsl の deadCount ガードが捨てる。

## ParticleSelfTest.cpp — 多点グラデーションの alpha 検査 (M42追補)
- 中間キーの検査が RGB しか見ていなかったので、GPU が中間キーを丸ごと無視して alpha のフェード曲線が別物になっていたのを C++ 側から誰も指摘できなかった。.w も検査するようにした (GPU 側の被覆は golden の fog.png)。

## ParticleSelfTest.cpp / Particles/GpuAliveEstimator.h — GPU 生存数の推定
- 以前の推定は容量合計を返していた。放出停止後に寿命分の tick で 0 に戻ることの検査は、この違いを固めるためのもの。

## ParticleSelfTest.cpp — SoA を増やしたときの長さ (M63a)
- probe の SoA を alive と同じ長さで埋めずに置いたところ、HashCpuParticles が alive 件を生バイトで畳むため、この節が実際にアクセス違反で落ちて気づいた。

## ParticleSelfTest.cpp — フリップブックの固定 fps (M63c)
- M63c より前のフリップブックは age (= 経過/寿命) 駆動だけで、寿命の違う 2 粒子が同じ経過秒で違うコマを踏んでいた。固定 fps (M63c-3 節で検査) はこれを直すために入れた。

## Particles/ParticleSystem.cpp / ParticleSelfTest.cpp — 調査用トグルと project_settings.json (M66h)
- M66h より前は SetActiveKind / SetCompareMode / SIMD トグルがその場で SaveSettings() を呼んでいた。比較モードを一瞬見ただけで、チームで共有する project_settings.json に差分が出た。M66h で setter は保存しなくなり (書き戻しは Project Settings 窓だけ)、selftest の M66h 節が「どの setter も書かない」をバイト列で固める。
- 同じ M66h で particleCompareMode / particleCompareOffsetX / particleCpuSimd の個人設定の正本を `<project>\.mye\editor_settings.json` へ移した。移行は行っていない (既定値から始まる)。setter が保存しなくなったので、ParticleSystem::Init で CLI (--particle-backend / --particle-compare) の値を差し替えるのは setter 経由でもよくなったが、Reset とログを走らせないよう直接代入のままにした。

## Vfx/VfxRenderer.h / Vfx/VfxRenderer.cpp / VfxSelfTest.cpp — VFX のフォグ (M57追補)
- M32c の VFX フォグは PS が ApplyFog を呼ばず、M29d の距離フォグ 5 本を手書きコピーしていた。M43a で足されたハイトフォグ + 太陽インスキャッタの 6 本を落としていて、同じシーンでメッシュと VFX の霧の濃さが食い違っていたのに、それに気づく仕掛けがどこにも無かった。M57追補で PS を ApplyFog に揃え、素材を純関数 BuildVfxFogParams にして VfxSelfTest (3.7) で検査するようにした。

## SceneSelfTest.cpp — ActiveComponent の階層伝播 (M64b)
- M64a までは自エンティティだけで判定していたので、親を止めても子の描画だけが残った (sim は親で止まるのに絵が残る)。M64b で階層伝播にした。

## SceneSelfTest.cpp — actor / prefab の形式判定
- actor:1 も prefab:1 も無い .json を、以前は entities だけ見て素通ししていた。今は弾く。

## SceneSelfTest.cpp — 未知コンポーネントのパススルー (M70a)
- 「型が引けないコンポーネントはロードで捨て、保存はアーキタイプだけを正本にする」非対称のせいで、保存した瞬間にディスクからデータが消えていた (スキーマ未登録でも GameLogic.dll のロード失敗でも起きる)。M70a でパススルーにした。

## SkeletonSelfTest.cpp — 部位ワールド規約 (M48a)
- 当初は `IB * jointGlobal * entityWorld == 恒等` を規約と仮定していたが誤りだった。FBX は entityWorld が恒等なので両式が偶然一致し、glTF で entityWorld を掛けた版が max|dev| = 1.000001 (全ジョイント共通の固定変換、spread = 0.000001) 外れて判明した。

## PhysicsSelfTest.cpp — opt-in 拡張のビット不変の確かめ方
- (M59f1) ジャイロ項 + 質量中心オフセットは、フィールドだけ足したビルドと配線後のビルドの 2 段階で [phys] ログが完全一致することを確かめた。
- (M59f2) 静止/動摩擦の分離 + 転がり抵抗も、既存 32 行の [phys] ログが 1 ビットも動かないことを実測した。
- (M51 後続) Collider.isTrigger の既定が false になった。selftest の床は明示の代入を残している。

## PhysicsSelfTest.cpp — 車両の運転入力と ABI (h2-7)
- M60j で ABI v15 に車両用のスロットを束ねる案があったが、廃止した。運転入力は既存 ABI で書けることを h2-7 の実走で固定している。
