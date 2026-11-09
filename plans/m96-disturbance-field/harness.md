# harness 台帳: m96-disturbance-field

- 依頼原文:
  このエンジンに
  ゼルダの伝説時のオカリナにある動く霧を実装したい
  プレイヤーの動きや攻撃などで実際に動く霧
  霧意外にも使えるようにしたい

  わからなかったら質問して。
  あっているかの確認したいからどんなふうの物だと思っているか先に言って
- 開始: 2026-11-08 / 基点コミット: 2961ba3453cb2983a67395b967506d7e0c1494cc
- フェーズ: 完了 (2026-11-09、review round 2 PASS)

## ユーザー判断 (司会の事前確認、2026-11-08)
- 形: 地面を這う薄い層 (2D) と立体 (3D) の両方
- 霧の状態はゲームロジックが読む (固定 Tick の CPU sim、hash / snapshot / replay 対象)
- 霧以外の用途: 草・植物の揺れ / 水面の波紋 / 煙・砂ぼこり・粒子 / 跡が残る (雪・砂) の 4 つすべて
- かき乱す源: プレイヤーの移動 / 攻撃・爆発 (明示) / 敵・動く物体を自動で / 風・環境 の 4 つすべて
- 格子の範囲: 場所に固定とプレイヤー追従の両方 (固定を先に)
- 物理 (剛体・布・ロープ) は押さない
- 進め方: /harness。coder は Sonnet
- 質問の運用: 当初は M80 方式 (推奨で進めて Notion に後回し) だったが、2026-11-08 にユーザーが「今いるから質問は普通にしていいよ」と変更した。planner が AskUserQuestion で直接聞く。使えなければ `[ユーザーに聞ける]` で返し、司会がすぐ聞く
- spec §7 の `[ユーザーに聞ける]` 1〜7 への回答 (2026-11-08):
  1. 自動の源: DisturbanceSource を付けた物だけ (推奨どおり)
  2. 斬撃の速度: アニメイベント定義の向き × 速さ (推奨どおり)
  3. 霧への太陽光: **当てる／当てないを bool で選べるようにする** (推奨と違う。既定値は planner が決める)
  4. フロクセルの有効化: **場を置いたら自動で有効にし、OFF にもできるようにする** (推奨と違う)
  5. 3D ボリュームの描画数: **4 個まで** (推奨と違う)
  6. 後回しの 3 件: **地形の起伏に沿う霧は今回入れる**。突風と視界遮蔽は後回し
  7. spec の確定: 確定 (回答の反映後、sub-01 に入る)
