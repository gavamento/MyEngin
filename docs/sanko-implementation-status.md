# 三校企画 実装状況（2026-09-08）

**計画全体は未完了。工程Aの一部を実装・検証した状態。**
研究施設から脱出する1〜2時間の完成ゲーム、配布可能な製品ビルドはまだ存在しない。

## 今回実装した内容

- 移動入力の長さが1を超える場合だけ正規化する。斜め移動で加速せず、小さなスティック入力は維持する。
- `AcousticEmitter.footstepGain` を操作へ接続する。しゃがみ0.4、歩行1.0、走行1.6に、スティック量に応じた0.2〜1.0の係数を掛ける。既存の音響処理が床材由来の振幅と到達距離の両方へ適用する。
- 走行はShift/LB、しゃがみはCtrl/B、光の設置・回収はF/Xのアクションで操作する。右スティックで水平・垂直の視点操作ができる。
- 光は回収完了まで強度を維持する。中断で設置済み状態を保ち、完了時に格納する。敵の安全範囲そのものは未実装なので、安全性の完成を意味しない。
- 航法の4目標上限を撤去する。同じ目標セルの距離場共有と毎tickの再構築は維持し、後続の要求も同期的に処理する。帰還判定による追加要求が発生しても本数超過で停止しない。
- 企画書に「設置光は範囲内の地形と床材を常時照らす」を明記した。これは仕様の明文化であり、新たな描画機能の実装ではない。

最初の変更では新しいシリアライズ対象フィールドやDLL ABI変更はない。操作変更は既存の音響デモに適用される。本編用の独立プロジェクトは未追加。

## 続きの実装：安全範囲と区画内の復活

- 光の設置完了時に設置順とプレイヤーの足場を登録する。最後に設置した残存光だけを死亡時に消費し、その足場へ戻る。残存光がなければ開始位置へ戻り、携行中の光は失わない。
- 回収で復活地点を解除し、置き直しで最新へ更新する。順序は1〜3の順位で保持し、繰り返し設置によるカウンターの桁溢れや順位の重複を防ぐ。
- 開始位置の取得済みフラグを追加し、開始座標が原点でも移動先で上書きしない。復活時は移動要求を止め、120tickの捕捉猶予を与える。
- 完成した光の範囲内では捕捉されず、回収途中も保護を維持する。未完成の光は保護しない。捕捉・保護・回収対象の判定で遮蔽と高さを検査する。
- `Light.safeRadius`（既定0）を反射登録する。通常照明は影響せず、設置完了時に設定し、格納・消費時に0へ戻す。
- 敵の経路計算では安全範囲と交差するセルをtick単位で除外する。範囲内の音には外側の調査目標を使い、移動要求の線分も検査する。既に範囲内にいた敵には外向きの退避入力を与える。
- 光型の待機目標を安全範囲の外側へ分散する（エンティティ昇順で8方向）。設置順・復活位置・開始位置フラグはスクリプト登録フィールドとして復元対象に含める。
- Lightの生バイトが変化するため、SimSnapshotをv13からv14へ更新した。旧スナップショットは拒否される。C ABIのAPIテーブルは変更していない。

今回の対象は既存デモの同一区画・3灯プール。区画を跨ぐ復活、永久灯の本編配置、動的生成、復活閃光・敵のひるみは未実装。敵の物理押し出しを受けた後の位置補正、重なった安全範囲からの退避も残る。待機場所・目標が辿れない場合の差し替えは下の追補で実装した。安全範囲の航法処理は単層区画の水平円として扱い、多層施設には未対応。

### 続きの検証結果

