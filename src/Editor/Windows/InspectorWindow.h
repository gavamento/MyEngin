#pragma once
#include <DirectXMath.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Editor/AssetPreviewCache.h"
#include "Editor/FractureBakeService.h" // M80i: 破片焼きの非同期ワーカー
#include "Editor/Selection.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Core/ImportMetaResolver.h"
#include "Engine/Engine/Audio/AudioClip.h" // Deep-Modal プレビューの直近クリップ (M76g)
#include "Engine/Engine/Audio/ModalAudio.h" // ModalShotResult / kModalPreviewDefaultImpulse (sub-10 H)
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/EngineLoop.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"
#include "Engine/Renderer/FxStackAsset.h"     // M78c: fxstack アセット編集状態
#include "Engine/Renderer/ProjectShaderProperties.h"  // M78c r2: スキーマ駆動 Inspector

namespace mye {

struct FieldDesc;
struct ComponentDesc;
class UndoStack;
// Deep-Modal (M76g)。ModalFeatureMap/DmNetHeader の定義は ModalAudio.h 経由で既に見えている
// (Engine/Engine/Modal/ModalTypes.h / ModalFeatureMap.h) — ModalSoundComponent だけは
// ModalAudio.h も前方宣言止まりなのでここでも前方宣言を要る
struct ModalSoundComponent;

// 水面プレビュー (Edit 中だけ水面を動かす) のオン/オフ。Inspector の WaterWave 欄のボタンが書き、
// EditorApp::OnRenderViews が読んで RenderSystem::waterPreviewSeconds を進める (描画専用)
bool& EditorWaterPreviewOn();

// Inspector に出すエンティティの集合 (M40a)。[0] = primary。表示値は primary、編集は全対象へバッチ適用
struct InspectorTargets {
    uint64_t fid = 0;            // primary の fileId
    EntityID e = kNullEntity;    // primary の EntityID
    std::vector<uint64_t> fids;  // 生存する選択の fileId (primary 先頭)
    std::vector<EntityID> ents;  // 同じ並びの EntityID
    bool multi = false;
    EntityID prefabRoot = kNullEntity; // primary が属するプレハブインスタンスのルート (無ければ null)
    bool isPrefabMember = false;
};

// コンポーネント 1 型ぶんの描画に使う値 (InspectorWindow::DrawComponent が組む)
struct InspectorComponentRow {
    ComponentTypeId type = 0;
    const ComponentDesc* desc = nullptr;
    std::vector<uint64_t> fids;   // この型を持つ対象の fileId ([0] = primary)
    std::vector<void*> comps;     // 同じ並びのコンポーネント実体
    bool managed = false;         // C# スクリプトコンポーネント (フィールドは managed 側が持つ)
    bool addedInInstance = false; // プレハブインスタンスで追加された comp (M50c の "+C")
};

// リフレクション駆動 Inspector (engine_spec.md 9 章)。
// ComponentRegistry のフィールド表から widget を自動生成する —
// コンポーネント個別の UI コードは存在しない (これが M1 リフレクション設計の回収点)
// M40a マルチ選択: 全選択が共通に持つコンポーネントを表示 (値は primary のもの)、
// 編集/削除/追加/paste/reset は全選択へバッチ適用 (1 Undo エントリ)。ギズモは primary のみ
class InspectorWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    // preview はマテリアルのライブプレビュー用 (M53)。AssetBrowser のサムネイルと同一インスタンス
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                 AssetPreviewCache& preview);

