#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

struct EngineContext;
struct Selection;
class UndoStack;

// AssetBrowser のドラッグ&ドロップ用ペイロード名。データは UTF-8 のファイルパス (null 終端)。
inline constexpr const char* kAssetDragPayload = "MYE_ASSET";

// ---- ファイル名ユーティリティ (M50b で公開) ----
// 禁止文字 (\/:*?"<>| + 制御文字) を除去する緩いサニタイズ。非 ASCII (日本語名) は通す。
// 前後空白と末尾ドットを落とし、空になったら fallback
std::string SanitizeFileName(const std::string& in, const char* fallback);
// destDir 直下で衝突しない絶対パスを返す ("name.ext" → "name (1).ext"。
// .actor.json 等の複合サフィックスは維持)
std::wstring MakeUniqueAssetPath(const std::wstring& destDir, const std::wstring& filename);

// ---- アセット新規作成 (AssetBrowser の右クリック Create) ----
// M51i: 戻り値を作成パスに変更 (Create Undo 記録のため。失敗は空)
std::wstring CreateFolderAsset(const std::wstring& dir, const std::string& name);
std::wstring CreateSceneAsset(const std::wstring& dir, const std::string& name);   // .scene.json (空)
std::wstring CreateAnimationAsset(EngineContext& ctx, const std::wstring& dir,
                                  const std::string& name);                        // .anim.json
std::wstring CreateMaterialAsset(EngineContext& ctx, const std::wstring& dir,
                                 const std::string& name);                         // .mat.json
std::wstring CreateSoundAsset(EngineContext& ctx, const std::wstring& dir,
                              const std::string& name);                            // .sound.json
std::wstring CreateMixerAsset(EngineContext& ctx, const std::wstring& dir,
                              const std::string& name);                            // .mixer.json
// .actor.json (M48d)。ルート 1 個だけの最小構成アセット。**新規作成は常に .actor.json** —
// 既存 .prefab.json は読み書きとも据え置き (強制移行しない)
std::wstring CreateActorAsset(EngineContext& ctx, const std::wstring& dir,
                              const std::string& name);                            // .actor.json
// <root>\src\GameLogic\Scripts\<name>.cpp。root はプロジェクト起動なら <project>、
// レガシー起動ならエンジンリポジトリ
std::wstring CreateCppScript(EngineContext& ctx, const std::string& name);
std::wstring CreateCSharpScript(EngineContext& ctx, const std::string& name); // assets\scripts\<name>.cs

// ---- 外部ファイルインポート (エクスプローラー D&D) ----
struct ImportResult {
    int imported = 0; // コピーしたファイル数 (フォルダ内の個々のファイルも数える)
    int skipped = 0;  // 自己ドロップ / .meta / OS ゴミファイルでスキップ
    int failed = 0;   // コピー失敗
};
// srcs (絶対パス) を destDir へコピーし .meta を付与する。フォルダは再帰コピー。
// 同名は "name (1).ext" 形式で自動リネーム (.scene.json 等の複合サフィックスは維持)
ImportResult ImportExternalPaths(EngineContext& ctx, const std::vector<std::wstring>& srcs,
                                 const std::wstring& destDir);

// ---- アセット移動 (グリッド/ツリーへの D&D、M30b) ----
// srcPath (ファイル or フォルダ) を destDir 直下へ移動する。**.meta を同伴移動** して GUID を
// 永続させ (インポートと違い外部由来でないので安全)、ctx.assetDb の実行時テーブルも更新する。
// 同一フォルダ / 自己・子孫への移動 / .meta 直接指定は無視。同名衝突は " (1)" 連番。
// undo 非 null なら Relocate エントリを積む (M51i)。戻り値: 移動後の絶対パス (無視/失敗は空)
std::wstring MoveAssetToFolder(EngineContext& ctx, const std::wstring& srcPath,
                               const std::wstring& destDir, UndoStack* undo = nullptr);

// ---- アセットリネーム (右クリック → Rename、M30d) ----
// srcPath を同じフォルダ内で newName にリネームする。ファイルは拡張子/複合サフィックス
// (.prefab.json 等) を維持し newName は stem のみ (Unity 同様)。.meta を同伴リネームして
// GUID を永続させ (= シーン参照維持)、ctx.assetDb のテーブルも更新する。
// 不正文字 (\/:*?"<>|) は拒否。同名衝突は " (1)" 連番。
// undo 非 null なら Relocate エントリを積む (M51i)。戻り値: 新パス (無視/失敗は空)
std::wstring RenameAsset(EngineContext& ctx, const std::wstring& srcPath,
                         const std::string& newName, UndoStack* undo = nullptr);