- 操作テストは26項目、`/Od`・`/O2`とも成功。設置順、回収・置き直し、連続死亡、携行品保持、壁越し捕捉防止、登録フィールドからの復元を含む。
- 安全範囲の航法・退避・消灯解除・ハッシュ・JSON保存・スナップショット復元の11項目を追加。Debug／Release全selftestで成功。
- Debug／Releaseビルド成功。PDBのRPCエラーと並列コンパイルのメモリ不足は、並列度を下げて解消した。Releaseの成功コマンドは `MSBuild.exe MyEngine.sln /p:Configuration=Release /p:Platform=x64 /p:PreferredToolArchitecture=x64 /p:CL_MPCount=2 /m:1 /v:minimal /nologo`。
- ルール検査は0 errors / 0 warnings。Debug音響Replayは600tick一致、スナップショット16往復成功。
- `tools\verify_sanko_replay.ps1` が成功。通常・部位・ゲームフロー・マルチ入力・物理・関節・音響の7シーンで各600tickがDebug／Release一致し、各Debug検証ではスナップショット16往復が成功。Debug／Releaseの400tickタイムトラベルもALL PASS。これはキャッシュを消さない検証であり、コールドクックの検証にはならない。
- 最新ログは `cache\sanko_light_selftest_debug.log`、`cache\sanko_light_selftest_release.log`、`cache\sanko_light_rules.log`、`cache\sanko_light_replay\`。実機目視・パッド実操作・画像回帰・性能測定は未実施。
- エンジン本体の変更を含むため、反映にはEditorを再起動する。旧版の生スナップショットはv14で読み込めない。

## 追補（2026-09-12）：入口を塞がれた部屋で敵が固まる

三校で「部屋の入口付近に光を設置すると敵がそこで固まる」報告。安全範囲の通行禁止で入口が閉じると、部屋の中の目標（鳴った音・巣・光の待機場所）への距離場が届かず、`SampleDirection` が進めないを返し続ける。遷移表は到達可能性を見ないため、追跡・帰還のまま永久に停止していた。立てこもりは仕様として許す。

- `AcousticNav::NearestReachable` を追加。目標へ辿れないとき、自分のセルから辿れる開セルのうち目標セルに最も近いもの（整数2乗距離、同点は走査順）を返す。
- `AgentSystem` は移動の直前に辿れない目標をこの地点へ差し替える。帰還は辿れる所まで戻れば巡回へ移る。光に引かれた追跡は、辿れる最寄りに着いたら探索へ落とす（光が見えている間は入口の前をうろつき続ける）。
- 新しいフィールド・スナップショット版・ABI の変更は無い。敵の移動が変わる場面では Replay の結果が変わる。
- 残り：安全範囲の縁で移動要求を0にする線分検査のデッドロック（縁に沿って滑らせない）は未対応。

## 追補（2026-09-13）：書架や閉じた扉の「上」を通る道で敵が詰まる

三校 stage2 で、追跡中の敵が書架（高さ2.2m）の側面に移動入力 3m/s・速度0のまま張り付いた。音響グリッドは立体なので、天井まで届かない障害物（書架・作業台・配管・木箱、閉じた扉 2.4m）の上の層は開になる。流れ場が上の層を通って障害物を越える道を張り、`SampleDirection` が返す水平成分だけを受けた敵が障害物を押し続けていた。stage2 には天井まで届かない当たり判定が42個ある。

- 流れ場の辺を「同じ層か下の層へ」に限る（登らない）。`BuildDistance` に向きの引数を足し、目標へ向かう場と `NearestReachable` の出発点から広がる場で辺の向きを分ける。`SampleDirection` も上の層の隣を選ばない。
- `BuildFlowField` は空中の目標（台の上で鳴った音・宙の光）を真下が閉じるまで落とす。落とさないと上の層の目標へ誰も着けず、差し替えが毎 tick 走り、`ReachedTarget` も層違いで偽のままになる。
- 音の伝播は変えない（音は障害物の上を越えてよい）。「音が通れる所は敵も通れる」は同じ層の中で成り立つ。
- `SampleDirection` は「選んだ隣の向き」ではなく「今の位置から選んだ隣のセル中心への向き」を返す。実測の詰まりは登りではなくこちらが本体だった：書架の角を斜めにかすめて自分の粗セルが閉になり、表の順で逃がした隣から見た -X（面へ直角）を返し続けて壁ずりが起きなかった。隣の選び方は整数比較のまま。
- `AcousticSelfTest` に 3 本（壁の上が開でも越えない / 抜け道を床の層で回る / 空中の音は床へ落ちる）。譲り合いのテストは配置を粗セルの中心線（z=0.5）へ移した（境界に置くと中心線へ寄る z 成分が乗るため。検査の意図は不変）。
- 新しいフィールド・スナップショット版・ABI の変更は無い。敵の移動が変わる場面では Replay の結果が変わる。
- 残り：階段・坂を登る敵は扱えない（登りを一律に禁じたため）。多層施設を作るときは「足場の下が閉じている層だけを歩ける」形へ広げる。

## 実装・参照先

| ファイル | 絶対パス |
|---|---|
| [操作](../src/GameLogic/Scripts/WatcherFpsCamera.cpp) | `C:\HAL\MyEngin\src\GameLogic\Scripts\WatcherFpsCamera.cpp` |
| [設置・回収](../src/GameLogic/Scripts/WatcherLightTool.cpp) | `C:\HAL\MyEngin\src\GameLogic\Scripts\WatcherLightTool.cpp` |
| [入力定義](../assets/input/actions.json) | `C:\HAL\MyEngin\assets\input\actions.json` |
| [航法宣言](../src/Engine/Engine/Acoustic/AcousticNav.h) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AcousticNav.h` |
| [航法実装](../src/Engine/Engine/Acoustic/AcousticNav.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AcousticNav.cpp` |
| [音響・敵テスト](../src/Engine/Engine/Acoustic/AcousticSelfTest.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AcousticSelfTest.cpp` |
| [操作テスト](../tools/WatcherRulesSelfTest.cpp) | `C:\HAL\MyEngin\tools\WatcherRulesSelfTest.cpp` |
| [操作テスト実行](../tools/watcher_rules_verify.bat) | `C:\HAL\MyEngin\tools\watcher_rules_verify.bat` |
| [企画書](../三校企画.md) | `C:\HAL\MyEngin\三校企画.md` |
| [音響処理（参照）](../src/Engine/Engine/Acoustic/AcousticField.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AcousticField.cpp` |
| [敵処理（参照）](../src/Engine/Engine/Acoustic/AgentSystem.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AgentSystem.cpp` |
| [コンポーネント（参照）](../src/Engine/Core/Components.h) | `C:\HAL\MyEngin\src\Engine\Core\Components.h` |
| [スクリプトAPI（参照）](../src/Shared/ScriptAPI.h) | `C:\HAL\MyEngin\src\Shared\ScriptAPI.h` |
| [Engine API（参照）](../src/Shared/EngineAPI.h) | `C:\HAL\MyEngin\src\Shared\EngineAPI.h` |
| [安全半径の登録](../src/Engine/Core/Components.cpp) | `C:\HAL\MyEngin\src\Engine\Core\Components.cpp` |
| [敵更新の宣言](../src/Engine/Engine/Acoustic/AgentSystem.h) | `C:\HAL\MyEngin\src\Engine\Engine\Acoustic\AgentSystem.h` |
| [固定時間の受け渡し](../src/Engine/Engine/TickRunner.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\TickRunner.cpp` |
| [保存形式v14](../src/Engine/Engine/Replay/SimSnapshot.h) | `C:\HAL\MyEngin\src\Engine\Engine\Replay\SimSnapshot.h` |
| [音声テストの版追随](../src/Engine/Engine/Audio/AcousticAudioSelfTest.cpp) | `C:\HAL\MyEngin\src\Engine\Engine\Audio\AcousticAudioSelfTest.cpp` |
| [削除なしのReplay検証](../tools/verify_sanko_replay.ps1) | `C:\HAL\MyEngin\tools\verify_sanko_replay.ps1` |

## 初回の検証結果

すべて `C:\HAL\MyEngin` を作業ディレクトリとして実施した。

| 検証 | 結果 |
|---|---|
| `MSBuild.exe MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo` | 終了コード0 |
| 同じコマンドの `Configuration=Release` | 終了コード0 |
| Debug / Release `Editor.exe --selftest --warp --no-audio` | 実プロセスの終了を待って両方0、最終Acoustic audioテストまでALL PASS |
| `tools\watcher_rules_verify.bat` | 実スクリプトをAPIスタブで実行。`/Od` と `/O2` の両方で12項目成功。警告はエラー扱い |
| `tools\check_rules.ps1` | 0 errors / 0 warnings |
| 音響デモの合成入力600tick記録→Debug検証（`--snapshot-stress 37`） | VERIFY PASS、16回のスナップショット往復 |
| 同じ記録→Release検証 | VERIFY PASS、600tickのハッシュ一致 |
| `git diff --check` | 成功 |

追加した航法テストは12個の異なる目標、既存ハンドルの維持、同目標共有、tick跨ぎの履歴破棄を確認する。AgentSystemテストは48m幅・0.5mセルの開放グリッド上で8体が異なる帰還目標に対して毎tick移動要求を出すことを確認する。物理移動・混雑回避や本編の性能を保証するテストではない。

全ログは `cache\sanko_selftest_debug_full.log`、`cache\sanko_selftest_release_full.log`、`cache\sanko_replay_record.log`、`cache\sanko_replay_snapshot.log`、`cache\sanko_replay_release.log`、`cache\sanko_rules.log` に保存（版管理対象外）。

`tools\replay_verify.bat` 全体は既存ファイルの削除処理を含むため実行していない。今回は音響シーンに限定して、同じ記録・照合引数を削除なしで実行した。他シーンのReplay、キャッシュを消した状態との一致、画像回帰、実パッド、実GPUの目視・性能検証は未実施。

## 計画に対する残作業

- **工程A:** 本編の開始時一人称、ジャンプを使わない本編入力、しゃがみの視点・当たり判定・天井検査、投擲と取得・扉・ポーズを含む全操作、補充・携行上限、最初の接触による投擲発音、動的な光生成、設置候補の床・壁・段差検査、復活閃光と敵のひるみ、物理押し出し後の侵入防止と滞留位置の再割り当て、調査記憶、状態別発音、秒指定の高精度残光。V切替、ジャンプ、3灯プールは残っている。区画内の設置順復活・捕捉猶予・壁越し捕捉防止・回収遮蔽は上記の続きで実装した。
- **工程B:** 永続ID、専用の版付き保存、全区画状態の停止・復元、区画を跨ぐ最後の光への復活、2区画往復、破損時バックアップ復旧。
- **工程C:** 導入と本編6区画の編集可能資産、進行条件、近道、資源ゼロでも突破できる経路、導入省略、脱出までの通しプレイ。
- **工程D:** 施設美術、敵2種、床材と専用効果音、タイトル・設定・進行表示・操作説明・結果・クレジット。
- **工程E:** 全Replay・画像回帰、実パッドとキーボードによる完走、初見テスト、実機1080p/60fps測定、権利確認、Editor不要の配布ビルド。

上限撤去後の航法コストは異なる目標数に応じて増える。ゲーム側で同時稼働敵8体という制作予算を守り、実際の間取りでCPU時間を測る必要がある。
