//====================================================================================
//                          ModalInferenceBackend.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          Deep-Modal 推論バックエンドの抽象 (CPU/将来 GPU 共通)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Modal/DmNet.h"
#include "Engine/Engine/Modal/Voxelizer.h"

namespace mye {

// 推論バックエンドの抽象 (ユーザー判断: 最初は C++ CPU 実装、将来 GPU Compute Shader
// 実装 (D3d11ModalBackend、本サブでは差し込み口のみ) へ差し替えられる形にする)。
// **同じ fixture selftest を両バックエンドが通る**想定 (許容誤差だけ違ってよい)。
class ModalInferenceBackend {
public:
    virtual ~ModalInferenceBackend() = default;

    virtual const char* Name() const = 0; // "cpu" / "d3d11cs"

    // true: Infer をワーカースレッドで呼んでよい (CPU 実装)。
    // false: Infer はメインスレッド専用 (GPU の immediate context 前提) —
    //   呼び出し側 (ModalSoundLibrary::Pump) が 1 フレーム 1 ジョブで吸収する
    virtual bool RunsOnWorkerThread() const = 0;

    // モデル差し替えごとに 1 回。重みの展開・配置など、Infer より前にやっておける
    // 前処理はここで済ませる。false は Infer を一切呼ばないこと (未 Prepare 状態)
    virtual bool Prepare(const DmNet& net, std::string* err) = 0;

    // 32^3 ボクセル → ネット出力 (192 チャンネル × 16^3 cell、[channel][cell] の
    // channel-major 順、cell index は modal::CellIndexOf と同じ = cx が最内)。
    // out192x4096 のサイズは呼び出し側 (ModalFeatureMap::BuildFeatureMap) が保証しない —
    // 実装が resize すること
    virtual bool Infer(const modal::VoxelGrid& in, std::vector<float>& out192x4096,
                       std::string* err) = 0;
};

} // namespace mye
