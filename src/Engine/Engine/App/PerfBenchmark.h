#pragma once

#include <string>

namespace mye {

// Release-only measurement entry point. The output path is absolute or relative to cwd.
int RunPerfBenchmark(const std::wstring& outputPath, const std::string& commitSha);

} // namespace mye
