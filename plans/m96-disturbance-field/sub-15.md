# sub-15: 地面の霧を局所ライトで照らす (ON/OFF の切り替え)

- 依存: sub-03 (波 W1。spec §6)
- 状態: OK (統合待ち。W1 の順 04 → 05 → 15 → 12)
- 往復: 1


## 並列の注意 (波 W1)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): `pad0` を `fogLocalLights` に改名 (サイズ不変、snapshot 版なし。末尾は触らない)。ライトは**新しい CB** (b8 以降の空きを調べて決め、共有ヘッダに書く)。規則 9 のグループ「地面の霧のライト」。デモは `AddFieldDemoLanterns`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §2 #24 (ユーザー回答 2026-11-08: 照らす / 照らさないを ON/OFF で切り替える。太陽光の bool と同じ扱い)、§4.3 の「地面の霧」。
ランタン・松明の周りで地面の霧が光る絵を、GroundLayer の層ごとに選べるようにする。

1. 欄: `DisturbanceFieldComponent` に `fogLocalLights` (bool、場ごと、既定 **false**)
   - 置き場所: sub-01 で入れた明示の詰め物 `pad0` (`fogSunLight` / `autoFroxel` / `drawDebug` の次、2026-11-08 に存在を確認) を `fogLocalLights` に改名する。サイズ不変なので snapshot の版は上げない (spec 付録 A.1)。末尾は触らない (W1 で sub-04 が末尾に欄を足している)
   - コンポーネントの生バイトが変わるので、場を含む焼き込みハッシュ (`DisturbanceFieldSelfTest` の `kBakedFinalHash` など) は**再採取してよい** (sim の結果は変わらない。再採取した値と理由を SELF_EVAL に書く)
   - Volume では無視 (Volume はフロクセルが局所ライトのビームと影を既に持つ)。Inspector に注記
2. ライトの選び方 (CPU、描画レーン): `SceneLightData` の局所ライト (点・スポット、`type != 0`) のうち、影響球 (位置・range) が `fogLocalLights` の層のどれかの板の AABB と交わるものを、**ライト一覧の順**に最大 `kGroundFogMaxLights = 8` 本まで選ぶ。超えた分は落とし、WARN をシーンにつき 1 回 (落とした本数)。選び方は毎フレーム同じ入力から同じ結果になる
3. 10 か所のシェーダへの渡し方: 選んだライトの**縮約した写し** (位置・range・色 × 強度・スポットの向きと cosInner / cosOuter・種類) を、sub-02 / sub-03 の場の CB に載せる (8 本 × 64 バイト程度)。各シェーダが持つ既存のライト一覧の並びには依存しない (粒子・VFX・スカイなど、ライト一覧を持たない合成箇所もあるため)。CB のバイト数と最大本数は C++ と HLSL で一致させ、規則 9 に入れる
4. 計算: `GroundFogComposite` の段ごとの散乱に、`fogLocalLights` の層の σ_s × ライトの放射輝度 × HG(g) × 減衰を足す。減衰とスポットの式は `common.hlsli::LightSample` (フロクセルの注入が使っているもの) と**同じ式**にする (霧と面で光の届き方が食い違わないため)。HG の g は sub-03 と同じ値
   - **影なし** (局所ライトの影 = シャドウアトラスは使わない)。理由: 10 か所のシェーダすべてにアトラスを束縛する必要があり、段数 24 × ライト本数 × 影サンプルのコストになる。影つきのビームが要る場面は Volume (フロクセル、影つき) で作る
   - 速くするため、線分とライトの影響球が交わらないライトは段のループの前に外す (ピクセルごと)
5. Inspector の説明: 照らすライトは最大 8 本 (超えた分は使われない)、影なし、ON にすると GPU のコストが増える (`Tr()` 両言語)
6. デモ: `--field-demo` の末尾に点光源のランタン 2〜3 個を霧の中に置き、層の `fogLocalLights` を true にする

## やらないこと (このサブでは)

- 局所ライトの影、9 本目以降のライト
- Volume (フロクセル) の局所ライトの扱いの変更

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h` / `.cpp` (欄と説明)、必要なら `SimSnapshot.h`・`AcousticAudioSelfTest.cpp`
- `src\Engine\Engine\Disturbance\DisturbanceRender.*` (ライトの選び方と CB への詰め替え)、`src\Engine\Renderer\Passes\DisturbanceFieldPass.*`
- `assets\shaders\disturbance_common.hlsli` (`GroundFogComposite` と CB)、`common.hlsli` の `LightSample` (読むだけ)
- `tools\check_rules.ps1` 規則 9
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 35。

- 35:
  - (a) `fogLocalLights = false` の絵が切り替え前 (sub-14 のコミット) と `--img-diff` で差 0
  - (b) true でランタンの周りの霧が明るくなる (ランタンの近くと遠くの霧の平均輝度を報告)。ランタンが霧の層から離れていれば (影響球が板と交わらない) 絵が変わらない
  - (c) 9 本置くと 8 本だけ使われ、WARN が 1 回 (ログ)
  - (d) 1920×1080 で層 1 枚・ライト 0 / 1 / 8 本の GPU 時間を報告 (合否の線は置かず、planner が最大本数の既定を判断)
- 12: replay_verify `field` (運用によりまとめて実行)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- スクショ一式 (Runtime.exe、Forward と Deferred)、`Editor.exe --img-diff`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-08)。pad0 → fogLocalLights (サイズ 76 不変、版なし)、b8 のライト CB (528 B)、8 本の選択と WARN、LightSample と同じ式、規則 9 の 2 エントリを確認。35(a) はシェーダの比較だけ (C++ は flag off でマスク 0 = 経路が変わらない) で受け入れ。35(d) のノイズは sub-17 の静かな再計測へ。式の二重管理は nit (sub-17 で共有ヘッダ化を検討)
