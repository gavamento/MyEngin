#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/EntityID.h"

namespace mye {

// ---- 物理マテリアル資産 (.physmat.json、M59a1) ----
// 剛体ソルバへ流し込む材料特性のデータ化。**M59a1 の時点では sim は 1 バイトも読まない**
// (消費 = Collider.physMaterial の解決は M59a2)。スキーマは M59f2 (静動摩擦分離・転がり抵抗)
// の分まで先に定義してある — 後からキーを足すとプリセットとユーザー資産の書式が二世代に
// 割れるため。熱・破壊系のキー (M61/M62) は将来この構造体の末尾へ追加できる
// (FromJson は contains + 既定値の前方互換読みなので旧ファイルはそのまま許容される)。
//
// 決定論: 値はワールドハッシュ非対象 (メッシュコライダーと同クラス =「再生時に同じ資産が
// ある」前提。M59 決定台帳 5)。読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ。
//
// 単位系は SI (m / kg / s)。実材料からの換算の目安 (プリセット 5 種の根拠。
// 5 種目の glue は M60k で足した粘着 (adhesion 60 N) の被写体で、これだけは実材料ではなく
// 「4kg は保ち 14kg は落とす」という被覆の都合から決めてある):
//   密度 kg/m^3 : 鋼 7850 / ゴム 1100 / 氷 917 / 木 (オーク) 700 / 水 1000
//   静止摩擦 μs : 鋼-鋼 0.74 / ゴム-乾燥路面 ~1.0 / 木-木 0.5 / 氷-氷 0.1
//   動摩擦 μd   : 鋼-鋼 0.57 → ゲーム値 0.45 / ゴム 0.8 / 木 0.35 / 氷 0.03
//   反発 e      : 鋼球-鋼板 0.6 / スーパーボール 0.8 / 木 0.35 / 氷 0.05
//   転がり抵抗  : 鋼輪-レール 0.001 / タイヤ-舗装 0.02 / 木 0.01 / 氷 0.002
//   Cd は本来「形状」特性 (球 0.47 / 立方体 ~1.05)。ここは材料既定値で、
//   M59b の AeroComponent 側の上書きが優先される。
// GPa 級の剛性 (ヤング率そのもの) はここに置かない — float 直値でソルバに入れると発散する
// ので、M60' の XPBD compliance (= 1/k) 側で扱う (M59 決定台帳 4)。
struct PhysMat {
    uint64_t hash = 0; // = GUID (PhysMatLibrary のキー)
    std::string name;
    std::wstring path;

    float density = 1000.0f;        // kg/m^3。M59a2 の Rigidbody.useDensity が質量導出に使う
    float staticFriction = 0.5f;    // μs (M59f2 までソルバは動摩擦のみ消費)
    float dynamicFriction = 0.5f;   // μd (既存 Collider.friction に対応する側)
    float restitution = 0.0f;       // e (0..1)
    float rollingResistance = 0.0f; // 転がり抵抗係数 (M59f2)
    float dragCoefficient = 0.47f;  // Cd 既定 (M59b の Aero が上書き可能)
    // ---- M60d 追加 (末尾 append。旧ファイルは contains 無し = 0 で従来どおり) ----
    // 粘着力 [N]。接触の法線インパルス下限を負まで開いて「引っ張っても離れない」を作る。
    // ★計画は「adhesion·面積」だったが **ソルバは接触面積を持っていない** (マニフォールドは
    //   最大 4 点で面積が無い) — 単位を N に倒して「このペアが支えられる引っ張り力」を
    //   直接オーサリングする形にした。結合則は **min** (弱いほうが勝つ = 反発と揃える)。
    // ★0 なら下限は 0 = **従来とビット同一** (存在ゲート)
    float adhesion = 0.0f;
};

// 列挙 1 件 (AssetRef ピッカー / Asset Browser 用。SoundEntry 範型)
struct PhysMatEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .physmat.json の管理 (SoundLibrary 範型)。EngineLoop が所有し physmat:: で注入
class PhysMatLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path);          // 失敗時 0
    uint64_t Register(const std::wstring& path, PhysMat mat); // 返り値 = hash

    const PhysMat* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return mats_.find(hash) != mats_.end(); }
    std::vector<PhysMatEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const PhysMat& m);
    // 読み値は Sanitize 済みで返す。物理マテリアルでない JSON ("physmat" キー無し) は false
    static bool FromJson(const nlohmann::json& j, PhysMat& out);

    // NaN / 負値 / 範囲外の防波堤。**ソルバより手前で必ず 1 回通っていること** —
    // M59a2 の結合則 (μ = sqrt(μa·μb) / e = min) に NaN が入ると、2 台の結果が
    // 「どちらの NaN がどう伝播したか」で割れる。ここで止めるのが唯一の防波堤
    static void Sanitize(PhysMat& m);

private:
    std::unordered_map<uint64_t, PhysMat> mats_;
};

// モジュール注入 (meshcol:: と同じ流儀。EngineContext を汚さない)。
// EngineLoop が起動時に Install し、終了時にライブラリ破棄前に必ず外す。メインスレッド専用
namespace physmat {
void Install(PhysMatLibrary* lib);
PhysMatLibrary* Library();          // 未接続 = nullptr (起動走査 / ReloadHub / エディタ UI 用)
const PhysMat* Resolve(AssetID id); // 未接続 / 未登録 / null ID = nullptr (M59a2 のソルバ解決用)
} // namespace physmat

} // namespace mye
