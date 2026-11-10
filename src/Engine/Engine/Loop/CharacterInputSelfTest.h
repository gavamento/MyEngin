#pragma once

namespace mye {

// 入力 → CharacterController (CharacterInput、M97a) の回帰テスト。
// 3 つの移動方式、直接キーの変更と actions.json の軸との合成、ジャンプ (押した瞬間・空中では不発・先行入力)、
// 入力レーン、CharacterController が無いときに何もしないこと、止めた tick の入力が再開時に暴発しないこと、
// 同じ入力列でワールドハッシュが一致することを確かめる
bool RunCharacterInputSelfTest();

} // namespace mye
