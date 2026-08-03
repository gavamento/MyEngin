#pragma once
#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/MusicMath.h"

// XAudio2 インターフェイスは .cpp でのみ完全定義を使う (AudioSystem.h と同じ流儀)
struct IXAudio2;
struct IXAudio2SubmixVoice;
struct stb_vorbis; // external/stb/stb_vorbis.c (マクロ設定は Audio/StbVorbis.h が単一情報源)

namespace mye {

// ストリーミング元。**.wav = ディスクから逐次読み / .ogg = 圧縮バイト列を持って逐次デコード**。
//
// ogg を圧縮のままメモリに置くのは、展開後 PCM が 10 倍前後に膨らむため
// (5MB の ogg → 50MB の PCM)。「全展開しない」ことがストリーミングの本体なので、
// wav はファイルハンドルを開いたまま必要なフレームだけ seek + read する。
//
// **XAudio2 にもスレッドにも依存しない**ので、selftest が「全展開デコーダ (DecodeWav) と
// サンプル単位で一致するか」をデバイス無しで検証できる。
class MusicSource {
public:
    MusicSource() = default;
    ~MusicSource() { Close(); }
    MusicSource(const MusicSource&) = delete;
    MusicSource& operator=(const MusicSource&) = delete;

    bool Open(const std::wstring& path); // 失敗時は Close 済みの状態に戻る
    void Close();
    bool IsOpen() const { return channels_ != 0; }

    uint16_t Channels() const { return channels_; }
    uint32_t SampleRate() const { return sampleRate_; }
    int64_t TotalFrames() const { return totalFrames_; }

    bool Seek(int64_t frame);
    // frames フレームを 16bit インターリーブで dst へ。戻り値 = 実際に読めたフレーム数
    int64_t Read(int16_t* dst, int64_t frames);

private:
    int64_t ReadWav(int16_t* dst, int64_t frames);
    int64_t ReadOgg(int16_t* dst, int64_t frames);

    uint16_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    int64_t totalFrames_ = 0;
    int64_t pos_ = 0; // 次に読むフレーム位置

    // wav
    std::ifstream file_;
    WavInfo info_;
    std::vector<uint8_t> scratch_; // 生バイト → 16bit 変換の中継 (ブロック 1 枚ぶん)

    // ogg
    stb_vorbis* vorbis_ = nullptr;
    std::vector<uint8_t> oggBytes_;
    std::vector<char> arena_; // stb_vorbis のアリーナ (alloca 回避。MusicStream.cpp の注記参照)
};

// BGM 1 曲ぶんの再生指定 (M45f)。SE と違い **クリップ表 (PCM 全展開) には載せない** —
// ディスク / 圧縮バイト列から逐次デコードして鳴らす
struct MusicRequest {
    std::wstring path;          // .wav / .ogg の実ファイル
    uint64_t key = 0;           // 同一判定用の識別子 (.sound.json の GUID)。0 = 常に別曲扱い
    float volume = 1.0f;        // 線形 0..1 (バス音量とは別に掛かる)
    float fadeSeconds = 1.0f;   // 0 以下 = 即時切替
    bool loop = true;
    int64_t loopStartFrame = 0; // .sound.json の loopStartSample (フレーム単位)
    int64_t loopEndFrame = 0;   // 同 loopEndSample。end <= start は「末尾まで」
};

// BGM ストリーマ。**ワーカースレッド 1 本 + スロット 2 個 (A/B = クロスフェード用)**。
//
// スレッド分担 (ここを崩すと即クラッシュするので変えないこと):
//   - メインスレッド: voice の生成/破棄、ファイルを開く/閉じる、Start/Stop、ゲイン (フェード)
//   - ワーカー      : リングの補充 (デコード + SubmitSourceBuffer) のみ
//   - オーディオスレッド (XAudio2 のコールバック): **SetEvent 1 発だけ**。
//     ここから SubmitSourceBuffer / Stop / GetState / DestroyVoice を呼ぶと自己デッドロックする
//   - ワーカーは ECS も World も一切読まない (決定論契約 7)
//
// 排他は mu_ 1 本 (スロット状態と voice ポインタの両方を守る)。
// **コールバックは mu_ を取らない**ので、mu_ を持ったまま DestroyVoice を呼んでも
// デッドロックしない (DestroyVoice はオーディオスレッドが voice を離すまでブロックする)。
//
// 不変条件: コールバック実体と PCM リングは DestroyVoice() より長生きすること。
// → スロットは std::array<unique_ptr<Slot>> で持つ (**再確保で move する std::vector は禁止**。
//   オーディオスレッドが読んでいるバッファの足元が消える)。
class MusicStreamer {
public:
    static constexpr int kSlots = 2;

    // ★コンストラクタ/デストラクタとも **定義は .cpp** に置くこと。Slot が不完全型のまま
    //   ここで = default すると unique_ptr の削除子が実体化できずコンパイルが通らない
    MusicStreamer();
    ~MusicStreamer();
    MusicStreamer(const MusicStreamer&) = delete;
    MusicStreamer& operator=(const MusicStreamer&) = delete;

    void Init(IXAudio2* xaudio); // ワーカー起動。xaudio が null なら全 API が no-op
    // ★破棄順: ワーカーを殺し切る → Stop/Flush → DestroyVoice → リング/コールバック解放
    void Shutdown();
    bool Ready() const { return xaudio_ != nullptr; }

    // dest = 送り先サブミックス (BGM バス)。**同じ key が既に鳴っていれば何もせず true**
    // (シーン遷移で同じ BGM が指定されたときに曲を頭出しし直さないため = BGM 引き継ぎ)
    bool Play(const MusicRequest& r, IXAudio2SubmixVoice* dest);
    void Stop(float fadeSeconds); // フェードアウト (0 以下で即時)
    // 即時停止 + voice 破棄。**バスグラフ再構築と suspend の前に必ず呼ぶ** —
    // 送り先サブミックスが消えるので voice を残してはいけない
    void StopNow();

    // メインスレッド。フェード進行と、鳴り終わった/フェードし切ったスロットの回収。
    // dt = 実時間秒 (フェードは絶対経過時間で進める)
    void Update(float dt);

    // エディタ UI 用。**ABI へは絶対に出さない** (決定論契約 5: 再生状態を sim へ戻さない)
    bool Playing() const;
    uint64_t CurrentKey() const;

private:
    struct Slot; // MusicStream.cpp で定義 (IXAudio2VoiceCallback の実体を含むため)

    int AcquireSlot();                  // mu_ 保持下で呼ぶこと
    void ReleaseSlot(Slot& s);          // 同上。voice を止めて破棄しファイルも閉じる
    void FillSlot(Slot& s);             // 同上。リングを埋めて submit する
    void WorkerLoop();

    std::array<std::unique_ptr<Slot>, kSlots> slots_{};
    mutable std::mutex mu_;
    std::thread worker_;
    // HANDLE。<Windows.h> をヘッダへ引き込まないため void* で持つ
    void* bufferEnd_ = nullptr; // 自動リセット。コールバックが SetEvent するだけ
    void* quit_ = nullptr;      // 手動リセット。ワーカー停止
    IXAudio2* xaudio_ = nullptr;
};

} // namespace mye
