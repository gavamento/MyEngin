//====================================================================================
//                          ChangeTrackingSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の Editor 側の回帰テスト (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class ChangeTrackingSession;

// AI Change Tracking の Editor 側 (M104j) の回帰テスト。**サービスなしで動く** (偽の I/O と、流し込む行だけ)。
// 検査: projectId の固定表 / 応答の解釈 (順不同・分割・複数行・不正行・上限超え) / hello の失敗ごとの状態 /
//       時間切れ / 切断と再接続 / Poll の量の上限 / モデルの取り直しの間引き / 無効時に I/O の口を呼ばない。
// 実サービスとの結合は MYE_TRACK_PROBE=1 のときだけ (bin\x64\<構成>\mye_track.exe が要る。既定では起動しない)
bool RunChangeTrackingSelfTest();

// AI Changes 窓 (M104k) の、ImGui を回さない部分の回帰テスト (ChangeViewSelfTest.cpp)。文面の整形 (英日) / 節への振り分け /
// DiffView の行の組み立て / 1 万行の差分で見えている行だけを描くこと (ヘッドレスの ImGui フレーム)。
// RunChangeTrackingSelfTest の末尾から呼ぶ (同じ "changetracking" スイート)
void RunChangeViewSelfTests(const std::function<void(bool, const char*)>& check);

// シーンの変更の強調・一覧・選択の同期 (M104m) の回帰テスト (SceneChangeOverlaySelfTest.cpp)。偽の I/O と、ヘッドレスの ImGui フレームで
// 実際に Hierarchy を描いて、バッジの数・World が変わらないこと・取り直しの契機を確かめる。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunSceneChangeOverlaySelfTests(const std::function<void(bool, const char*)>& check);

// レビュー (承認・却下・要確認・再開) の Editor 側 (M104o) の回帰テスト (ReviewSelfTest.cpp)。対象の組み立て (1000 件ごとに分ける) / 失敗・時間切れで
// 表示を先に変えないこと / まとめての確認 / 文面 (英日) / 窓 (ヘッドレスの ImGui フレーム) / 自分の保存の印。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunReviewSelfTests(const std::function<void(bool, const char*)>& check);

// 復元の判定 (M104p。作業ツリーを書かない) の回帰テスト (RestoreSelfTest.cpp)。ファイル全体・Hunk の判定 / Rust と同じ固定表 (tests	rackixture\hunk_apply_cases.json) /
// ゲート / パスの検査 (一時ディレクトリの本物のジャンクション) / 文面 (英日) / プレビュー (偽のサービスと偽の読み取り口、ヘッドレスの ImGui フレーム) /
// 一時プロジェクトの全ファイルが変わらないこと / 操作全体のレビュー・集約・大きいファイルの先頭。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunRestoreSelfTests(const std::function<void(bool, const char*)>& check);

// 復元の実行 (M104p2。利用者のファイルを書く) の回帰テスト (RestoreApplySelfTest.cpp)。**必ず一時プロジェクトで**行う。適用 (ファイル全体・Hunk・追加の取り消し・
// 削除の取り消し・名前の戻し) / 書く前に止まる (競合・ゲート・パスの差し替え) / 失敗 → 巻き戻し・他者の書き込みの保護 / 起動時の回復 / 内容の取得の失敗 /
// .git\index が変わらず git を起動しない / 8 MiB 超 / 記録の送り直し / 窓。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunRestoreApplySelfTests(const std::function<void(bool, const char*)>& check);

// ライブのシーンの復元 (M104q。World を Undo の 1 エントリで書き換え、ファイルは書かない) の回帰テスト (SceneRestoreSelfTest.cpp)。**ライブの Scene を作って**行う。
// 正規形での照合 (1.0 と 1・型違い・EntityRef) / Field・Component・Entity (子孫つき)・name・親子の適用と Undo・Redo / 競合で World が変わらない /
// 別のシーンに当たらない / 読み直しが合わなければ戻す / プレハブの上書きの記録 / C# コンポーネント (MyeScripting.dll と .NET が無ければ SKIP と明記) /
// ゲート / 窓 (偽のサービス。ファイルを書かない・ジャーナルを作らない・記録を送る) / アセットの登録の後始末。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunSceneRestoreSelfTests(const std::function<void(bool, const char*)>& check);

// Before / After の 3D 比較 (M104s。プレビュー専用の Scene へ読む。ファイルもライブのシーンも書かない) の回帰テスト (ScenePreviewSelfTest.cpp)。
// 読み込み・印・カメラ・Scene の数 / 失敗 (欠け・版・壊れた JSON・上限・接続なし・ファイルなし) / 512 KiB ずつの取得と古い応答 / アセットの注記 (偽の履歴) /
// C# を読み込まない / 窓 (ヘッドレスの ImGui フレーム・デバイス消失) / 入口 / 本番の Scene・Undo・選択・一時プロジェクトのファイルが変わらないこと /
// Play 中に開いても sim のハッシュが変わらないこと。RunChangeTrackingSelfTest の末尾から呼ぶ
void RunScenePreviewSelfTests(const std::function<void(bool, const char*)>& check);

// 本物の ManagedHost (SceneRestoreSelfTest が起動したもの) で、プレビューが C# コンポーネントを読み込まず、ライブの C# の状態に触れないことを確かめる
class ManagedHost;
class Scene;
void RunScenePreviewManagedChecks(const std::function<void(bool, const char*)>& check, ManagedHost& host, Scene& liveScene, ComponentTypeId probeType);

// 実サービスとの結合 (RunProbe から呼ぶ。MYE_TRACK_PROBE=1)。root = 一時プロジェクト。pump(条件, 上限ms) はセッションを回して条件を待つ
void RunSceneRestoreProbe(ChangeTrackingSession& session, const std::wstring& root, const std::function<bool(const std::function<bool()>&, int)>& pump,
                          const std::function<void(bool, const char*)>& check);

// 描画バックエンドの無い ImGui の文脈を 1 つ作り、フレームを 1 つ回して捨てる (frame の中で窓を描く試験用)。
// 既にある文脈 (Editor 本体) には触らず、終わったら元へ戻す
void WithHeadlessImGuiFrame(const std::function<void()>& frame);

} // namespace mye
