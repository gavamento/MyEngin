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

// 描画バックエンドの無い ImGui の文脈を 1 つ作り、フレームを 1 つ回して捨てる (frame の中で窓を描く試験用)。
// 既にある文脈 (Editor 本体) には触らず、終わったら元へ戻す
void WithHeadlessImGuiFrame(const std::function<void()>& frame);

} // namespace mye
