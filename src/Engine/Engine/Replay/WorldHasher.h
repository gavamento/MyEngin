#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
class CpuParticleBackend;
class XpbdBackend;
struct TimeControl;
class PersistStore;

// ワールド状態ハッシュ (engine_spec.md 11.3)。
// 対象: 全エンティティ (index 昇順) の親リンク + シリアライズ対象コンポーネントの
//       登録フィールド (float はビットパターン)、ワールド RNG、TimeControl と
//       PersistStore (M51g、RNG の直後・キー昇順)、CPU パーティクル状態。
// GPU パーティクルは描画出力扱いで除外 (比較モードで別途検証)。
// null の項は畳み込まない (World 単体の selftest 用。EngineLoop の
// 記録/検証は常に Scene の実体を渡す)。
// 実装を変更すると過去の .rep が検証不能になるため、変更時は ReplayFile の
// バージョンを上げること

// sim 状態源の集約 (M60'a = 予約事項 1)。「ECS 外の sim 状態」のハッシュ源は
// ここに member を 1 個ずつ足す (次は M60'b の XpbdBackend)。名指し引数のまま
// 増やすと全呼び出し (~107 箇所) が毎回壊れるので箱で受ける。
// ★member は必ず末尾へ append — 呼び出し側は位置指定の波括弧初期化で組んでいる
struct SimSources {
    const CpuParticleBackend* particles = nullptr;
    const TimeControl* time = nullptr;
    const PersistStore* persist = nullptr;
    // M60'b: XPBD 変形体の池。★畳み込みは内容ゲート (池が空なら節ごと畳まない) —
    // CPU 粒子節と違い「配線しただけ」で既存シーンのハッシュを動かさないため
    const XpbdBackend* xpbd = nullptr;
};

uint64_t HashWorld(World& world, const SimSources& src = {});

// 乖離診断用: エンティティ毎のサブハッシュ (index 昇順)
struct EntityHash {
    EntityID entity;
    uint64_t hash;
};
void HashWorldDetailed(World& world, const SimSources& src, std::vector<EntityHash>& outEntities,
                       uint64_t& outTotal);

// ---- フィールド単位ダンプ (M52a) ----
// HashWorld / HashWorldDetailed / HashWorldDump は **同一実装の 3 出口**。
// 3 者が別々に世界を走査していると診断がそのうち嘘をつく (割れていないと言う / 別の行を
// 指す) ので、走査順と畳み込みはこのファイル内の 1 実装にしかない。
// WorldHasherSelfTest が「3 出口の total 一致」を毎回固定する。
//
// 行の書式 (タブ区切り 7 列):
//   tick / entity ("index:generation"、大域行は "-") / 名前 / コンポーネント /
//   フィールド / 値 (生バイトの hex) / 畳み込み hash
// 値列は **ハッシュが読むバイト範囲そのまま** (FieldTypeSize 分) を出す —
// String64 の終端以降 (M48i の罠) を診断が見落とさないための必須条件。
// 畳み込み列の意味: エンティティのフィールド行はそのエンティティ内の畳み込み
// (= HashEntity の途中値)、"#entity" 行と大域行はワールド全体の畳み込み。
// よって **最終行の畳み込み列 == total**。
struct HashDump {
    uint64_t tick = 0;
    uint64_t total = 0;
    std::vector<std::string> lines;
};

void HashWorldDump(World& world, const SimSources& src, uint64_t tick, HashDump& out);

bool WriteHashDump(const std::wstring& path, const HashDump& dump);
bool ReadHashDump(const std::wstring& path, HashDump& out);

// 2 つのダンプの差分。値列が食い違った**葉の行**を「差分」として数える。
//   - 畳み込み列は 1 つ割れると以降が全部ずれるので件数には使わず、
//     「最初に乖離した行」の特定だけに使う
//   - フィールド名が '#' 始まりの行 (#nameHash / #entity / #total) は他の行から決まる
//     まとめ値なので valueDiffs には数えない。**これにより 1 フィールドの変異は
//     必ず valueDiffs==1 になる** (数百行のノイズに埋もれさせない)
//   - まとめ値だけが割れて葉が 1 つも割れていない = ダンプがハッシュ対象を取りこぼして
//     いる証拠なので、rollupDiffs を別に数えて異常として報告する
struct HashDumpDiff {
    uint64_t valueDiffs = 0;
    uint64_t rollupDiffs = 0;
    size_t firstFoldLine = static_cast<size_t>(-1); // 0 起点。乖離が無ければ -1
    bool structureDiffers = false; // キー列 (tick 以外の 4 列) か行数が食い違う
    bool totalDiffers = false;
    bool Same() const
    {
        return valueDiffs == 0 && rollupDiffs == 0 && !structureDiffers && !totalDiffers;
    }
};
// 差分をログへ報告して結果を返す (maxReport 行まで詳細、以降は件数のみ)
HashDumpDiff DiffHashDumps(const HashDump& a, const HashDump& b, int maxReport = 32);

} // namespace mye
