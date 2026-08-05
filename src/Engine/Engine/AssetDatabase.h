#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

#include "Engine/Core/ImportMetaResolver.h"

namespace mye {

// アセットの種別 (.meta の "type" と AssetBrowser のアイコン/フィルタ用)。
// append-only で拡張すること (.meta に文字列で保存するため実値は非依存だが、
// 判定ロジックの一貫性のため既存値は維持する)。
enum class AssetType : int32_t {
    Unknown = 0,
    Texture,    // .png .jpg .jpeg .tga .bmp .dds
    Model,      // .glb .gltf .fbx .obj
    Material,   // .mat.json
    Prefab,     // .prefab.json
    Anim,       // .anim.json
    Controller, // .controller.json
    Scene,      // .scene.json
    Audio,      // .wav .ogg
    Shader,     // .hlsl .hlsli
    Script,     // .cs
    Sound,      // .sound.json (M45c)
    Mixer,      // .mixer.json (M45d)
    Actor,      // .actor.json (M48d — プレハブ 2.0。Prefab は部分集合として読込互換)
};

// アセット 1 件のサイドカー情報 (<asset>.meta に JSON で保存)。
// version 2 (M39b) でテクスチャに tex (インポート設定) が付く。ReadMeta は
// contains+既定値の前方互換読みなので v1 の .meta はそのまま許容される
struct AssetMeta {
    uint64_t guid = 0;                 // 安定識別子。初期値 = HashStr(normpath) → 現行 AssetID と一致
    AssetType type = AssetType::Unknown;
    int32_t version = 1;               // .meta フォーマットのバージョン
    std::wstring path;                 // 本体ファイルの実パス (.meta を除く)
    importmeta::TextureImportSettings tex; // type==Texture のみ意味を持つ (v2、M39b)
};

// アセットDB (M23): assets\ を走査し、各アセットに .meta サイドカーを生成/読込して
// GUID ⇄ パスの双方向解決を提供する。
//
// 決定論/互換性: GUID は「パスハッシュ継承」方式。新規アセットの初期 GUID =
// HashStr(WideToUtf8(NormalizePathKey(path))) = 現行の AssetID (TextureLibrary::IdForFile 等) と
// 完全一致するため、既存の .scene.json / .rep は無傷 (WorldHash 不変・ReplayFile bump 不要)。
// リネーム時は .meta を本体と一緒に移動することで GUID が永続し、シーン参照が壊れない
// (リネーム耐性)。
class AssetDatabase {
public:
    // assetsRoot 以下を再帰走査し、各アセットの .meta を生成/読込してマップを再構築する。
    // 再実行で追加/削除に追従する (冪等)。
    void ScanAndSync(const std::wstring& assetsRoot);

    // パス → GUID。.meta があればその GUID、なければ path-hash を返す。
    // createIfMissing=true かつ .meta 不在なら .meta を書き出す。
    uint64_t GuidForPath(const std::wstring& path, bool createIfMissing = true);

    // GUID → 現在のパス (未知なら空文字列)。
    std::wstring PathForGuid(uint64_t guid) const;

    // パス → 種別 (未走査なら拡張子から即時判定)。
    AssetType TypeForPath(const std::wstring& path) const;

    // 拡張子/サフィックスから種別を判定 (静的)。
    static AssetType ClassifyPath(const std::wstring& path);
    static const char* TypeName(AssetType t);
    static AssetType ParseTypeName(const std::string& s);

    // パスが .meta サイドカーそのものか。
    static bool IsMetaPath(const std::wstring& path);

    size_t Count() const { return byGuid_.size(); }

    // .meta 単体の読み書き (AssetOps / テストからも使う)。
    static bool ReadMeta(const std::wstring& metaPath, AssetMeta& out);
    static bool WriteMeta(const std::wstring& metaPath, const AssetMeta& m);

    // <asset> に対応する .meta を生成/更新し、確定した GUID を返す (静的ヘルパ)。
    // AssetOps の creator が新規アセット保存直後に呼ぶ。
    static uint64_t EnsureMeta(const std::wstring& assetPath);

    // ファイル/フォルダの移動を実行時テーブルへ反映する (M30b)。
    // oldPath 配下 (フォルダの場合) を含む旧キーを byPath_/typeByPath_/byGuid_ から外し、
    // 移動後の newPath を再走査して同伴 .meta の GUID で再登録する。
    // 物理的な移動 (fs::rename + .meta 同伴) は呼び出し側 (AssetOps::MoveAssetToFolder) の責務
    void MoveAsset(const std::wstring& oldPath, const std::wstring& newPath);

    // このインスタンスを assetkey::Resolve + assetguid::ResolvePath のバックエンドにする
    // (M30c パス→GUID / M39a GUID→パス)。
    // 解決は GuidForPath(path, createIfMissing=false) — テーブル → ディスク .meta → path-hash。
    // 未移動アセットは GUID == path-hash なので従来とビット同一。
    // EngineLoop が ScanAndSync 直後に呼び、終了時に Uninstall する (両フック同時)
    void InstallAsKeyResolver();
    static void UninstallKeyResolver();

private:
    void SyncOne(const std::wstring& path);

    std::unordered_map<uint64_t, std::wstring> byGuid_;      // guid → 現在パス
    std::unordered_map<std::wstring, uint64_t> byPath_;      // normpath → guid
    std::unordered_map<std::wstring, AssetType> typeByPath_; // normpath → 種別
};

} // namespace mye
