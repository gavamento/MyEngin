#pragma once

namespace mye {

// CPU time to submit 1000 real D3D11 draws on WARP. Returns -1 on setup failure.
double RunDrawSubmissionBenchmark(int warmups, int samples);

} // namespace mye