private:
    // アセット選択時の表示 (M40c): 名前/種別/GUID + テクスチャは Import Settings 編集
    void DrawAssetInspector(EngineContext& ctx, Selection& selection, AssetPreviewCache& preview);

    // ---- エンティティ選択時の OnImGui の部品 (上から描く順) ----
    void DrawNameRow(EngineContext& ctx, Selection& selection, UndoStack& undo, const InspectorTargets& tg);
    // M76k: 名前欄の直下のタグ欄。TagComponent は「オブジェクト固有の印」= 名前と同じ枠で、
    // コンポーネント一覧には出ない (Hidden)。ここが唯一の編集口
    void DrawTagRow(EngineContext& ctx, Selection& selection, UndoStack& undo, const InspectorTargets& tg);
    // 対象の説明 1 行 (件数 / Entity index:generation)。名前 → タグ → ここ の順
    void DrawTargetInfoRow(const InspectorTargets& tg);
    void DrawPrefabBar(EngineContext& ctx, Selection& selection, UndoStack& undo, const InspectorTargets& tg);
    // コンポーネント 1 型ぶん。表示判定・見出し・PushID / PopID を持ち、中身は下の 3 つに任せる
    void DrawComponent(EngineContext& ctx, Selection& selection, UndoStack& undo, const InspectorTargets& tg,
                       ComponentTypeId t);
    void DrawComponentContextMenu(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                  const InspectorTargets& tg, const InspectorComponentRow& row);
    void DrawComponentFields(EngineContext& ctx, Selection& selection, UndoStack& undo,
                             const InspectorTargets& tg, const InspectorComponentRow& row, void* comp);
    void DrawComponentNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const InspectorTargets& tg, const InspectorComponentRow& row);
    // M76g: ModalSound 節の末尾 (状態 / セル数 / 6 面ボタン / Export WAV)。
    // DrawComponentNotes から desc.name == "ModalSound" のときだけ呼ばれる
    void DrawModalSoundNotes(EngineContext& ctx, const InspectorTargets& tg, const InspectorComponentRow& row);
    // M80i: Destructible 節の末尾 (焼きボタン・非同期の状態・拒否理由・プレハブ無効化)。
    // DrawComponentNotes から desc.name == "Destructible" のときだけ呼ばれる
    void DrawDestructibleNotes(EngineContext& ctx, Selection& selection, UndoStack& undo,
                               const InspectorTargets& tg, const InspectorComponentRow& row);
    // 焼き結果が Ready になったら呼ぶ (メインスレッド): 成功なら .mfrac を保存して
    // AssetDatabase/FractureLibrary に登録し、Destructible.fractureAsset を書き換えて
    // BuildFracturePieces で子を組み直す (1 Undo エントリ)。失敗/拒否は fractureOutcomes_ へ残す
    void CommitFractureBakeResult(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                  const InspectorTargets& tg);
    // 6 面ボタン 1 個ぶんの本体。sub-06 と同じ MakeModalShotPlay を呼ぶ (2 本目の規則を書かない)
    void FireModalPreviewFace(EngineContext& ctx, const InspectorTargets& tg,
                              const ModalSoundComponent& comp, const ModalFeatureMap& fm,
                              const DmNetHeader& hdr, int face);
    // 直近のプレビュー clip (modalPreview_) を .wav へ書き出す
    void ExportModalPreviewWav(EngineContext& ctx);
    void DrawRemovedPrefabComponents(EngineContext& ctx, Selection& selection, UndoStack& undo,
                                     const InspectorTargets& tg);
    void DrawUnknownComponents(EngineContext& ctx, const InspectorTargets& tg);
    void DrawAddComponentPopup(EngineContext& ctx, Selection& selection, UndoStack& undo,
                               const InspectorTargets& tg);
    void DrawScriptDropTarget(EngineContext& ctx, Selection& selection, UndoStack& undo,
                              const InspectorTargets& tg);

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

    // タグ欄の「タグを追加...」のインライン入力 (M76k)。開いた行だけが入力欄を出す
    char tagAddBuffer_[32] = {};
    bool tagAddOpen_ = false;

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
        // M67: 反射に映るときの品質クラス (0=Hero … 4=Default)。読みで範囲外は 4 に落とす
        int reflectionClass = 4;
        bool transparent = false;
        uint64_t textureGuid = 0; // 0 = なし (保存は GUID 数値、M39a)
        uint64_t normalGuid = 0;
        // M79 sub-04: shader が "*.surface" のときの Properties 値 (fxstack と同じ PropValue map)
        std::unordered_map<std::string, PropValue> properties;
        // M79 sub-06: サーフェスの視錐台余白 [m] / 両面描画 (spec §4.2)。サーフェスでなくても
        // 保持する (JSON 往復で値を落とさない) が、効くのはサーフェス選択時だけ
        float boundsPadding = 0.0f;
        bool doubleSided = false;
    };
    MaterialEditState matEdit_;
    // shader 名 → Properties スキーマのキャッシュ (M79 sub-04。fxstack と同じ仕組みだが
    // 対象アセットが違うので別キャッシュに分けてある)。
    // M79d-fix (review-1 #5): ファイル更新時刻で無効化する PropertySchemaCache に変更
    PropertySchemaCache matSchemaCache_;
    void LoadMaterialEdit(EngineContext& ctx, const std::wstring& path);
    void DrawMaterialInspector(EngineContext& ctx, const std::wstring& path,
                               AssetPreviewCache& preview);
    // 保存する .mat.json 本文。Save とプレビューの**両方**がこれを使うので、
    // 「プレビューで見た絵」と「保存した結果」が構造的にずれない (M53)
    std::string MaterialEditToJson(const std::wstring& path) const;

    // M79 sub-04: Properties スキーマ取得 (キャッシュ付き) と自動 UI 描画の共通化。
    // fxstack (DrawFxStackInspector) とマテリアル (DrawMaterialInspector) の両方から呼ぶ —
    // 取得は ShaderManager::FetchPropertySchema (M78f の assets 全域索引) 経由にしてあり、
    // 旧 fxstack 実装が ShaderDirs() 直下しか見ていなかった不具合も合わせて直る (spec §2)。
    // M79d-fix (review-1 #5): cache は PropertySchemaCache (ファイル更新時刻で無効化)
    const PropertyParseResult& GetOrFetchPropertySchema(
        EngineContext& ctx, const std::string& shaderName, PropertySchemaCache& cache);
    // Properties スキーマに沿ったウィジェット一式を描画し、変更を values へ書き戻す。
    // texAssetEncode: Tex2D でアセットを選んだときに values へ書く文字列を作る
    // (fxstack は 16 進 GUID 文字列、マテリアルは Material.texture 等と同じ 10 進 GUID 文字列。
    // 規約が違うので呼び出し側が注入する。spec §4.2)
    void DrawPropertiesEditor(EngineContext& ctx, const PropertyParseResult& schema,
                              std::unordered_map<std::string, PropValue>& values,
                              const char* idScope,
                              const std::function<std::string(uint64_t)>& texAssetEncode);

    // マテリアルのライブプレビュー (M53)。エディタ UI 状態 — シリアライズもハッシュもしない。
    // matEdit_ から組んだ Material は JSON 本文のハッシュが変わったときだけ作り直す
    // (毎フレーム作ると GUID 解決とテクスチャ検索を無駄に踏む)
    int matPreviewShape_ = 0; // PreviewShape の添字 (0=球)
    Material matPreviewMat_;
    uint64_t matPreviewHash_ = 0; // 0 = 未構築

    // サウンドインスペクタの編集キャッシュ (M45c)。.sound.json のスキーマ固定編集。
    // マテリアルと同じく **アセット編集は UndoStack 対象外** (既存規約)
    SoundAsset soundEdit_;
    bool soundEditValid_ = false;
    void LoadSoundEdit(const std::wstring& path);
    void DrawSoundInspector(EngineContext& ctx, const std::wstring& path);

    // 物理マテリアルインスペクタの編集キャッシュ (M59a1)。.physmat.json のスキーマ固定編集。
    // サウンドと同じく **アセット編集は UndoStack 対象外** (既存規約)
    PhysMat physMatEdit_;
    bool physMatEditValid_ = false;
    void LoadPhysMatEdit(const std::wstring& path);
    void DrawPhysMatInspector(const std::wstring& path);

    // fxstack インスペクタの編集キャッシュ (M78c)。*.fxstack.json のパス別インメモリ編集。
    // アセット編集は UndoStack 対象外 (マテリアル / サウンドと同じ規約)
    struct FxStackEditState {
        bool        valid        = false;
        int         selectedPass = 0;     // 選択中パス (Inspector で展開表示する)
        std::string saveStatus;           // 最後の保存結果メッセージ (表示後クリア)
        // M78c round 2: シェーダ名 → ParseProperties 結果のキャッシュ
        // (スキーマ駆動 Inspector の毎フレームパースを避ける)。
        // M79d-fix (review-1 #5): ファイル更新時刻で無効化する PropertySchemaCache に変更
        PropertySchemaCache schemaCache;
    };
    mye::FxStackAsset fxstackEdit_;
    FxStackEditState  fxstackEditState_;
    void LoadFxStackEdit(const std::wstring& path);
    void DrawFxStackInspector(EngineContext& ctx, const std::wstring& path);

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

    // Deep-Modal 面打ちプレビュー (M76g)。エディタ UI 状態のみ — シリアライズ/ハッシュ非対象。
    // Export WAV は「直近にここへ書いた clip」を使う (SoundGenWindow::Save と同じ設計。
    // どのエンティティの Inspector を開いているかに関係なく、直近に鳴らした音を書き出せる)
    struct ModalPreviewState {
        // N・s。スライダの現在値 (0.1..20)。既定はヘッドレス --modal-face-probe の
        // kProbeImpulse と同じ共有定数 (sub-10 H、reviewer round 1 指摘 2 の是正 —
        // 「4.0 では 6 面すべて BelowMin」だった旧既定と、片方だけ直った定数の再発防止)
        float impulse = kModalPreviewDefaultImpulse;
        AudioClip clip;         // 直近のプレビュー (MakeModalShotPlay の出力)
        bool valid = false;     // clip が実際に鳴ったか (BelowMin 等では false のまま)
        int face = -1;          // 直近に押した面 (0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z)。ファイル名に使う
        uint64_t entityFid = 0; // clip を作ったエンティティの fileId (ファイル名に使う)
        // sub-10 H: 「押しても何も起きない」と「まだ押していない」を UI から区別できるように
        // する (reviewer round 1 指摘 2)。everFired==false の間は lastResult は無意味
        bool everFired = false;
        ModalShotResult lastResult = ModalShotResult::Played;
    };
    ModalPreviewState modalPreview_;

    // 破片焼き (M80i)。ワーカー本体はここが所有 (Inspector はアプリ生存中 1 個の長命オブジェクト、
    // EditorApp::inspector_ と同じ寿命)。ID は Destructible を持つエンティティの fileId
    FractureBakeService fractureBakeService_;
    // 直近の焼き結果 (拒否理由・失敗理由の表示用)。fractureAsset に反映される「生成済み」表示は
    // ここではなく世界の現在状態 (Destructible.fractureAsset + 子の数) から毎フレーム導出する —
    // ここはシーンに永続しない「最後に何が起きたか」だけを覚えるエディタ UI 状態
    struct FractureBakeOutcome {
        bool success = false;
        bool rejectedOpenMesh = false;
        int32_t boundaryEdges = 0;
        int32_t nonManifoldEdges = 0;
        int32_t orientationMismatches = 0;
        std::string failReason;
    };
    std::unordered_map<uint64_t, FractureBakeOutcome> fractureOutcomes_;
};

} // namespace mye
