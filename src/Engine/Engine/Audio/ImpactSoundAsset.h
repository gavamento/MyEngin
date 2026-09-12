//====================================================================================
//                          ImpactSoundAsset.h
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          .impact.json → ImpactSynth → RegisterClip → SoundAsset
//====================================================================================
#pragma once
#include <cstdint>
#include <string>

#include "nlohmann/json.hpp"

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Audio/ImpactSynth.h"
#include "Engine/Engine/Audio/SoundAsset.h"

namespace mye {

class AudioSystem;

// 手続き生成サウンドの資産 (.impact.json)。**ImpactSynth は PCM を作るだけ**なので、
// ファイル / AudioSystem / SoundLibrary を触る糊はここに閉じる (計画 §18-20)。
//
// 1 ファイル = 1 サウンド。起動走査 (RegisterAssetLibraries) が読み、variations 本の PCM を
// 生成して AudioSystem へ登録し、それらを参照する SoundAsset を SoundLibrary へ**名前キーで**
// 登録する。スクリプトは .sound.json と区別なく PlaySound("glass_break") で鳴らせる。
//
//   { "engine":"MyEngine", "impactSound":1,
//     "kind":"GlassBreak", "materialA":"Glass", "materialB":"Concrete",
//     "strength":1.0, "size":1.0, "durationSec":0.5, "seed":1000, "variations":4,
//     "sound": { "volume":1.0, "volumeRandom":0.08, "pitchRandom":0.03, "bus":"SE", "priority":180 } }
//
// ★名前はファイル名 (glass_break.impact.json → "glass_break")。.sound.json と同じ規約。
// ★クリップの AssetID は HashStr("synth://<name>#<i>") — M74a の guid:// サブアセット鍵と
//   同じ「登録名のハッシュ」方式。ファイル GUID と衝突しない専用の名前空間
// ★SoundLibrary への登録パスは仮想パス "synth://<name>" にする。実ファイルのパスを渡すと
//   Inspector の SaveToFile が .impact.json を .sound.json の中身で上書きしてしまう
struct ImpactSoundDesc {
    std::string name;          // 名前キー
    ImpactSynthParams synth;   // seed は variation 0 の値。i 本目は seed + i
    int32_t variations = 4;    // 1..16
    SoundAsset sound;          // 再生側の設定 (variations は登録時に埋めるのでここでは空)
};

// JSON → desc。"impactSound" キー必須 (別資産を黙って読まない)。kind / material の未知名は false。
// name はあれば読む (無ければ呼び出し側がファイル名から決める)
bool ParseImpactSound(const nlohmann::json& j, ImpactSoundDesc& out);

// variation i のクリップ登録名 "synth://<name>#<i>" と、その AssetID (= HashStr(登録名))
std::string ImpactClipName(const std::string& name, int32_t variation);
AssetID ImpactClipId(const std::string& name, int32_t variation);

// desc を生成・登録する。戻り値 = SoundLibrary のハッシュ (失敗 0)。
// 既に登録済みなら差し替える (RegisterClip が参照中の voice を止める = ホットリロード可)
uint64_t RegisterImpactSound(AudioSystem& audio, SoundLibrary& sounds, const ImpactSoundDesc& desc);

// .impact.json を読んで RegisterImpactSound する。戻り値 = SoundLibrary のハッシュ (失敗 0)
uint64_t LoadImpactSoundFile(AudioSystem& audio, SoundLibrary& sounds, const std::wstring& path);

} // namespace mye
