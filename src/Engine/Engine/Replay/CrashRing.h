#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Platform/Input.h"

namespace mye {

// クラッシュ .rep のリング (M52f、決定台帳 2)。
//
// 「直近のスナップショット 1 枚 + そこから今までの全 tick 入力」を**常に .rep の
// バイト列そのものの形で**保持する。落ちた瞬間にハンドラがやるのは
// 「このバイト列を WriteFile する」だけ — 確保もロックも直列化も走らない。
//
// ★M52e の TimeTravel とは**別インスタンス・別方針**:
//     TimeTravel … 多数のスナップショットを持ち、任意の過去へ戻れる (エディタの巻き戻し)
//     CrashRing  … スナップショット 1 枚だけ。戻る先は要らず、必要なのは
//                  「落ちる直前まで再現できる 1 本の .rep」
//   TimeTravel の器をそのまま使わないのは、ハンドラ内で std::vector を舐めて .rep を
//   組み立てる余裕が無いため (= 事前に組んでおく方針そのものが違う)。
//
// ★入力は tick に**入る前**に載せる。落ちるのは tick の中なので、tick 末まで待つと
//   「まさに落ちた tick の入力」が .rep に残らず、再生してもその tick へ入れない。
//   その tick のハッシュはまだ無いので **0 = 期待値なし (未完了)** を書いておき、
//   tick が走り切ったら実ハッシュで上書きする (8 バイト整列ストア = 破れない)。
//   0 の意味は Replay.h に予約として明記してある。
struct CrashRingConfig {
    // 何 tick ごとにスナップショットを撮り直すか。撮り直すたびにレコードは 0 本に戻る。
    // 短くするほど .rep は小さくなるが撮影が増える (Release 実測 0.040ms/枚)
    uint64_t snapshotInterval = 600;
    size_t maxTicks = 720; // レコード上限 (安全余裕。到達したら次の境界で撮り直す)
    // 入力レーン数 (M52g)。**レコード長を決める**ので Begin より前に確定していること。
    // チューニング値ではなく実行の性質だが、ここに置くと「撮り直しのたびに読み直す」
    // 1 経路で済む (別 setter だと Begin 前後で食い違う窓ができる)
    uint32_t playerCount = 1;
};

class CrashRing {
public:
    // レコード 1 本 = 入力 (playerCount 本ぶん) + tick 末ハッシュ。
    // ★**撮影時に固めた値**を返す (config_ を直接読まない) — 撮影後に Configure で
    //   レーン数が変わると、イメージ内のヘッダ (= 実際のレイアウト) と食い違って
    //   レコードの読み書き位置がずれる。取り直すまでは撮影時の形が正しい
    size_t RecordBytes() const { return recordBytes_; }

    void Configure(const CrashRingConfig& c) { config_ = c; }
    const CrashRingConfig& Config() const { return config_; }

    void SetEnabled(bool on) { enabled_ = on; }
    bool Enabled() const { return enabled_; }

    // tick 境界 (構造変更が空) で 1 枚目を撮って記録を始める。撮影に失敗したら無効化する
    bool Begin(const SimRefs& refs, uint64_t tick);

    // tick 本体を呼ぶ**直前**。その tick が消費する入力レーンを先に載せる
    // (inputs は playerCount 本の配列)
    void OnTickBegin(uint64_t tick, const InputSnapshot* inputs, uint32_t playerCount);
    // tick が走り切った直後。in-flight レコードのハッシュを確定し、必要なら撮り直す
    void OnTickEnd(const SimRefs& refs, uint64_t ranTick, uint64_t hashAfter);

    // tick 列を tick まで巻き戻す (M52i)。**ロールバック再シムの直前に呼ぶ**。
    // ★これが無いと、再シムの OnTickBegin が「tick 列が飛んだ」と判定して毎回
    //   撮り直しになり、リングが常に 1〜2 tick しか持たない .rep へ痩せる
    //   (実測: ネット対戦中の crash.rep / desync バンドルが役に立たなくなる)。
    //   保持しているスナップショット 1 枚の tick 以降なら、レコード本数を切り詰める
    //   だけで整合するので撮り直しは要らない
    void Rewind(uint64_t tick);

    // ---- ここから下はクラッシュハンドラの中から呼ばれる (確保もロックもしない) ----
    // 完成済みの .rep イメージ。撮影中 (= 一貫していない) なら nullptr
    const std::byte* RepImage(size_t& outSize) const;
    bool WriteRepFile(const wchar_t* path) const;

    // ---- 参照 (ログ / セルフテスト用) ----
    uint64_t SnapshotTick() const { return snapshotTick_; }
    uint64_t RecordCount() const;
    size_t SnapshotBytes() const { return snapshotBytes_; }
    size_t ImageBytes() const;
    uint64_t SnapshotCount() const { return snapshotCount_; }
    bool InFlight() const { return inFlight_; }

private:
    bool TakeSnapshot(const SimRefs& refs, uint64_t tick);
    std::byte* RecordAt(uint64_t index);

    CrashRingConfig config_;
    bool enabled_ = false;
    // ★「撮影中」は .rep として一貫していない。ハンドラはこのフラグだけを見て、
    //   中途半端なイメージを書き出さない (= 壊れた .rep を渡さない)
    bool ready_ = false;
    bool inFlight_ = false;
    uint64_t snapshotTick_ = 0;
    uint64_t nextTick_ = 0; // 次に来るはずの tick。ズレたら撮り直す (シーク / シーン跨ぎ)
    uint64_t ticksSinceSnapshot_ = 0;
    uint64_t snapshotCount_ = 0;
    size_t snapshotBytes_ = 0;
    size_t recordBytes_ = sizeof(InputSnapshot) + sizeof(uint64_t); // 撮影時に確定 (M52g)
    size_t recordBase_ = 0;               // レコード 0 本目のオフセット
    std::vector<std::byte> image_;        // .rep そのもののバイト列
    std::vector<std::byte> scratch_;      // 撮影の作業領域 (tick 境界でしか触らない)
};

// クラッシュハンドラへ渡す差し込み口の実引数 (CrashPayloadFn の user)。
// **ハンドラ内で参照される**ので、生きているオブジェクトを指したまま保つこと
struct CrashPayload {
    const CrashRing* ring = nullptr;
    // 元シーンの絶対パス (コードから組んだシーンは空)。std::wstring を持たないのは
    // ハンドラ内で再確保済みのバッファを掴む事故を避けるため — 毎フレーム写す
    wchar_t sceneSource[520] = {};
};

// CrashPayloadFn の実体: crash.rep と scene.json を書く
void WriteCrashPayload(void* user, const wchar_t* bundleDir);

} // namespace mye
