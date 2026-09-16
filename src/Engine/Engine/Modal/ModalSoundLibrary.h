//====================================================================================
//                          ModalSoundLibrary.h
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          .dmnet モデル管理 + メッシュ毎の特徴マップの非同期焼き
//====================================================================================
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Modal/CpuModalBackend.h"
#include "Engine/Engine/Modal/ModalFeatureMap.h"

namespace mye {

struct RenderResources;

enum class ModalState : uint8_t {
    Missing, // mesh 未登録 / CPU 頂点なし (キャッシュしない。後から登録され得る)
    Baking,  // ジョブ投入済み、結果待ち
    Ready,   // 特徴マップあり、Get() で取れる
    Failed,  // ボクセル化/推論が失敗 (WARN 1 回)
    NoModel, // .dmnet 未ロード
};

// メッシュ (AssetID) → ModalFeatureMap の非同期焼き (spec §4.1「焼き」)。
// `TextureLibrary::AsyncWorker` (GpuResources.cpp) と同型のワーカー (mutex + cv + deque)。
// ボクセル化は**常にワーカー**、推論はバックエンドの `RunsOnWorkerThread()` で
// ワーカー/メインスレッド (`Pump()`、1 フレーム 1 ジョブ) が切り替わる。
// `.msfm` の読み書きはメインスレッドのみ (CookedCache は非スレッドセーフ)。
//
// ★スレッド安全性の設計判断: `ModalInferenceBackend::Prepare()` は `Infer()` と
//   同時に呼ばれてはならない (CPU 実装は Prepare で変換した重みを Infer が読むだけの
//   単純な設計で、二重バッファ等は持たない)。`ReloadModel()` は **ワーカーの手持ちジョブが
//   尽きるまで待ってから** Prepare をやり直すことでこれを避ける (呼び出し頻度は
//   .dmnet のホットリロード時だけなので、数百 ms 級の待ちは許容する)
class ModalSoundLibrary {
public:
    ~ModalSoundLibrary();

    void Init(RenderResources* resources)
    {
        resources_ = resources;
    }
    void SetBackend(std::unique_ptr<ModalInferenceBackend> backend);
    // name: "cpu" か "d3d11cs"。"d3d11cs" は未実装なので WARN を出して CPU へ縮退する
    // (D3d11ModalBackend は差し込み口のみ、spec §3「後回し」)。未知の綴りは false
    bool SetBackendByName(const std::string& name);
    const char* BackendName() const
    {
        return backend_ ? backend_->Name() : "none";
    }

    // .dmnet を読み、バックエンドへ Prepare する。失敗は WARN + false (以後 NoModel のまま)。
    // 成功したパスを覚えておき、ReloadModel() の再入に使う
    bool LoadModel(const std::wstring& path);
    // ワーカーの手持ちジョブが尽きるのを待ってから、直前に LoadModel した path を読み直す
    // (ReloadHub がホットリロードから呼ぶ)。モデル未ロードなら false
    bool ReloadModel();

    ModalState Request(AssetID mesh);
    const ModalFeatureMap* Get(AssetID mesh) const;
    const DmNetHeader* Header() const
    {
        return net_ ? &net_->header : nullptr;
    }

    void Pump(); // メインスレッド: ワーカー結果の取り込み + GPU 推論の 1 フレーム 1 ジョブ

    // 直接登録 (selftest / 手続き生成メッシュ用)。同 ID は差し替え
    void Register(AssetID id, ModalFeatureMap map);
    void Clear();
    void Shutdown();

    // 同期焼き (selftest / --modal-bake 用)。ボクセル化 + 推論をこのスレッドで即座に行う。
    // 成功で Ready、失敗で Failed (どちらも cache_ に反映してから返す)
    bool BakeSync(AssetID mesh);

    // dirty (未保存) な .msfm 表を**全部まとめて**書き出す (reviewer round 1 指摘 3 の是正)。
    // UpdateTableEntry は保存を毎回はしない (dirtyTables_ へ積むだけ) ので、これを呼ぶまで
    // ディスクには反映されない。Shutdown() が破棄時に呼ぶほか、--modal-bake がバッチの
    // 終わりに明示的に呼ぶ (モデル単位で表ごと書き直す O(n^2) の I/O を、モデル数ぶんの
    // O(n) へ落とす — 表は「サブメッシュを 1 枚焼くたび全体を書き直す」形式なので、
    // 焼くたびに保存すると 1 モデルの累積書き込みが枚数の 2 乗で増える)。
    // ★この「全部まとめて」だけが flush 点だと、長時間セッション中の異常終了で焼き結果が
    // 全部消える (reviewer round 2 指摘 4) — 定常的な安全網は `UpdateTableEntry` が
    // srcPath ごとに自動で呼ぶ `FlushOneTable` (kFlushEveryNUpdates 件ごと) が受け持つ。
    // こちらは「今すぐ確実に全部書き切りたい」ときの明示操作
    void FlushDirtyTables();

private:
    struct Entry {
        ModalState state = ModalState::Missing;
        ModalFeatureMap map;
    };

