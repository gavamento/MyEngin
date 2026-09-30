#pragma once

namespace mye {

// デカール (M56a) のヘッドレス回帰テスト。GPU もウィンドウも要らない。
//
// このテストが守っている不変量:
//   - `FillDecalTransform` の逆行列が world の逆であること (転置の取り違えを止める)。
//     ここを間違えると「絵は出るのに箱の外へはみ出す」という気付きにくい壊れ方をする
//   - 投影方向がワールド行列の第 3 行 (= ローカル +Z、LightComponent と同じ規約) であること
//   - `DecalAngleFadeCos` が [0,180] の外を丸めること (cos が単調な区間から出ない)
//   - `DecalDrawOrderLess` が収集順に依存しない全順序であること (規則 7)
//   - `DecalComponent` が **kComponentNoHash** = 足してもワールドハッシュが 1 ビットも
//     動かないこと (描画専用レーンであることの機械証明。.rep 互換の根拠)
bool RunDecalSelfTest();

} // namespace mye
