#pragma once
#include <cstdint>
#include <vector>

// 軽量プロファイラ (M12)。決定論規約: 計測値は sim 状態でもワールドハッシュでもないため
// リプレイに影響しない。スコープ記録はフレーム毎に固定領域へ貯める (tick 中の追加確保なし)。
namespace mye::prof {

// スレッド: スコープ・レンダ統計・履歴はメインスレッド専用 (ロックを持たない)。
// ジョブのワーカーから呼ばない。
//
// ---- CPU 階層スコープ計測 ----
// 並びは開始順 (親 → 子)。name は呼び出し側が寿命を保証する文字列 (リテラル、または
// フレームをまたいでアドレスが変わらない文字列)。
struct ScopeRecord {
    const char* name;
    float ms;   // 閉じるまでは 0
    int depth;
    int parent; // FrameScopes() 内の親の添字。ルートは -1
};

// 毎フレーム先頭で呼ぶ。前フレームを確定して履歴へ積み、今フレームの記録を空にする
void BeginFrame();
void PushScope(const char* name); // 手動 (通常は MYE_PROFILE_SCOPE を使う)
void PopScope();
// 今フレームの生の記録 (同名を合算しない)。フレームの途中で読むと、開いているスコープは 0 ms
const std::vector<ScopeRecord>& FrameScopes();

// ---- 確定済みフレームの履歴 ----
// 表示は「確定済みフレーム」だけを読む。フレームの途中の値を混ぜないので、フェーズ時間・
// スコープ・レンダ統計の時点が揃う (GPU 時間だけはクエリの読み出し遅延ぶん古い)。
inline constexpr int kFrameHistory = 240;     // 保持するフレーム数 (60 fps で 4 秒)
inline constexpr int kMaxFrameScopes = 192;   // 1 フレームの合算後スコープ数の上限
inline constexpr int kMaxRawScopes = 4096;    // 1 フレームの生の記録数の上限 (超過分は捨てて数える)

// 同じ親の下の同名スコープを 1 行へ合算したもの (1 フレームに複数 tick 回ると同名が並ぶため)
struct ScopeStat {
    const char* name = nullptr;
    float ms = 0.0f; // 合計
    int calls = 0;   // 合算した回数
    int depth = 0;
    int parent = -1; // FrameRecord::scopes 内の親の添字。ルートは -1。親は必ず自分より前にある
};

// フレームの壁時計の内訳。EngineLoop が SetFramePhases で渡す。
// frameMs だけは BeginFrame が自分で測る (BeginFrame から次の BeginFrame まで)
struct FramePhases {
    float reloadMs = 0.0f;  // フェーズ 2 (ホットリロード)
    float tickMs = 0.0f;    // 全 tick (フェーズ 3-5,7)
    float renderMs = 0.0f;  // フェーズ 6 (シーン描画の CPU 側)
    float imguiMs = 0.0f;   // フェーズ 8 の ImGui 構築と描画
    float presentMs = 0.0f; // Present 呼び出し (vsync 待ちを含む)
    int ticks = 0;
};

// フレームの末尾で呼ぶ。次の BeginFrame が確定するフレームへ付く (呼ばなければ 0 のまま)
void SetFramePhases(const FramePhases& phases, uint64_t frameIndex, uint64_t tickIndex);

// ---- GPU パスの時間 ----
// 描画側が毎フレーム「名前・時間・今フレームに走ったか」を申告する (表示側がパスごとの
// 有効条件を知らずに済む)。ms は GPU クエリの読み出し遅延ぶん古い値。
// ran = false のパスは時間が前回値のまま残っているので、表示は灰色にして合計へ入れない。
// 同じ名前を 1 フレームに 2 度申告したら後勝ち
inline constexpr int kMaxGpuPasses = 48;
struct GpuStat {
    const char* name = nullptr;
    float ms = 0.0f;
    bool ran = false;
};
void ReportGpu(const char* name, float ms, bool ran);
// Render 1 回ぶん全体の GPU 時間 (複数ビューでは最後に描いたビュー)。パスの合計との差が未計測分
void ReportGpuFrame(float ms);

// 履歴の確定を止める / 再開する (表示の一時停止用)。止めている間のフレームは履歴へ積まない
void SetHistoryPaused(bool paused);
bool IsHistoryPaused();

// ---- 項目別の計測 (スクリプト別・エンティティ別) ----
// スコープは「どのシステムが重いか」まで。こちらは 1 件ずつ処理する直列ループの中で
// 「どのスクリプト型 / どのエンティティが重いか」を測る。呼び出しごとに時計を 2 回読むので、
// 既定では止めてある (SetItemTimingEnabled)。止まっている間のコストは分岐 1 つ。
// 集計は 2 つ:
//   名前別     = (分類, 名前) ごとの合計と回数。上限を超えない限り正確
//   エンティティ別 = (分類, 名前, エンティティ) ごと。上限を超えた組は捨てて数える
// category はリテラル。name は呼び出しの間だけ生きていればよい (prof が写しを持つ)。
// ホットリロードでスクリプト型の文字列が解放されても、履歴の中の名前は壊れない
inline constexpr int kMaxFrameItems = 256;     // エンティティ別の行数の上限
inline constexpr int kMaxFrameItemNames = 64;  // 名前別の行数の上限
struct ItemStat {
    const char* category = nullptr;
    const char* name = nullptr; // prof が持つ写し (プロセス終了まで有効)
    uint32_t entityIndex = 0;   // 名前別の行では 0
    uint32_t entityGeneration = 0;
    float ms = 0.0f;
    int calls = 0;
};
void SetItemTimingEnabled(bool enabled);
bool ItemTimingEnabled();
void AddItemSample(const char* category, const char* name, uint32_t entityIndex, uint32_t entityGeneration,
                   float ms);

// 1 件ぶんの処理を囲む。計測が止まっていれば何もしない
class ItemScope {
public:
    ItemScope(const char* category, const char* name, uint32_t entityIndex, uint32_t entityGeneration);
    ~ItemScope();
    ItemScope(const ItemScope&) = delete;
    ItemScope& operator=(const ItemScope&) = delete;

private:
    const char* category_;
    const char* name_;
    uint32_t entityIndex_;
    uint32_t entityGeneration_;
    int64_t start_; // 0 = 計測していない
};

// ---- 重いフレームの自動記録 ----
// 確定したフレームの壁時計がしきい値を超えたら、そのフレームを丸ごと控えへ写す。
// 履歴は 4 秒で流れてしまうので、たまにしか起きない引っかかりを後から開けるようにする。
// 控えは kMaxCaptures 件のリングで、あふれたら古いものから捨てる
inline constexpr int kMaxCaptures = 32;
void SetCaptureThresholdMs(float ms); // 0 以下 = 記録しない
float CaptureThresholdMs();
void ClearCaptures();

struct FrameRecord;
int CaptureCount();
const FrameRecord& Capture(int index);      // 0 = いちばん古い控え。範囲外は空のレコード
int HistoryCount();                         // 確定済みフレーム数 (最大 kFrameHistory)
const FrameRecord& History(int age);        // age 0 = 直近の確定フレーム。範囲外は空のレコード

struct ScopeTimer {
    explicit ScopeTimer(const char* n) { PushScope(n); }
    ~ScopeTimer() { PopScope(); }
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
};

// ---- レンダ統計 (ドローコール / 三角形 / カリング) ----
// 影の draw は本描画の欄に混ぜない。ビュー別の集計 (SetRenderStatsView) の和が累積値と一致する
inline constexpr int kRenderStatsViewSlots = 4; // viewKey 0..3 (RenderSystem の viewKey 規約)
inline constexpr int kRenderStatsLodSlots = 4;     // LOD0 + 3 段
inline constexpr int kRenderStatsCascadeSlots = 3; // ShadowPass::kCascades と同数

struct RenderStats {
    int drawCalls = 0;
    int triangles = 0;
    int culled = 0; // フラスタムカリングで除外したメッシュ数 (M16)
    // 影 (CSM + 局所影アトラス)。cascadeDraws は CSM のカスケード別
    int shadowDrawCalls = 0;
    int shadowTriangles = 0;
    int shadowCascadeDraws[kRenderStatsCascadeSlots] = {};
    // CSM のキャスター: カスケード別のカリングに掛けた候補数と、カスケードごとに残った数
    int shadowCasterCandidates = 0;
    int shadowCascadeCasters[kRenderStatsCascadeSlots] = {};
    // メッシュ LOD 段ごとの描画したオブジェクト数と三角形数 (本描画のみ。インスタンシングは個数で数える)
    int lodDraws[kRenderStatsLodSlots] = {};
    int lodTriangles[kRenderStatsLodSlots] = {};
    // 以降は後続サブが埋める欄 (欄と dump の形を先に固定してある)
    int paletteEvaluated = 0;
    int paletteReused = 0;
    int occlusionPhase1Draws = 0;
    int occlusionPhase2Draws = 0;
    int occluded = 0;
    // カリングの理由別。culled は「視錐台で落としたメッシュ」のまま (意味を変えない)
    int culledDistance = 0;       // 距離カリングで落としたメッシュ
    int culledScreenSize = 0;     // 画面サイズカリングで落としたメッシュ
    int culledNonMesh = 0;        // メッシュ以外の経路 (パーティクル・トレイル・ビルボード・デカール・2D) を視錐台で落とした項目
    int unculledGpuEmitters = 0;  // 上界を持てず判定しなかった GPU エミッタ
};
// 描画パスの DrawIndexed 地点で呼ぶ。triangles は呼び出し全体 (インスタンシングは三角形 x 個数)、
// lod は描いた LOD 段、instances はその呼び出しで描いたオブジェクト数
void AddDraw(int triangles, int lod = 0, int instances = 1);
void AddCulled(int n);                         // 収集時にカリングした件数を加算 (M16)
void AddShadowDraw(int triangles, int cascade = -1); // 影の DrawIndexed 地点。cascade < 0 = CSM 以外
void AddRenderStats(const RenderStats& delta); // 任意の欄をまとめて加算 (後続サブの欄用)
// 以降の加算を viewKey 別の集計にも入れる (RenderSystem::Render の先頭で呼ぶ)。範囲外は 0 番
void SetRenderStatsView(uint32_t viewKey);
RenderStats GetRenderStats(); // 従来の累積値 (全ビューの和)
RenderStats GetRenderStatsForView(uint32_t viewKey);

// 確定済みの 1 フレーム。履歴のリングに固定長で持つ (確定時に確保しない)
struct FrameRecord {
    uint64_t frameIndex = 0;
    uint64_t tickIndex = 0;
    float frameMs = 0.0f; // 壁時計 (BeginFrame の間隔)
    FramePhases phases;
    int scopeCount = 0;
    int droppedScopes = 0; // 上限 (kMaxRawScopes / kMaxFrameScopes) を超えて捨てた記録数
    ScopeStat scopes[kMaxFrameScopes];
    int itemCount = 0;
    int itemNameCount = 0;
    int droppedItems = 0; // エンティティ別の上限を超えて捨てた組の数 (名前別の合計には入っている)
    ItemStat items[kMaxFrameItems];
    ItemStat itemNames[kMaxFrameItemNames];
    float gpuFrameMs = 0.0f;
    int gpuCount = 0;
    GpuStat gpu[kMaxGpuPasses];
    RenderStats render;                              // 全ビューの和
    RenderStats renderByView[kRenderStatsViewSlots]; // viewKey 別
};

// ---- メモリ (MemoryTrack.cpp の global operator new/delete フック) ----
// 確保を「いま何の処理中か」で分ける札。スレッドごとに持ち、確保した側のスレッドの札へ数える
// (ワーカースレッドは既定の General のまま)。解放はどの札の確保だったか分からないので、
// 札別に出せるのは確保の回数とバイト数だけ (札別の生存量は出せない)
enum class MemTag : uint8_t {
    General,  // 下のどれでもない
    Asset,    // フェーズ 2: ホットリロードと非同期ロードの取り込み
    Tick,     // 固定 tick (物理・スクリプト以外)
    Physics,
    Script,   // C++ / C# スクリプトの呼び出し (GameLogic.dll 内の確保は別ヒープなので数に入らない)
    Render,   // フェーズ 6: シーン描画の CPU 側
    EditorUi, // フェーズ 8: ImGui
    Count
};
inline constexpr int kMemTagCount = static_cast<int>(MemTag::Count);

namespace detail {
// 札の実体は Profiler.cpp に置く。MemoryTrack.cpp に置くと、札を立てるだけのコード
// (EngineLoop / TickRunner) が new/delete のフックまでリンクへ引き込んでしまい、
// フックを使っていない実行ファイル (Runtime / Server) の確保経路が変わる
extern thread_local MemTag t_memTag;
} // namespace detail

inline MemTag CurrentMemTag() { return detail::t_memTag; }
inline void SetMemTag(MemTag tag) { detail::t_memTag = tag; }

// スコープの間だけ札を差し替え、抜けるとき元へ戻す
class MemTagScope {
public:
    explicit MemTagScope(MemTag tag) : previous_(detail::t_memTag) { detail::t_memTag = tag; }
    ~MemTagScope() { detail::t_memTag = previous_; }
    MemTagScope(const MemTagScope&) = delete;
    MemTagScope& operator=(const MemTagScope&) = delete;

private:
    MemTag previous_;
};

struct MemStats {
    uint64_t totalAllocs = 0; // 累計確保回数
    uint64_t totalFrees = 0;  // 累計解放回数
    uint64_t liveAllocs = 0;  // 生存中 (allocs - frees)
    uint64_t totalBytes = 0;  // 累計確保バイト数 (要求サイズ)
    uint64_t liveBytes = 0;   // 生存中のバイト数 (ヒープが実際に割り当てたブロックサイズの和)
    uint64_t tagAllocs[kMemTagCount] = {}; // 札別の累計確保回数
    uint64_t tagBytes[kMemTagCount] = {};  // 札別の累計確保バイト数 (要求サイズ)
};
MemStats GetMemoryStats();

} // namespace mye::prof

#define MYE_PROF_CAT2(a, b) a##b
#define MYE_PROF_CAT(a, b) MYE_PROF_CAT2(a, b)
#define MYE_PROFILE_SCOPE(name) ::mye::prof::ScopeTimer MYE_PROF_CAT(prof_scope_, __LINE__)(name)
