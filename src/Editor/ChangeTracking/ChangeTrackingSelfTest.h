//====================================================================================
//                          ChangeTrackingSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の Editor 側の回帰テスト (宣言)
//====================================================================================
#pragma once
#include <functional>

namespace mye {

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

// 描画バックエンドの無い ImGui の文脈を 1 つ作り、フレームを 1 つ回して捨てる (frame の中で窓を描く試験用)。
// 既にある文脈 (Editor 本体) には触らず、終わったら元へ戻す
void WithHeadlessImGuiFrame(const std::function<void()>& frame);

} // namespace mye
