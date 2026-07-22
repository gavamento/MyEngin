#pragma once

namespace mye {

// VFX (M29c: Sprite/Trail/TextMesh) のヘッドレス回帰テスト。
// TrailStore の点列蓄積/寿命/容量/エンティティ同期と、VfxGeometry の純関数
// (テキストクアッド構築 / リボン構築 / ビルボード基底) を検証する。全て D3D 不要。
bool RunVfxSelfTest();

} // namespace mye
