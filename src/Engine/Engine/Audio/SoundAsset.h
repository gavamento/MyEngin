#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Engine/Audio/AudioSystem.h"

namespace mye {

// 距離減衰カーブ (M45e の X3DAudio 側で実際に使う。ここでは .sound.json の保持だけ)
enum class SoundRolloff : int32_t { Logarithmic = 0, Linear = 1, Inverse = 2 };

// 1 バリエーション = 1 クリップ + 抽選重み。同じ音の言い回し違いを 1 アセットに束ねる
struct SoundVariation {
    // 解決済みクリップ = GUID (AudioSystem のクリップキーそのもの)。
    // 旧形式 (文字列パス) は clipPath に読むだけで、解決は SoundLibrary::LoadFromFile が
    // .sound.json のディレクトリ相対で行う (.mat.json / .controller.json と同じ M39a 流儀)
    uint64_t clip = 0;
    std::string clipPath;
    int32_t weight = 1; // 0 以下は候補外
};

// .sound.json 1 件。**再生パラメータのデータ化**であって ECS コンポーネントではない
// (AudioSource コンポーネントは M45e)。決定論レーン外なので float を持ってよい
struct SoundAsset {
    uint64_t hash = 0; // = GUID (SoundLibrary のキー)
    std::string name;
    std::wstring path;

    std::vector<SoundVariation> variations;

    // ---- 2D 再生パラメータ ----
    float volume = 1.0f;       // 線形 0..1
    float volumeRandom = 0.0f; // ± 幅 (線形)
    float pitch = 1.0f;        // 周波数比
    float pitchRandom = 0.0f;  // ± 幅 (周波数比)
    bool loop = false;
    // BGM フラグ。true なら**ボイスプールではなくストリーミングレーン**で鳴る (M45f)。
    // PCM 全展開もされない (DemoContent::RegisterAssetLibraries の ClipUsage 判定)
    bool stream = false;
    std::string bus = "SE";    // 実行中のミキサーの名前で解決 (未知名は AudioSystem の既定バス)
    int32_t priority = 128;    // 大きいほど重要 (VoicePolicy と同じ規約)
    int32_t maxInstances = 0;  // 同時発音数の上限 (0 = 無制限)

    // ---- 3D 設定 (M45e で実際に効く) ----
    float spatialBlend = 0.0f; // 0 = 2D、1 = フル 3D
    float minDistance = 1.0f;  // これより近ければ減衰なし
    float maxDistance = 50.0f;
    SoundRolloff rolloff = SoundRolloff::Logarithmic;
    float dopplerScale = 1.0f;
    float reverbSend = 0.0f;   // リバーブバスへの送り量 (0 = ドライのみ)

    // ---- ループ点 (単位 = フレーム。end<=start で「末尾まで」)。stream + loop で効く ----
    int32_t loopStartSample = 0;
    int32_t loopEndSample = 0;
};

// 列挙 1 件 (AssetRef ピッカー / Asset Browser 用。ControllerEntry 範型)
struct SoundEntry {
    uint64_t hash = 0;
    std::string name;
};

// あるクリップ GUID が .sound.json からどう参照されているか (M45f)。
// **BGM を PCM 全展開しない**ための判定に使う — 数分の曲を展開すると数十 MB になり、
// ストリーミングの意味が無くなる
enum class ClipUsage : int32_t {
    None = 0,   // どの .sound.json からも参照されていない (従来どおり展開する)
    Sampled,    // 1 つでも stream=false から参照されている (SE として展開が要る)
    StreamOnly, // 参照元が **すべて** stream=true (展開せずストリーミングだけで足りる)
};

// 登録済み .sound.json の管理 (ControllerLibrary 範型)。
// **クリップの実体 (PCM) は持たない** — 参照先は AudioSystem のクリップ表 (GUID キー)
class SoundLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // clipPath を解決して clip を埋める。失敗時 0
    uint64_t Register(const std::wstring& path, SoundAsset asset); // 返り値 = hash
    bool SaveToFile(uint64_t hash) const;

