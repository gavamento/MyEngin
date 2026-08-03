#pragma once

namespace mye {

// レイトレーシングの BVH 構築とトラバーサルのヘッドレス回帰テスト (M46b)。
// GPU は使わず、HLSL と同一ロジックの CPU ミラー (RtMath.h) を検証する
bool RunRtSelfTest();

} // namespace mye