// ---- アセット削除 (M51i) ----
// path (ファイル or フォルダ) を **ごみ箱** へ移動する (IFileOperation + FOF_ALLOWUNDO)。
// ファイルは .meta を同伴し、フォルダは配下ごと。ctx.assetDb の実行時テーブルからも除去する。
// UndoStack には積まない — 復元手段はごみ箱 (OS の「元に戻す」→ エディタ再起動の再走査で復活)
bool DeleteAssetToRecycleBin(EngineContext& ctx, const std::wstring& path);

// ---- アセット複製 (M51i、Ctrl+D) ----
// srcPath を同じフォルダ内へ "name (1).ext" 連番でコピーする。**旧 .meta はコピーしない** —
// 新パスのパスハッシュで新規 GUID を発行する (GUID の複製は byGuid_ の後勝ち上書きで
// 既存シーン参照が複製物へ張り替わる事故になる)。フォルダは配下を再帰コピー
// (.meta 除外 = 全ファイル新 GUID)。undo 非 null なら Duplicate エントリを積む。
// 戻り値: 複製先の絶対パス (失敗は空)
std::wstring DuplicateAsset(EngineContext& ctx, const std::wstring& srcPath,
                            UndoStack* undo = nullptr);

// ---- Create の Undo 記録 (M51i) ----
// 生成直後のアセットを Create エントリとして積む (undo = ごみ箱へ / redo = 内容を書き戻す)。
// Create* の署名は変えない — InstantiateAssetAtPath 内部の CreateSoundAsset (シーン Undo
// エントリの一部) を独立エントリに割らないため、記録は AssetBrowser の Create 経路だけが行う
void RecordAssetCreated(UndoStack& undo, const std::wstring& path);

// 複合サフィックス (.scene.json 等) を保ったままファイル名を stem/suffix に分割する
// (リネーム UI のプリフィル用に公開。実装は ImportExternalPaths と共用)
void SplitAssetName(const std::wstring& filename, std::wstring& stem, std::wstring& suffix);

// ---- アセット配置 (ドラッグ&ドロップ) ----
// path が .prefab.json ならインスタンス化、.glb/.gltf ならモデルロード。1 Undo エントリ + 自動選択。
// pos 非 null でその位置に、parentFileId 非 0 でその子に配置。
void InstantiateAssetAtPath(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& path, const DirectX::XMFLOAT3* pos,
                            uint64_t parentFileId);

// ---- スクリプトアタッチ (D&D で .cs をエンティティへ、M31) ----
// csPath (.cs) から C# スクリプトコンポーネント (生成 .cs は namespace 無し → FullName ==
// ファイル名 stem) を target に付与する。未登録なら CompileCSharpScripts で自動コンパイルして
// から再解決する。付与は Add Component と同じ 1 Undo エントリ + 付与先を自動選択。
// 戻り値: 成功なら true (種別違い / 未解決 / 二重付与は false で WARN ログ)
bool AttachScriptToEntity(EngineContext& ctx, Selection& selection, UndoStack& undo,
                          const std::wstring& csPath, EntityID target);

// ---- マテリアル割当 (D&D で .mat.json をエンティティへ) ----
// matPath (.mat.json) をロードして target の MeshRenderer に割り当てる
// (AssetBrowser ダブルクリック割当と同じ流儀の 1 Undo エントリ)。
// 戻り値: 成功なら true (MeshRenderer 不在 / ロード失敗は false で WARN ログ)
bool AssignMaterialToEntity(EngineContext& ctx, Selection& selection, UndoStack& undo,
                            const std::wstring& matPath, EntityID target);

// ---- スクリプトワークフロー ----
void OpenInExternalEditor(const std::string& editorCmd, const std::wstring& path); // {file}/{line} 置換
// C++ スクリプトのビルド。プロジェクト起動時は <project>\src\GameLogic\Scripts\*.cpp を
// cl.exe で <project>\cache\GameLogic.dll へ直接ビルドする (vcxproj / msbuild を介さない)。
// レガシー起動時のみ従来どおり tools\build_scripts.bat (gen + msbuild) を起動する
void RebuildGameLogic(EngineContext& ctx);
void CompileCSharpScripts(EngineContext& ctx); // assets\scripts\*.cs をエンジン内 Roslyn でコンパイル

// M51j: ビルドワンストップ (BuildSettings) 用の非対話スクリプトビルド。
// RebuildGameLogic と同じ生成物 (bat) を使うが、fire-and-forget ではなく
// プロセスハンドル (void* = HANDLE) を返し、呼び出し側が毎フレームポーリングして
// 完了と終了コードを拾う。出力は logPathOut のファイルへリダイレクトされる
// (pause で止まらないよう stdin は NUL)。失敗 (bat 不在等) は nullptr。
// ハンドルは呼び出し側が CloseHandle すること
void* StartGameLogicBuild(EngineContext& ctx, std::wstring& logPathOut);

} // namespace mye
