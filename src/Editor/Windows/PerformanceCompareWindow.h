#pragma once

#include <string>
#include <vector>

namespace mye {

class PerformanceCompareWindow {
public:
    ~PerformanceCompareWindow();
    bool open = false;
    void OnImGui();

private:
    struct CommitRow {
        std::string sha;
        std::vector<std::string> parents;
        std::string date;
        std::string subject;
        int lane = 0;
        std::vector<std::string> lanesBefore;
        std::vector<std::string> lanesAfter;
    };
    struct PerfRow {
        std::string name;
        double currentMs = 0.0;
        double baseMs = 0.0;
        double targetMs = 0.0;
        bool hasBase = false;
    };

    void RequestHistory();
    void PollHistory();
    void BuildLanes();
    void DrawGitline(float height);
    void RunComparison();
    void PollPerformanceCi();
    bool LoadPerformanceResult();

    std::wstring repoRoot_;
    std::wstring historyLogPath_;
    void* historyProcess_ = nullptr;
    bool historyRequested_ = false;
    float historyRatio_ = 0.6f;
    std::string historyError_;
    std::string headSha_;
    std::string firstParentSha_;
    std::string selectedSha_;
    std::vector<CommitRow> commits_;

    void* perfProcess_ = nullptr;
    std::wstring perfLogPath_;
    std::wstring perfResultDir_;
    std::string perfStatus_;
    bool perfErrorSeen_ = false;
    std::string perfUrl_;
    std::string perfCurrentCommit_;
    std::string perfBaseCommit_;
    std::string launchedTarget_;
    std::string launchedBase_;
    std::vector<PerfRow> perfRows_;
};

} // namespace mye
