//====================================================================================
//                          ChangeTrackingSelfTest.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の Editor 側の回帰テスト (宣言)
//====================================================================================
#pragma once

namespace mye {

// AI Change Tracking の Editor 側 (M104j) の回帰テスト。**サービスなしで動く** (偽の I/O と、流し込む行だけ)。
// 検査: projectId の固定表 / 応答の解釈 (順不同・分割・複数行・不正行・上限超え) / hello の失敗ごとの状態 /
//       時間切れ / 切断と再接続 / Poll の量の上限 / モデルの取り直しの間引き / 無効時に I/O の口を呼ばない。
// 実サービスとの結合は MYE_TRACK_PROBE=1 のときだけ (bin\x64\<構成>\mye_track.exe が要る。既定では起動しない)
bool RunChangeTrackingSelfTest();

} // namespace mye