- **selftest は中間 (sub-07 の後) と最後 (sub-14) の 2 回だけ** (2026-11-08 ユーザー指示「セルフテスト行うのは最後と中間の2回だけにして」)。各サブはビルドと check_rules まで。selftest 系の受け入れ条件は planner の VERDICT で「後でまとめて確認」として扱い、sub-07 の後に司会がまとめて走らせる
- **replay_verify も selftest と同じく中間 (sub-07 の後) と最後 (sub-14) の 2 回だけ** (2026-11-08 ユーザー指示「replay_verifyもセルフテストと同じように」。sub-01 は指示前に実行済み = 23 ジョブ PASS)
- sub-02 の横縞 (フロクセルの奥行き分割が浅い角度で見える): **地面の霧専用の描画を足す** (2026-11-08 ユーザー回答。planner の裁定「許容」とは逆)
- 地面の霧を局所ライト (点光源・スポット) で照らすか: **ON/OFF で切り替えられるようにする** (2026-11-08 ユーザー回答。planner の推奨は「照らさない」)
- スカイボックス無しの背景: **Forward でも地面の霧を掛ける** (2026-11-08 ユーザー指示「Forwardでもスカイなしの時に霧を掛けて」。planner は spec R13 で制限として残す裁定だった)
- スカイ無しの背景のフロクセルの霧 (グローバル・3D Volume) も **Forward を Deferred に揃える** (2026-11-08 ユーザー回答。planner の推奨は「揃えない」)
- **coder を最大 4 本並列** (2026-11-08 ユーザー指示「coder並列化して4つくらい」)。波は spec §6 / 付録 A・B: W1 = 04 (メイン) / 05 / 12 / 15、W2 = 06 / 07 / 14 / 16 (統合後に中間のまとめ実行)、W3 = 08 / 09 / 11、W4 = 10 / 13、W5 = 17。並列のサブは worktree で実装し、司会が統合順に master へマージする
- 跡 (雪・砂) の色・強さの欄: **地形 (TerrainComponent) の側** (2026-11-08 ユーザー回答、推奨どおり)
- **質問の運用を「推奨で進めて Notion に後回し」へ切り替え** (2026-11-08 深夜、ユーザー指示「寝るから推奨で進める方にシフトして」)。planner が推奨で裁定し、[ユーザーに聞ける] は司会が Notion の質問表 (ID / 聞きたいこと / 推奨 / 逆を選ぶと / 回答) に転記する
- 事前調査: `plans/m96-disturbance-field/design-draft.md` (planner の出発点、裁定で変えてよい)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 2 | 64f3b55 | 2D 層の状態と 3 点セット (TypeId 96/97、snapshot v70、規則 18) |
| sub-02 | OK | 1 | 2687227 | 霧の描画 (2D)、--field-demo、replay ジョブ field |
| sub-03 | OK | 2 | 9b16433 | 地面の霧の専用描画 (板のレイマーチ、太陽光の bool) |
| sub-04 | OK | 2 | d4e54f4 | 流れのソルバ |
| sub-05 | OK | 2 | e664245 (統合 5ca2e83) | スクリプトの口 (ABI v35 = 202) |
| sub-06 | OK | 1 | 0784048 (統合 712bdfd) | アニメイベント disturb |
| sub-07 | OK | 3 | 59704c5 (統合 946bb49、追補 94239a1) | 3D ボリューム (4 個まで描画)。後で中間のまとめ実行 |
| sub-08 | OK | 1 | 5351b0c (統合 23e7fd4) | Volume のフロクセル自動有効と太陽光の bool |
| sub-09 | OK | 2 | 11d8a29 (統合 fed6402) | 追従モード |
| sub-10 | OK | 2 | 7eb4db5 (統合 d269a34) | 地形の起伏に沿う 2D 層 |
| sub-11 | OK | 1 | 5134dad (統合 1209684) | 粒子 (CPU / GPU) |
| sub-12 | OK | 1 | 4e871bb | 水面の波紋 |
| sub-13 | OK | 1 | 356a32c (統合 b4100a1) | 跡 (地形) |
| sub-14 | OK | 1 | 7b99fba (統合 44c80d4) | 草 |
| sub-15 | OK | 1 | 332ccd0 (統合 be28b81) | 地面の霧を局所ライトで照らす切り替え (最大 8 本、影なし) |
| sub-16 | OK | 1 | b19e3e9 (統合 256f582) | Forward のスカイ無し背景に地面の霧とフロクセル (全画面パス、golden 更新) |
| sub-17 | OK | 1 | 2b11247 | 仕上げ (ADR-036・文書)。まとめ実行: selftest-full D/R 116/0、replay 24/24、shot 31/1 (既存 flow_title)、三校 既存 2 件 |
| sub-18 | OK | 2 | 9c42f0a | review-1 の修正 (#1〜5, #7、#6 は ADR 追記)。受け入れ 37。最後に selftest-full / replay / shot を回す |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 3/3/4/4 | major 3 (復元後の描画通番が 1 に戻る / 巨大な速度で AV / disturbance Fast 層が Debug 22.3 s)、minor 4 (縁の流れ 0 / 追従で入る列の流れ 0 / coneCos の std::cos = planner / コメント位置)。全文 review-1.md |
| 2 | PASS | 4/4/4/4 | なし (review-1 #1〜#7 すべて解消)。全文 review-2.md |

## 一時停止 → 再開済み (2026-11-08 22:2x 停止、同日再開。ユーザー指示「一時停止して、少ししたら再開する」)
W1 の 4 本を止めた。全部未コミット。再開時は各 sub-NN.md 末尾の「中断メモ (2026-11-08)」から続ける (coder は新しく起動し、中断メモと worktree の場所を渡す)。
| サブ | 場所 | 状態 | 残り |
|---|---|---|---|
| sub-04 | メイン C:\HAL\MyEngin (未コミット) | ソルバ・並列・デバッグ線・--field-bench 実装済み、Release ビルド済み。snapshot v71 | kBakedFinalHash を 0xD2D198026566A0CB へ、スクショ回収 (cache\sub04_*.png)、渦の 1 枚、Debug ビルド、check_rules、SELF_EVAL。**受け入れ 15 (性能 1.67 ms) は不合格** = planner 判断。放射インパルスが投影で弱まる件も planner へ |
| sub-05 | .claude\worktrees\agent-adc78e94e6a5e1d68 (ブランチ worktree-agent-adc78e94e6a5e1d68、基点 b7cb1fa に reset 済み) | ABI v35 = 202 一式、selftest コード、デモ (ZZFieldDemoDriver) | Release 再ビルド結果の確認、Debug ビルド、スクショ撮り直し、C# 一時プローブ、SELF_EVAL |
| sub-12 | .claude\worktrees\agent-aa741273890b74858 (ブランチ worktree-agent-aa741273890b74858、master を merge した 8eb38e6 が基点) | 波紋のシェーダ・転送条件・デモ池・selftest コード | 最終配置の再ビルド、スクショ 4 枚 (WARP は 300 秒の timeout)、受け入れ 23、SELF_EVAL。転送条件の変更は付録 A 外 = planner 確認 |
| sub-15 | .claude\worktrees\agent-ad788ccedd6a6eede (ブランチ worktree-agent-ad788ccedd6a6eede、基点 b7cb1fa に reset 済み) | 実装一式、Release ビルド、スクショ OFF/ON 3 枚 | DemoContentField.cpp の一時改造 (S15TEMP) を scratchpad\sub15\DemoContentField.final.cpp で戻す、35(a)(c)(d)、Debug ビルド、check_rules、SELF_EVAL |
- 罠: Agent(isolation: worktree) の worktree は古い aa3288e から切られた (sub-01〜03 が無い)。05 / 15 は reset --hard b7cb1fa、12 は merge master で直した。次の波は起動直後に基点を確認させること

## W2 (2026-11-09 開始)
- W1 統合完了: 04 d4e54f4 / 05 5ca2e83 / 15 be28b81 / 12 4e871bb (sub-12 は前任のマージコミットを避け差分だけ当てた)
- W2 の worktree は司会が master の先頭から手で作る: C:\HAL\MyEngin\.claude\worktrees\w2-sub06 / w2-sub07 / w2-sub14 / w2-sub16 (ブランチ m96-w2-sub06 等)
- sub-14 OK (7b99fba)。統合の目視点に追加: RenderSystem.cpp の UpdateDisturbanceField 呼び出しが CollectLights 直後に 1 か所だけあり、sub-07 の UpdateFroxel 周りと食い違っていない。planner が直した sub-17.md (手動確認の追加) はメイン側に未コミットで残してある
- sub-06 OK (0784048)。統合の目視点: TickRunner.cpp の配送 switch、DemoContentField.cpp 末尾の呼び出しの並び、DisturbanceFieldSelfTest.cpp の無名名前空間ブロックと呼び出し行
- sub-07 OK (59704c5)。性能の線を「jobs ありで 1.67 ms」に変更 (spec §2 #27)。kBakedFinalHash 0x2D2D2A180F889F62 (sizeof 88)。統合の目視点: RenderSystem.cpp で UpdateDisturbanceVolumes が UpdateDisturbanceField (sub-14 で CollectLights 直後) のすぐ後、RenderTypes.h の append 位置、sizeof 88 の表明 (sub-15 テスト側も)
- Notion 質問表: 「M96 かき乱しフィールド: ハーネスの保留質問」 https://app.notion.com/p/41202024a4d481ac811ac5365298b94d (Q-1 = Volume の性能の線)
- W2 統合完了 (07 946bb49 → 06 712bdfd → 14 44c80d4 → 16 256f582)。競合は DemoContentField.cpp (共有の閉じ括弧と末尾の呼び出し)、DisturbanceFieldSelfTest.cpp (同、スイート関数の戻り値)、RenderSystem.cpp (UpdateDisturbanceVolumes を UpdateDisturbanceField の直後へ) を手で解いた。Debug / Release ビルド OK、check_rules 0 error / 59 warning。続いて中間のまとめ実行 (selftest-full Debug / Release、replay_verify)
- 中間のまとめ実行 (2026-11-09): 1 回目は disturbance スイートで FAIL + クラッシュ (Volume の古い断言、Step のスロット列 move で参照がぶら下がり)。sub-07 round 2・3 で修正 (波紋の注入式の欠陥も planner が must で直させた)。修正後 --selftest-full Debug / Release とも 116 suites 0 failed、replay_verify 24 jobs PASS、check_rules 0 error。94239a1。kBakedFinalHash = 0x851C4663912825C1

## W3 (2026-11-09 開始)
- 09 / 11 / 08 を master (94239a1 の後の台帳コミット) から切った worktree で並列実装。統合順 09 → 11 → 08
- W3 統合完了 (09 fed6402 → 11 1209684 → 08 23e7fd4)。競合は SimSnapshot.h (v74 に揃えコメントは両方)、AcousticAudioSelfTest (74)、DemoContentField.cpp / DisturbanceFieldSelfTest.cpp (共有の閉じ括弧・末尾の呼び出し・戻り値の式) を手で解いた。disturbance_volume.hlsli (sub-11 の上書き口と sub-08 の sunSigma) と粒子の 2 backend (引き寄せとモード値) は自動マージで両立を目視。Debug / Release ビルド OK、check_rules 0 error、fxc particle_sim / froxel_inject OK、--field-bench (Release) all checks passed・kBakedFinalHash 0x90646BB353FD22D5 のまま
- sub-09 round 2 は coder が受け渡しのループに入ったため司会が停止し、worktree の差分確認と --field-bench (Debug / Release) を司会が回して planner へ渡した

## W4 (2026-11-09 開始)
- 10 / 13 を master から切った worktree で並列実装。統合順 10 → 13
- W4 統合完了 (10 d269a34 → 13 b4100a1)。競合は SimSnapshot.h (v76)、AcousticAudioSelfTest (76)、DemoContentField.cpp / DisturbanceFieldSelfTest.cpp (共有の閉じ括弧・末尾の呼び出し Hill → Snow・戻り値の式) を手で解いた。Debug / Release ビルド OK、check_rules 0 error、fxc 5 本 OK、--field-bench all checks passed (kBakedFinalHash 0x0D0CBC4DCBEE185E)。統合後の --field-demo f300 に白飛びした矩形あり (scratchpad\m96-shots\w4_integrated_f300.png) → planner へ
- 池の白飛びの修正 (W4 追補): 原因は water_surface.hlsl の ApplyFog の引数の並びが M77b (746775c) 以来宣言と違っていたこと (M96 の回帰ではなく潜在バグの露見)。3 行の修正で Forward / Deferred とも池が霧に霞む。水面を使わない 4 デモ × 2 は tol 0 で差 0。planner OK。次は sub-17 (W5)

## 申し送り (セッション跨ぎ)
- design-draft.md と本台帳は sub-01 のコミットに同梱する (基点時点で未追跡)
- (planner、2026-11-08) spec.md と sub-01〜12 を作成。planner に AskUserQuestion が無かったため、spec §7 末尾の `[ユーザーに聞ける]` 1〜7 (7 = 確定確認) を司会が聞く。回答で裁定が逆になった論点は planner が spec §2 / §8 と該当 sub に反映してから sub-01 に着手する
- (planner、2026-11-08) ユーザー回答 1〜7 を反映済み。spec は「確定」。サブは 14 本 (新設 sub-03 = フロクセルの自動有効と太陽光の bool、sub-09 = 起伏に沿う層。旧 03〜12 は 04〜08 / 10〜14 へ繰り下げ、件名の記号も M96a〜n)
- (planner、2026-11-08) sub-02 の横縞へのユーザー回答 (専用描画) を反映: sub-03 = 地面の霧の専用描画 (新設)、旧 sub-03 は Volume 専用に絞って sub-08 へ、旧 sub-08〜14 → sub-09〜15、件名 M96c〜o。まとめ実行の最後は sub-15
- (planner、2026-11-08) 地面の霧の局所ライトへのユーザー回答 (ON/OFF) を反映: sub-15 = 局所ライト (新設、fogLocalLights 既定 false・最大 8 本・影なし)、仕上げは sub-16 (M96p)。まとめ実行の最後は sub-16 (中間は sub-07 の後のまま)
- (planner、2026-11-08) ユーザー指示「Forward でもスカイなしの時に霧を掛けて」を反映: sub-16 = Forward の背景だけの全画面パス (新設)、仕上げは sub-17 (M96q)。まとめ実行の最後は sub-17
- (planner、2026-11-08) 並列化: 波 W1 (04 実装中 / 05 / 12 / 15)、W2 (06 / 07 / 14 / 16)、W3 (08 / 09 / 11)、W4 (10 / 13)、W5 (17)。予約表は spec 付録 A、統合は付録 B。W1 の前に計画ファイルだけを先にコミット (B.0)。中間のまとめ実行は W2 の統合の後
- (planner、2026-11-09) 別件: golden `flow_title` が着手前 (bc957ea) から FAIL (maxDiff 62、944 画素、画面下の文字の明るさ)。M96 と無関係。`tools\check_rules.ps1` は powershell.exe 5 では構文エラー、pwsh で実行する
- (planner、2026-11-09) 別件: `SampleTerrainHeightWorld` (PhysicsBodyOps.cpp) が地形格子の頂点の真上でレイを外すことがある (sub-17 で発見)。M96 の影響は該当列の基準高さが baseY に落ちるだけ。直すなら物理側の別の仕事
- (司会、2026-11-09 朝) ユーザー判断: 質問の運用を「その場で聞く」に戻す。Q-1 (Volume 性能を jobs あり 1.67 ms 線で合格) = 推奨どおり「よい」。Q-2 (インパルスの coneCos を std::cos で作る、spec §2 #28) = 推奨どおり「使ってよい」。Notion の質問表にも記入済み
