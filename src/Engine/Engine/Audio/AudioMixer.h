#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Engine/Audio/VoicePolicy.h" // kMinDb / DbToLinear / LinearToDb (M45a)

namespace mye {

// dB フェーダの上端。下端は VoicePolicy.h の kMinDb (= -∞ 扱いの無音)
inline constexpr float kMaxBusDb = 12.0f;

// I3DL2 リバーブプリセット。**.mixer.json には名前で保存する** — 実パラメータ表は
// AudioSystem.cpp 側にある (xaudio2fx.h が要るため) が、名前で持てば表の並びを
// 変えても既存ファイルが壊れない。Default は Room=-10000 = 実質リバーブ無し
inline constexpr int kReverbPresetCount = 11;
const char* ReverbPresetName(int index); // 範囲外は "Default"
int ReverbPresetIndex(const char* name); // 未知名は 0 (Default)

// ミキサーバス 1 本の編集データ。ランタイム (AudioSystem のサブミックス) はこれを写す。
// **決定論レーン外** — sim はミキサーの値を一切読まない
struct MixerBus {
    std::string name;      // 空不可・大文字小文字を無視して一意
    std::string parent;    // 空 = ルート (mastering voice 直下)。ルートは 1 本だけ
    float volumeDb = 0.0f; // kMinDb で無音
    bool mute = false;
    bool solo = false;
    // リバーブバスへの送り量 (0..1)。**ルートバスは常に 0** —
    // リバーブの出力先がルートなので、ルートから送ると閉路になる
    float reverbSend = 0.0f;
};

// .mixer.json 1 件 (SoundAsset / ControllerAsset と同じ流儀)
struct MixerAsset {
    uint64_t hash = 0; // = GUID (MixerLibrary のキー)
    std::string name;
    std::wstring path;

    std::vector<MixerBus> buses;
    std::string reverbPreset = "Default"; // ReverbPresetIndex で解決
    float reverbWetDryMix = 100.0f;       // リバーブ「バス」は 100% wet が既定 (送り量で調整する)
};

// 列挙 1 件 (Asset Browser / ミキサー窓のアセット選択用)
struct MixerEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .mixer.json の管理 (SoundLibrary 範型)。
// **アクティブなミキサーは 1 本だけ** — AudioSystem のバスグラフはグローバルに 1 つなので、
// 「どれが今鳴っているか」をライブラリ側で持つ
class MixerLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, MixerAsset asset);
    bool SaveToFile(uint64_t hash) const;

    const MixerAsset* Get(uint64_t hash) const;
    MixerAsset* GetMutable(uint64_t hash);
    bool Contains(uint64_t hash) const { return mixers_.find(hash) != mixers_.end(); }
    std::vector<MixerEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    uint64_t ActiveHash() const { return active_; }
    void SetActive(uint64_t hash) { active_ = hash; }
    // 起動時に「どれを鳴らすか」を決める: stem が "default" のものを優先し、
    // 無ければ名前順の先頭。1 本も無ければ 0
    uint64_t PickStartupMixer() const;

    static nlohmann::json ToJson(const MixerAsset& m);
    static bool FromJson(const nlohmann::json& j, MixerAsset& out);

private:
    std::unordered_map<uint64_t, MixerAsset> mixers_;
    uint64_t active_ = 0;
};

// ---- 純関数 (selftest がデバイス無しで叩ける) ----

// 大文字小文字を無視した名前引き。見つからなければ -1
int FindMixerBus(const MixerAsset& m, const char* name);

// 各バスの親インデックス。**-1 = ルート (parent 名が空) / -2 = 親名が解決できない (孤児)**
std::vector<int> MixerBusParents(const MixerAsset& m);

// ルートからの深さ。孤児・循環に巻き込まれたバスは -1
std::vector<int> MixerBusDepths(const MixerAsset& m);

// トポロジ検証: バスが 1 本以上 / 名前が非空かつ一意 / ルートがちょうど 1 本 /
// 親が全て解決できる / 循環なし。error は最初の違反だけ書く (null 可)
bool ValidateMixer(const MixerAsset& m, std::string* error);

// ソロを考慮した実効ミュート。ソロが 1 つでも立っていれば
// 「ソロバス自身 + その祖先 + その子孫」以外を落とす
// (祖先を残さないとソロにした音がマスターまで届かない)
std::vector<uint8_t> MixerEffectiveMutes(const MixerAsset& m);

// 既定ミキサー (Master / BGM / SE / UI)。AudioSystem の初期バス構成でもある
MixerAsset DefaultMixer();

} // namespace mye
