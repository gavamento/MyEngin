#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

#include "Editor/Selection.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Core/ImportMetaResolver.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct FieldDesc;
class UndoStack;

// リフレクション駆動 Inspector (engine_spec.md 9 章)。
// ComponentRegistry のフィールド表から widget を自動生成する —
// コンポーネント個別の UI コードは存在しない (これが M1 リフレクション設計の回収点)
// M40a マルチ選択: 全選択が共通に持つコンポーネントを表示 (値は primary のもの)、
// 編集/削除/追加/paste/reset は全選択へバッチ適用 (1 Undo エントリ)。ギズモは primary のみ
class InspectorWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo);

private:
    // アセット選択時の表示 (M40c): 名前/種別/GUID + テクスチャは Import Settings 編集
    void DrawAssetInspector(EngineContext& ctx, Selection& selection);

    // fids/comps は同コンポーネントを持つ選択エンティティ列 (要素 [0] = primary、comp と同一)。
    // 単一選択では要素 1 個。ポップアップ系 (mask/参照ピッカー) はこの列へバッチ書込する
    bool DrawField(EngineContext& ctx, const char* componentName, void* comp,
                   const FieldDesc& field, EntityID entity, Selection& selection, UndoStack& undo,
                   const std::vector<uint64_t>& fids, const std::vector<void*>& comps);
    // 参照ピッカー (ポップアップで選択。変更時は自前で Undo エントリを記録する)
    void DrawAssetRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                      UndoStack& undo, const std::vector<uint64_t>& fids,
                      const std::vector<void*>& comps, uint32_t fieldOffset);
    void DrawEntityRef(EngineContext& ctx, const FieldDesc& field, void* p, Selection& selection,
                       UndoStack& undo, const std::vector<uint64_t>& fids,
                       const std::vector<void*>& comps, uint32_t fieldOffset);

    // Add Component ポップアップの検索フィルタ (開くたびにクリア)
    char addComponentFilter_[64] = {};

    // アセットインスペクタの編集キャッシュ (M40c)。選択パスが変わったら .meta から再読込
    std::wstring assetEditPath_;
    importmeta::TextureImportSettings assetImportEdit_;

    // マテリアルインスペクタの編集キャッシュ (M40d)。.mat.json のスキーマ固定編集
    struct MaterialEditState {
        bool valid = false;
        std::string name;
        std::string shader = "forward_lit";
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float metallic = 0.0f;
        float roughness = 0.5f;
        float emissive = 0.0f; // M46i: 自己発光の強さ (0 = 発光なし)
        bool transparent = false;
        uint64_t textureGuid = 0; // 0 = なし (保存は GUID 数値、M39a)
        uint64_t normalGuid = 0;
    };
    MaterialEditState matEdit_;
    void LoadMaterialEdit(EngineContext& ctx, const std::wstring& path);
    void DrawMaterialInspector(EngineContext& ctx, const std::wstring& path);

    // サウンドインスペクタの編集キャッシュ (M45c)。.sound.json のスキーマ固定編集。
    // マテリアルと同じく **アセット編集は UndoStack 対象外** (既存規約)
    SoundAsset soundEdit_;
    bool soundEditValid_ = false;
    void LoadSoundEdit(const std::wstring& path);
    void DrawSoundInspector(EngineContext& ctx, const std::wstring& path);

    // 回転編集中のオイラー角キャッシュ (quat→euler→quat の往復ドリフト防止)
    DirectX::XMFLOAT3 eulerCache_ = { 0, 0, 0 };
    EntityID eulerCacheEntity_ = kNullEntity;
    const void* eulerCacheField_ = nullptr;
    bool eulerEditing_ = false;

    // サイズ (LocalTransform.scale) の比率固定。エディタ UI 状態 — シリアライズ/ハッシュ非対象。
    // 基準はドラッグ開始時の値 (毎フレームの比率累積だと 0 通過で他軸が潰れたまま戻らない)
    bool scaleLinked_ = false;
    DirectX::XMFLOAT3 scaleLinkBase_ = { 1, 1, 1 };
    EntityID scaleLinkEntity_ = kNullEntity;
    const void* scaleLinkField_ = nullptr;
    bool scaleLinkEditing_ = false;

    // 名前欄の編集開始時の値 (確定時に同一なら改名しない — Esc の revert 対策、M48b)
    std::string nameOriginal_;
};

} // namespace mye
