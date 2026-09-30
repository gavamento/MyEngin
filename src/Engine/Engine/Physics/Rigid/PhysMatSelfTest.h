#pragma once

namespace mye {

// 物理マテリアル資産 (.physmat.json、M59a1) のヘッドレス回帰テスト。
//
// このテストが守っている不変量:
//   - ClassifyPath が ".physmat.json" を PhysMat に分類し、**Material に誤爆しない**こと
//     (どちらも "mat.json" を含む紛らわしいサフィックス同士)
//   - FromJson が種別キー ("physmat") の無い JSON を拒否すること
//   - Sanitize が NaN / 負値 / 範囲外を止めること — M59a2 の結合則 (sqrt / min) に
//     NaN が入ると 2 台のワールドハッシュが割れるため、ここが唯一の防波堤
//   - ToJson → FromJson がビット同一で往復すること
//   - Enumerate が名前昇順 (ハッシュの反復順を表に出さない。規則 7)
//   - physmat::Resolve が未接続 / null ID で安全に nullptr を返すこと
bool RunPhysMatSelfTest();

} // namespace mye
