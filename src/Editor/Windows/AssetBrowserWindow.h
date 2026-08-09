#pragma once
#include <string>

#include "Editor/Selection.h"
#include "Engine/Core/ImportMetaResolver.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

class UndoStack;
class AssetPreviewCache;

// Asset Browser (engine_spec.md 9 章、M11)。
// assets/ をフォルダツリー + ファイルグリッドで表示。テクスチャはサムネイル、
// 他は拡張子アイコン。ダブルクリックで OS 既定アプリで開く。
// .prefab.json はダブルクリックでシーンへインスタンス化 (M13)。
// .scene.json はダブルクリックでシーンを開く (EditorApp が dirty ガード経由でロード)。
class AssetBrowserWindow {
public:
    bool open = true; // 閉じる / 再表示 (タブ [x] と Window メニューに連動)
    void OnImGui(EngineContext& ctx, Selection& selection, UndoStack& undo,
                 const std::string& externalEditorCmd, AssetPreviewCache& preview);

    // ダブルクリックされた .scene.json のパスを取り出す (空 = リクエストなし)。
    // EditorApp が毎フレーム消費し、未保存変更ガードを通してロードする
    std::wstring TakePendingOpenScene()
    {
        std::wstring p;
        p.swap(pendingOpenScene_);
        return p;
    }

    // ダブルクリックされた構成アセット (.actor.json / .prefab.json) のパス (空 = なし)。
    // M48k: EditorApp がミニシーン編集モードで開く。シーンへの配置は D&D と
    // 右クリックメニュー「シーンに配置」に残してある
    std::wstring TakePendingOpenActor()
    {
        std::wstring p;
        p.swap(pendingOpenActor_);
        return p;
    }

    // ダブルクリックされた .mixer.json をアクティブにしたか (M45d)。
    // true なら EditorApp が Audio Mixer 窓を開く (適用自体はここで済んでいる)
    bool TakeOpenMixerRequest()
    {
        const bool r = openMixerRequest_;
        openMixerRequest_ = false;
        return r;
    }

    // ---- エクスプローラー D&D (EditorApp が WM_DROPFILES 処理時に参照) ----
    // 前フレームにパネルが前面に描画されていて (x,y) [クライアント座標] が矩形内なら true
    bool IsClientPosInPanel(float x, float y) const;
    const std::wstring& CurrentDir() const { return current_; } // 空 = 初回フレーム前 (未初期化)

private:
    void DrawDirTree(EngineContext& ctx, const std::wstring& dir);
    // 命名モーダルの確定処理 (M51i: Create Undo 記録のため undo を受ける)
    void DoCreate(EngineContext& ctx, UndoStack& undo, const std::string& externalEditorCmd);
    // リネームモーダルを開く準備 (createName_ に現在の stem をプリフィル、M30d)
    void BeginRename(const std::wstring& path);

    std::wstring current_; // 表示中フォルダ (絶対パス)
    std::wstring pendingOpenScene_; // ダブルクリックされたシーン (TakePendingOpenScene で消費)
    std::wstring pendingOpenActor_; // ダブルクリックされた構成アセット (M48k)
    bool openMixerRequest_ = false; // .mixer.json をダブルクリックした (M45d)
    // D&D 移動 (M30b)。描画中の fs 変更 (iterator 破壊) を避けるためフレーム末に実行する
    std::wstring pendingMoveSrc_;
    std::wstring pendingMoveDstDir_;
    // リネーム (M30d)。対象パスを保持しモーダル確定で RenameAsset を呼ぶ
    std::wstring pendingRenamePath_;
    bool requestRenameModal_ = false;
    // Import Settings (M39b)。対象パス + 編集中設定 (Apply で .meta v2 へ書き即リロード)
    std::wstring pendingImportPath_;
    importmeta::TextureImportSettings importEdit_;
    bool requestImportModal_ = false;
    // 削除 (M51i)。対象パスを保持し確認モーダルの確定で DeleteAssetToRecycleBin を呼ぶ
    std::wstring pendingDeletePath_;
    bool requestDeleteModal_ = false;
    // 複製 (M51i)。実行はフレーム末 (D&D 移動と同じ fs 変更の集約点)
    std::wstring pendingDuplicatePath_;
    // 検索 + 型フィルタ (M51i)。どちらかが有効な間は current_ 以下の再帰検索モードになる
    char searchBuf_[96] = {};
    int typeFilterIdx_ = 0; // kTypeFilters の添字 (0 = すべて)
    bool init_ = false;
    int pendingCreate_ = 0;     // Create モーダルで作る種別 (0=なし)
    bool requestModal_ = false; // 次フレームで命名モーダルを開く
    char createName_[96] = {};

    // パネル矩形 (ImGui::GetWindowPos 基準)。multi-viewport 無効の間はクライアント座標と
    // 一致する — 有効化する場合はスクリーン座標になるため要改修
    float panelMin_[2] = {};
    float panelMax_[2] = {};
    bool panelRectValid_ = false; // 今フレーム前面に描画されたか (背面タブ/クローズ時 false)
};

} // namespace mye