    const SoundAsset* Get(uint64_t hash) const;
    SoundAsset* GetMutable(uint64_t hash);
    // 名前キー (= HashStr(stem)) → GUID。引けなければ key を GUID 直指定とみなす。
    // **AudioSystem::ResolveClipKey と同じ流儀** — スクリプトが "footstep" のような
    // 人間が書ける名前で鳴らせるようにするための入口。未知のキーは 0
    uint64_t ResolveKey(uint64_t key) const;
    bool Contains(uint64_t hash) const { return sounds_.find(hash) != sounds_.end(); }
    std::vector<SoundEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)
    // 「この .wav/.ogg を PCM へ展開する必要があるか」の判定 (M45f)。
    // **1 つでも stream=false の参照があれば Sampled** — 同じ素材を SE と BGM の両方で
    // 使っているときに、SE 側が黙って鳴らなくなるのを防ぐ
    ClipUsage UsageOfClip(uint64_t clip) const;

    static nlohmann::json ToJson(const SoundAsset& s);
    // "clip" は両対応: 数値 = GUID をそのまま / 文字列 = 旧相対パス (clipPath に読むだけ)
    static bool FromJson(const nlohmann::json& j, SoundAsset& out);

private:
    std::unordered_map<uint64_t, SoundAsset> sounds_;
    // 名前キー → GUID。挿入口は Register 1 箇所だけなので同期がずれない
    std::unordered_map<uint64_t, uint64_t> named_;
};

// スクリプト v8 (M45g) が渡す soundKey の解決結果。**解決規則の実装は 1 本だけ** —
// drain と selftest が同じ関数を見るようにする (M45d でランタイムとアセット編集に
// 同じ規則を二重実装して「テストが見ているコードと鳴らしているコードが別物」になった前例)
struct ResolvedSound {
    const SoundAsset* asset = nullptr; // 非 null = .sound.json 由来 (バス/揺らぎ/3D 設定つき)
    AssetID clip = {};                 // asset == nullptr のときの生クリップ (M45c の名前キー経路)
    bool Valid() const { return asset != nullptr || !clip.IsNull(); }
};

// 解決順: .sound.json の名前キー → .sound.json の GUID 直指定 → 生クリップ
// (AudioSystem の名前キー = .wav/.ogg のファイル名 stem → クリップ GUID)。
// **生クリップへのフォールバックを外さないこと** — M19 からある PlaySound("beep") が
// 黙って無音になる (M45c の申し送り)
ResolvedSound ResolveSoundKey(const AudioSystem& audio, const SoundLibrary& lib, uint64_t key);

// ---- 純関数 (selftest がデバイス無しで叩ける) ----

// 重み付き抽選。roll は呼び出し側が用意した乱数値 (AudioSystem 所有の Pcg32。
// **world.Rng() は絶対に使わない** — RNG state は hash 対象で sim が壊れる)。
// 候補 (weight>0 かつ clip!=0) が無ければ -1
int PickVariationIndex(const SoundAsset& s, uint32_t roll);

// SoundAsset → PlayDesc (2D 部分のみ。3D 定位/ドップラーは M45e が上書きする)。
// jitter は [-1,1]。0,0 を渡せば揺らぎ無し = 試聴が毎回同じ音になる。
// **バス名の解決は audio 側に問う** — M45d でバスは .mixer.json のデータになったので、
// 静的な既定 4 バスではなく実際に張られているグラフで引く必要がある
PlayDesc MakePlayDesc(const SoundAsset& s, int variationIndex, float volJitter, float pitchJitter,
                      const AudioSystem& audio);

// stream = true の .sound.json を **BGM ストリーミングレーン**で鳴らす (M45f)。
// クリップ表 (PCM 全展開) には載せず、GUID から実ファイルを引いてディスクから直接読む。
// **BGM レーンは 1 本** — 別の曲が鳴っていればクロスフェードで置き換わる。
// バリエーションは先頭固定 (BGM を毎回抽選する意味が無い)、揺らぎも掛けない
bool PlayMusicSound(AudioSystem& audio, const SoundAsset& s, float fadeSeconds);

// エディタ試聴 (Asset Browser のダブルクリック / Inspector の ▶)。
// バリエーションは先頭固定・揺らぎ無しで再現性を優先する。クリップが未ロードなら
// GUID からパスを解決してロードを試みる。
// **stream = true なら PlayMusicSound へ回す** ので、返るハンドルは無効になる
// (BGM はボイスプールの外のレーンで鳴るため)
AudioHandle PreviewSound(AudioSystem& audio, const SoundAsset& s);

} // namespace mye