    struct Job {
        uint64_t meshId = 0;
        std::string meshName;
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t> indices;
        std::shared_ptr<const DmNet> net; // enqueue 時点の重みを掴む (差し替え中でも一貫させる)
    };
    struct JobResult {
        uint64_t meshId = 0;
        std::string meshName;
        std::shared_ptr<const DmNet> net;
        bool ok = false;
        std::string err;
        ModalFeatureMap map;
        // RunsOnWorkerThread()==false (GPU) のときは、ワーカーはボクセル化だけ済ませて
        // ここへグリッドを渡し、Pump() (メインスレッド) が Infer 以降を仕上げる
        bool needsMainInfer = false;
        modal::VoxelGrid grid;
    };

    void EnsureWorker();
    void WorkerLoop();
    void FinishResult(JobResult&& r);

    using CookTable = std::vector<std::pair<std::string, ModalFeatureMap>>; // key 昇順
    CookTable& LoadTable(const std::wstring& srcPath);
    void SaveTable(const std::wstring& srcPath, const CookTable& table);
    void UpdateTableEntry(const std::wstring& srcPath, const std::string& meshName,
                          const ModalFeatureMap& map);
    // srcPath 1 本だけを flush し、dirty フラグとカウンタを両方リセットする
    // (FlushDirtyTables() の単一テーブル版。自動 flush のトリガから呼ぶ)
    void FlushOneTable(const std::wstring& srcPath);

    RenderResources* resources_ = nullptr;
    std::unique_ptr<ModalInferenceBackend> backend_;
    std::shared_ptr<const DmNet> net_;
    std::wstring modelPath_;

    std::unordered_map<uint64_t, Entry> cache_;
    std::unordered_set<uint64_t> warnedFailed_; // WARN は 1 回だけ
    std::unordered_map<std::wstring, CookTable> tables_;
    std::unordered_set<std::wstring> dirtyTables_; // FlushDirtyTables() で保存する srcPath の集合
    // reviewer round 2 指摘 4 + planner round 2 追補 (規則 2「flush は損失の窓を区切る」):
    // Shutdown()/Clear()/--modal-bake の 3 箇所でしか flush しないと、長時間の Editor
    // セッション中の異常終了で溜まった焼き結果が全部消える。かといって
    // 「N 件たまったら**全部の** dirty テーブルを flush」だと、複数モデルを同時に
    // 焼いているときに無関係なモデルまで巻き込んで書き直す (元の O(n^2) の縮小再生産)。
    // ★**srcPath (= モデルファイル) ごと**にカウンタを持ち、そのモデル自身の更新が
    // kFlushEveryNUpdates 件たまったらそのモデルの表**だけ**を flush する —
    // 「落ちても失うのは 1 モデルにつき高々 N-1 件」という損失の単位をモデル境界に揃える
    std::unordered_map<std::wstring, int> dirtyUpdateCounts_;
    static constexpr int kFlushEveryNUpdates = 20;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobQueue_;
    std::deque<JobResult> doneQueue_;
    bool workerStarted_ = false;
    bool workerStop_ = false;
    bool busy_ = false; // ワーカーがジョブ処理中か (ReloadModel のドレイン待ちに使う)
};

// モジュール注入 (convexcol:: / physmat:: と同じ流儀)。EngineLoop が起動時に Install し
// 終了時に外す。AudioSourceSystem / Inspector プレビューが実体解決に使う。メインスレッド専用
namespace modalsound {
void Install(ModalSoundLibrary* lib);
ModalSoundLibrary* Library();
bool IsReady(AssetID mesh); // 未接続/未登録は false
const DmNetHeader* Header(); // 未接続/モデル未ロードは nullptr
} // namespace modalsound

// 「プロジェクト assets\deepmodal → エンジン assets\deepmodal」の 2 ルートで
// deepmodal.dmnet を解決する (FindEngineShaderDir と同じ二経路規則)。
// どちらにも無ければ空 (LoadModel が失敗して NoModel のまま起動を続ける)
std::wstring ResolveDeepModalPath(const std::wstring& assetsRoot);

} // namespace mye
