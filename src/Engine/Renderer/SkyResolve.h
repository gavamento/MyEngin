#pragma once
// スカイの描画モード解決 (RenderSystem::PrepareEnvironment と DeferredPath の RT 入力が使う純関数)と、
// 描画中に同期ロードするテクスチャ (スカイ / LUT) の再試行判定。
// GPU に触らないので SelfTest から直接検証できる。
//   skyMode: -1 = スカイ無し (clearColor 背景、IBL も焼かない) / 0 = グラデーション /
//            1 = キューブマップ / 2 = パノラマ (正距円筒 2D)

#include <cstdint>

namespace mye {

// requestedMode: CollectEnvironment が Skybox から詰めた値 (Skybox が無ければ -1)。
// hasTexture:    スカイテクスチャを SRV まで解決できたか。isCube: そのテクスチャがキューブか。
// ★-1 はそのまま返す — Skybox を置いていないシーンにグラデーション空と IBL を出さない
//   (M77a で無条件に 0 へ落としていた回帰の修正)
inline int32_t ResolveSkyMode(int32_t requestedMode, bool hasTexture, bool isCube)
{
    if (requestedMode <= 0) {
        return requestedMode;
    }
    if (!hasTexture) {
        return 0; // テクスチャが解決できない cubemap/panoramic はグラデーションで代用
    }
    return isCube ? 1 : 2;
}

// RT (レイトレーシング) の空入力。RT シェーダは TextureCube (t6) しか持たないので、
// パノラマのときは 2D SRV を張らず、パノラマから焼いた IBL の prefiltered キューブで代用する
enum class RtSkySource : int32_t {
    None,        // 張らない
    SkyCubemap,  // view.skyCubemap (キューブマップのスカイ)
    IblCubemap,  // view.iblPrefiltered (パノラマから焼いたキューブ)
};

struct RtSkyChoice {
    RtSkySource source = RtSkySource::None;
    int32_t envSkyMode = -1; // RtEnvCB.skyMode に入れる値 (-1 ambient / 0 gradient / 1 cube)
};

inline RtSkyChoice ResolveRtSky(int32_t skyMode, bool hasSkyCubemap, bool hasIblPrefiltered)
{
    RtSkyChoice c;
    if (skyMode == 1) {
        if (hasSkyCubemap) {
            c.source = RtSkySource::SkyCubemap;
            c.envSkyMode = 1;
        } else {
            c.envSkyMode = 0; // キューブが無いのに 1 のままだと真っ黒になる
        }
    } else if (skyMode == 2) {
        if (hasIblPrefiltered) {
            c.source = RtSkySource::IblCubemap;
            c.envSkyMode = 1;
        } else {
            c.envSkyMode = -1; // Skybox.lighting=0 で IBL が焼かれていない → 定数アンビエント
        }
    } else {
        c.envSkyMode = skyMode; // -1 / 0
    }
    return c;
}

// 前回の同期読み込みで読めなかったテクスチャ (GUID と、そのときのファイル更新時刻。取れなければ 0)。
// スカイと LUT は描画中に同期で遅延ロードするので、失敗を覚えないと壊れた画像を毎フレーム・
// ビューごとにデコードし直し、エラーログを出し続ける
struct FailedTextureLoad {
    uint64_t id = 0;
    int64_t stamp = 0;
};

// もう一度読みにいくか。同じ GUID・同じ更新時刻で失敗済みなら読まない。
// GUID が変わった (別の画像を指した) か、ファイルが書き換わったら読み直す (直したら自動で拾う)
inline bool ShouldRetryTextureLoad(const FailedTextureLoad& failed, uint64_t id, int64_t stamp)
{
    return failed.id == 0 || failed.id != id || failed.stamp != stamp;
}

} // namespace mye
