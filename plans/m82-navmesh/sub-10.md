# sub-10: 歩ける最大傾斜と段差の高さを設定どおりに効かせる (CC の stepOffset + Surface のセルサイズ自動決定)

- 依存: sub-03 (実行順は sub-03 の直後・sub-04 の前。sub-04 は sub-10 に依存する)
- 状態: OK (コミット待ち)
- 往復: 2

## 背景
ユーザー回答 (2026-10-03、spec 2. #19 の `[ユーザーに聞ける]` への答え): 「あるける最大傾斜や階段の高さを変更できるように」。
いまは値を設定しても次の 2 つで効かない (sub-03 の実測):
- 段差: CC に step 処理が無く、登れる高さが速度しだい (1.5 m/s で 0.15 m、3.5 m/s で 0.25 m)。Surface の `maxClimb` を上げても CC が付いてこない。
- 坂: Recast は隣接セルの高低差を `walkableClimb` で縛る (`rcFilterLedgeSpans` と `rcBuildCompactHeightfield` の隣接接続) ので、実効の坂上限は atan(maxClimb / (2·cellSize))。既定値では約 9 度。

## やること
1. **CC の段差 (`stepOffset`)**: `CharacterControllerComponent` の**末尾**に `float stepOffset` (m、hash 対象) を追加。`SolveCharacters` (`src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp:1247-1330` 付近) に step-up を足し、接地中に水平移動が段差に当たり、段差の上面が足元から `stepOffset` 以下なら**速度に関係なく**登る。上面が `slopeLimitDeg` を超える面なら登らない。
   - 既定値は **0.3** (Unity の CharacterController と同じ。ユーザー回答 2026-10-03、spec 2. #5)。フィールドの無い旧シーンの CC も 0.3 で読まれる。`stepOffset == 0` のときは従来の経路をビット単位でそのまま通すこと (分岐の追加で浮動小数点の演算順を変えない。下の切り分けの (i) はこれに依存する)。
   - インスペクタ (CC) に `stepOffset` を出す。Localization (en / ja)。範囲の制約 (負値を禁止、height を超える値の扱い) は coder が決めて SELF_EVAL に書く。
   - コンポーネントの大きさが変わるので、SimSnapshot / .rep / シーンの互換を確認する。SimSnapshot が型の生バイトを持つなら `kSimSnapshotVersion` を 25 → 26 に上げる。シーンは型名 + フィールド名で保存するので、旧シーンの CC は既定値 0.3 で読めること (SelfTest)。ABI は CC を関数経由でしか触らない (`EngineApiTable.cpp:314-333`) ので ABI の変更は無い見込み — 構造体のミラーが無いことを確かめて SELF_EVAL に書く。

1b. **既存への影響の切り分け** (spec 受け入れ条件 11 の例外を使うための手順。省略不可)
   - 対象: CC を含むデモ = `--acoustic-demo` (Walker / Watcher 系、`DemoContent.cpp:2953 / 3036 / 3096`) と `--nav-demo` (`DemoContent.cpp:4456`)。ほかに CC を足すデモが無いことを `grep CharacterControllerComponent` で確かめて SELF_EVAL に書く。SelfTest の期待値 (PhysicsSelfTest / AcousticSelfTest / NavAgentSelfTest / VfxSelfTest など CC を使うもの) も対象。
   - (0) **着手前の基準を先に取る**: コードを変える前の HEAD (`aa677a6` 以降の sub-10 着手時点) で、対象デモを `--hash-dump <file> --hash-dump-tick <T>` で複数 tick (例: 60 / 300 / 599) 取り、`cache\` に残す。SelfTest の CC 関係の期待値もこの時点の値を控える。
   - (i) **0 に強制すれば元どおり**: 変更後のビルドで、全 CC の `stepOffset` を 0 にして同じ dump を取る (一時プローブか、SelfTest 内で 0 を入れる。プローブはコミットしない)。CC の状態 (位置・回転・velocity・isGrounded) と、CC 以外の全コンポーネントが (0) と一致すること。違うのは新フィールドの行だけ。
   - (ii) **0.3 の差の出どころ**: 既定 0.3 の dump と (i) を比べ、最初に割れる tick と、そのとき割れるエンティティが CC 持ちであることを記録する (CC 以外が先に割れたら stepOffset 以外の原因 = 止まって報告)。
   - golden: 対象デモの golden は `acoustic_forward` / `acoustic_deferred` だけで、どちらも着手前から FAIL している (spec 受け入れ条件 11)。sub-10 では `--update` しない。代わりに (i) の条件で撮った画像が着手前 HEAD の actual とビット一致することを示す。`nav` golden は sub-04 で作るので、sub-10 の新しい既定のまま撮ってよい。
   - SelfTest の期待値を書き換えるときは、(i) で旧値が再現することを確かめてから書き換え、項目ごとに「旧値 → 新値、原因 = stepOffset」を SELF_EVAL に列挙する。

1c. **.rep とリプレイ資産**
   - MyEngine のリポジトリに `.rep` はコミットされていない (`git ls-files` で 0 件。`replay_verify` の `cache\golden_*.rep` は毎回録り直す)。録り直す資産は無い。
   - `kSimSnapshotVersion` を上げた場合、上げる前に録った `.rep` (利用者の手元・三校の `cache\`) は版の不一致で読み込みを拒否される (既存の方針どおり使い捨て)。拒否のログが出ることを確かめる。上げない場合 (スナップショットがフィールド単位で書かれている場合) は、旧 `.rep` を再生すると CC が 0.3 で動いて途中から割れる — そのときは版を上げて拒否させる方を選ぶ (黙って割れるより明示的に拒否する)。

1d. **外部プロジェクトの確認** (データは書き換えない。シーン・golden・スクリプトの変更はユーザーが決める)
   - 三校 (`C:\HAL\三校`、`assets\scenes\main.scene.json` に CC 2 個): **coder が実行できる** — `MYE_ENGINE=C:\HAL\MyEngin` を指定して `tools\verify.bat` (shot + replay、既定値 `..\My_Engin\MyEngin` はこの PC と合わないので必ず指定)。replay は PASS が必須。shot の `main.png` が割れたら更新せず、差分画像のパスと、1b (i) と同じ方法で「0 に強制すれば一致」を示す。加えて main シーンを (i) / (ii) の dump で比べ、軌跡が変わる CC のエンティティ名・最初に割れる tick・そのときの位置を一覧にする (ユーザーが目で見る場所の手がかり)。
   - HAL Collector (`C:\HAL\GameEngin_Demo`、CC は `assets\scenes\main.scene.json` と `assets\prefabs\enemy.actor.json`): 検証スクリプトが無いので、**coder は** `Runtime.exe --project C:\HAL\GameEngin_Demo` (ヘッドレスでも可) で (i) / (ii) の dump を取り、同じ形の一覧を作るところまで。
   - **ユーザーの目視が要る範囲**: 両ゲームを Editor で Play し、プレイヤー / 敵が高さ 0.3 m 以下の物 (段差・敷居・箱・台) に乗り上がるようになった箇所が、ゲームとして困るかを見る。困る箇所の対処 (その CC の `stepOffset` を下げる / 障害物を高くする) は外部プロジェクト側の作業で、M82 の範囲外。coder は上の一覧を `plans\m82-navmesh\sub-10-external-check.md` に書いて渡す (申し送りの文書。ユーザーが目視の順番に使う)。
2. **Surface の既定値と、セルサイズの自動決定**: `NavMeshSurfaceComponent` の**末尾**に `bool autoCellSize = true` を追加 (hash 対象)。
   - `autoCellSize` が true なら、ベイク時のセルサイズを cs = min(agentRadius / 2, maxClimb / (2·tan(maxSlopeDeg))) とし、下限 `kNavMinCellSize` (0.05 m 目安。coder が根拠つきで決めてよい) で止める。下限に当たったら実効の坂上限が `maxSlopeDeg` を下回るので、ベイク結果とインスペクタで警告する。false なら従来どおり `cellSize` をそのまま使う。
   - `maxClimb` の既定を 0.1 → **0.3** にする (CC の自動付与の `stepOffset` 0.3 と揃える)。既定値どうしで cs = min(0.15, 0.3 / 2) = 0.15、実効の坂上限 = 45 度。
   - タイル 1 枚のセル数 (`tileSize`) は cs が半分になるとタイルの実寸も半分になる。ベイク時間とタイル数を 20×10×20 の既定範囲で測り、既定の `tileSize` を据え置くか上げるかを数字つきで決める。
   - 入力ハッシュ (`NavComputeInputHash`) と `kNavBakeVersion` は、実際に使ったセルサイズが入ることを確かめる (自動決定に変えた結果、同じ設定値から違う .mnav ができないこと)。
3. **インスペクタ (Surface)**: 実際に使うセルサイズ (自動のときは計算値) と、実効の坂上限 atan(maxClimb / (2·cs)) を表示する。`maxSlopeDeg` が実効の坂上限を超えたら警告する。Agent 側の警告 (sub-03 の寸法不整合) に「CC の `stepOffset` < Surface の `maxClimb`」を足す (経路はあるのに登れない)。
4. **既存のテストとデモを新しい既定値に合わせる**: NavAgentSelfTest の庭 (いまは 10 度 / cellSize 0.2 で成立させている) に **30 度以上の坂**と **`maxClimb` ちょうどの段差**を足し、既定の Surface 設定 + `stepOffset` 0.3 の CC で `Arrived` になること。`maxClimb` を少し超える段差は経路にならないこと。NavSurface の期待ハッシュ (`kExpectedAssetHash`)、`--nav-demo`、`nav` ジョブを録り直し、動いた理由を SELF_EVAL に書く。
5. sub-03 の `Components.h` の `maxSlopeDeg` コメント (制約の注記) を新しい挙動に合わせて書き直す。

6. (round 2 で追加、spec 2. #20) **cellHeight の自動決定**: `autoCellSize` が true のときは、cellHeight も ch = min(cs / 2, maxClimb / 6) で決める (既定では 0.05)。下限は coder が根拠つきで決める。Inspector の『セル … (自動)』の表示に cellHeight を足す。`autoCellSize` が true のときは cellSize / cellHeight の入力欄を無効表示にする (round 1 の申し送り)。NavSurfaceSelfTest の固定ケース (`autoCellSize=false`) は動かさない。
   - 庭の『maxClimb を少し超える段差は経路にならない』を、+5 cm (0.35 m) に戻して検査する。地面の天面がボクセル境界ちょうどの配置と、境界から半セルずらした配置の両方で行う。
   - 既定の Surface (ch 0.05) でのベイク時間とサイズを、round 1 の表と同じ条件 (20x10x20 + 箱 12 個、Release) で測る。tileSize 48 を据え置くか、数字で決め直す。
   - Agent の警告 (`stepOffset < maxClimb`) を画面で確かめる (Inspector の窓を高くするか、スクロールして撮る)。

7. (round 2 で追加、spec 2. #21、ユーザー回答 2026-10-04) **stepOffset に scale を掛ける**: 実効の段差 = stepOffset × |scale.y| (ワールドの scale、親を含む。`PhysicsSystem.cpp:1783` の height と同じ取り方)。上限は実効の全高。Agent の警告 (`NavAgentStepBelowClimb`) も実効値と Surface の maxClimb を比べる。`Components.h` の stepOffset のコメントと、round 1 で入れた「scale は掛けない」の説明 (`PhysicsSystem.cpp:1786` 付近) を書き直す。
   - **acoustic の切り分け** (1b の手順を scale 込みでやり直す): (i) 全 CC を 0 に強制した実行が、着手前 HEAD (`cache\base10_rel\` の dump) と一致することを再確認する。(ii) 既定 0.3 で、`--acoustic-demo` の最初に割れる tick とエンティティを記録する。期待は tick 26 前後の Agent Eye (round 1 の実測)。最初に割れるのが CC を持たないエンティティなら、作業を止めて報告する。Agent Eye 以外の CC が先に割れた場合は止めず、名前と tick を記録する。
   - ほかのシーンも (ii) をやり直す: `--nav-demo`、三校、HAL Collector で scale ≠ 1 の CC が割れるかを調べ、`sub-10-external-check.md` の「軌跡が変わる CC」の一覧を更新する。
   - **golden**: `acoustic_forward` / `acoustic_deferred` は着手前から FAIL しているので `--update` しない (別の原因の FAIL を塗り潰さない)。0 に強制して撮った画像が着手前 HEAD の actual とバイト一致することを示す。既定で撮った画像の差 (maxDiff と画素数) を、着手前の FAIL 値と並べて SELF_EVAL に書く。後で誰かが acoustic の golden を撮り直すときは、この変化も含まれることを台帳へ申し送る。
   - **replay_verify**: acoustic を含む全ジョブの PASS は必須 (毎回録り直す自己照合なので、変化があっても Debug / Release / Server と snapshot stress は一致しなければならない)。
   - 段差登りの SelfTest (PhysicsSelfTest) に、scale.y 2 の CC で stepOffset × 2 の段差を登り、それ以上は登らないケースを足す。AcousticSelfTest などの期待値が動いたら「旧値 → 新値、原因 = stepOffset × scale」で列挙する (0 に強制すれば旧値が再現することを確認してから書き換える)。

## やらないこと (このサブでは)
- Recast 本体のパッチで坂と段差を分けること (spec 2. #19 で却下: ボクセルの上では坂と段差を区別する情報が無い)
- 外部プロジェクト (三校 / HAL Collector) のシーン・golden・スクリプトの書き換えとコミット
- 着手前から FAIL している golden 5 枚の `--update`
- 塗り・golden `nav` (sub-04)

## 触る場所 (planner の見立て)
- `src\Engine\Core\Ecs\Components.h/.cpp` (CC と Surface の末尾フィールド、登録)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (`SolveCharacters`)、`PhysicsSelfTest.cpp`
- `src\Engine\Engine\Navigation\NavBakeInput.cpp` (`NavMakeBakeConfig` のセルサイズ)、`NavBake.cpp` (入力ハッシュ)、`NavMeshAsset.h` (`kNavBakeVersion`)、`NavAgentSelfTest.cpp`、`NavSurfaceSelfTest.cpp`、`DemoContent.cpp`
- `src\Engine\Engine\Replay\SimSnapshot.h` (版、必要なら)
- `src\Editor\Scene\ComponentDependencies.cpp`、`src\Editor\Windows\Scene\InspectorWindow.cpp`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 19) `stepOffset` = h の CC が、高さ h の段差を 0.5 m/s 以上のどの速度でも登り、h + 0.05 m の段差は登らない。既定は 0.3。— PhysicsSelfTest
1b. (spec 11) 1b の切り分けの (0)〜(ii) が記録されている: 0 に強制した実行が着手前 HEAD と一致し、0.3 での最初の差が CC 持ちのエンティティから出る。書き換えた SelfTest の期待値が「旧値 → 新値」で列挙されている。replay_verify 全ジョブ PASS (CC を含むジョブも、Debug / Release / Server の照合と snapshot stress を含む)。— dump の比較結果 + `tools\replay_verify.bat`
1c. 外部プロジェクト: 三校の `tools\verify.bat` の replay が PASS、shot は一致か「0 に強制すれば一致」。HAL Collector と三校の「軌跡が変わる CC の一覧」が `plans\m82-navmesh\sub-10-external-check.md` にある。ユーザーの目視は sub-10 の合否に含めない (未確認として台帳へ申し送る)。
2. (spec 6) 既定の Surface 設定 (`autoCellSize` on、maxClimb 0.3、maxSlopeDeg 45) で、30 度以上の坂と 0.3 m の段差を越えて Agent が `Arrived` になる。`maxSlopeDeg` を下げると、その坂は経路から外れる。— NavAgentSelfTest
3. Surface のインスペクタに、実際のセルサイズと実効の坂上限が出る。設定が実効の上限を超えると警告が出る。CC の `stepOffset` < `maxClimb` の Agent に警告が出る。— NavEditorSelfTest で警告の判定関数を検査 + Editor のスクショ (一時プローブ可) か手動確認の画像パス
4. 旧シーン (CC に `stepOffset` が無い) を読むと 0.3 になる。`kSimSnapshotVersion` を上げたなら、旧版の `.rep` が拒否されてログが出る。— SelfTest
5. Debug / Release / Server でハッシュ一致 (NavAgent / NavSurface の期待値、`nav` ジョブ)。0 警告、check_rules 0、shot_verify は既存の FAIL 5 枚以外が不変。

## 検証コマンド
- 両構成ビルド、`Editor.exe --selftest` 両構成、`Server.exe --selftest` 両構成
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`
- `bin\x64\Release\Runtime.exe --nav-demo --screenshot` (坂を足したなら坂の上の輪郭が切れていないこと)
- 着手前 HEAD のビルド (git worktree 等。sub-02 で前例あり) と `--hash-dump` / `--hash-dump-tick` での比較 (1b)
- 三校: `MYE_ENGINE=C:\HAL\MyEngin` で `C:\HAL\三校\tools\verify.bat` (cmd 経由で起動。PowerShell から exe を直接呼ばない)
- HAL Collector: `Runtime.exe --project C:\HAL\GameEngin_Demo` + `--hash-dump`

## 実装メモ (coder が追記)

### SELF_EVAL: sub-10 (round 1)

実装:
  - src/Engine/Core/Ecs/Components.h/.cpp: CharacterControllerComponent の末尾へ `stepOffset` (既定 0.3、ワールド m、範囲 0..5、hash 対象) を追加。NavMeshSurfaceComponent の末尾へ `bool autoCellSize = true`、`maxClimb` の既定 0.1 -> 0.3、`tileSize` の既定 32 -> 48。maxSlopeDeg / cellSize / maxClimb のコメントを新しい挙動に書き直した (sub-10 の 5)
  - src/Engine/Engine/Physics/Rigid/PhysicsSystem.cpp: `TryCharStepUp` を新設し `SolveCharacters` に接続。前 tick に接地・ジャンプなし・stepOffset > 0 のキャラが、水平移動の進みが歩幅の半分未満のときだけ「stepOffset だけ持ち上げる -> 前へ出す (歩幅から半径まで 5 段階) -> 真下へ着地点を二分探索 (12 回固定)」を試す。着地面の法線が slopeLimit 以内で、接触点の高さ (足元基準) が 0.02 < h <= stepOffset + 5 mm のときだけ採用。stepOffset 0 / 条件を満たさない tick は従来の経路をそのまま通る (従来の押し出し・接地プローブのコードは 1 文字も動かしていない)
  - src/Engine/Engine/Replay/SimSnapshot.h: kSimSnapshotVersion 25 -> 26 (World 節の生バイトが伸びる)
  - src/Engine/Engine/Navigation/NavBakeInput.h/.cpp: `kNavMinCellSize` (0.05)、`NavResolveCellSize` (autoCellSize なら min(agentRadius/2, climb/(2 tan maxSlopeDeg))、下限で止めて clampedToMinimum)、`NavEffectiveSlopeLimitDeg`、警告の判定 `NavSurfaceSlopeUnreachable` / `NavAgentStepBelowClimb`。`NavMakeBakeConfig` は解決後のセルサイズを入れる (入力ハッシュは NavBakeConfig を畳むので実際に使った値が入る。kNavBakeVersion は据え置き: 同じ設定値でのベイク方式は変わっていない)
  - src/Editor/Windows/Scene/InspectorWindow.cpp + LocalizationTable.inl (en/ja): Surface に「セル 0.150 m (自動) / 実効の傾斜上限 45.0 度」と、下限到達 / 傾斜超過の警告 (折り返し付き)。Agent に「CC.stepOffset < Surface.maxClimb」の警告
  - src/Engine/Engine/Demo/DemoContent.cpp: --nav-demo を段差 0.3 (maxClimb ちょうど) / 坂 30 度 / 台 (x 9.0 half 1.8) に変更、Surface は既定設定 (cellSize / tileSize の明示を削除)
  - テスト: PhysicsSelfTest (20b 段差登り 10 項目)、NavAgentSelfTest (庭を 0.3 段差 + 30 度坂 + 0.4 の台 + 傾斜 45/20 度の切替に作り直し、MeasureClimb を stepOffset 引数化、期待ハッシュ更新)、NavSurfaceSelfTest (2b セルサイズ自動決定と入力ハッシュ、旧シーン互換 = stepOffset/autoCellSize 欠落で 0.3/true、固定の旧設定は fixture 側で autoCellSize=false/maxClimb 0.1)、NavEditorSelfTest (1b 警告判定)、SimSnapshotSelfTest (旧版 blob の拒否)、AcousticAudioSelfTest (版の期待 25 -> 26)

仕様との差分:
  - [追加] stepOffset はワールド m で持ち、Transform の scale を掛けない (radius / height は掛かる)。理由: 掛けると scale.y 1.6 の acoustic デモの敵 (Agent Eye) の実効段差が 0.48 になり、衝撃板 (天面 0.45) に乗り上がって着手前と軌跡が変わった (実測。tick 26 で割れた)。掛けなければ acoustic / nav / 三校 / HAL Collector のサンプル全点で差なし。上限は CC の全高 (小さい CC で巨大な stepOffset が壁を登らない)、0 以下 / NaN は「登らない」
  - [追加] 段差登りの前へ出す量は歩幅から最大「歩幅 + 半径」まで 1/4 半径刻みで試す (低速でも 1 tick で登る = 受け入れ 1 の「0.5 m/s 以上のどの速度でも」を満たすための仕様の穴埋め)。角に載った状態で登り切るには足の中心が角の手前 r*sin(傾斜) 以内に入る必要があり、壁際で止まったキャラは歩幅だけでは届かない。副作用: 登る tick に最大で半径程度 (0.3 m) の前進が 1 回入る (velocity.x/z がその tick だけ大きく出る。足音の歩幅計算には効くが 1 歩分)
  - [追加] 「接触点の高さ」は着地姿勢から ClosestPointOnShape で求めた障害物表面の点を足元 (c.py - halfSeg - radius) と比べる。角に半端に載っただけの姿勢 (足は stepOffset 以内だが表面が高い) を登らせないため
  - [追加] NavResolveCellSize / NavEffectiveSlopeLimitDeg の climb は生の maxClimb ではなく Recast に渡る floor(maxClimb / cellHeight) * cellHeight。既定 (0.3 / 0.1) では同じ値。maxClimb 0.35 のような設定で実効の坂が過大に出ないように
  - [追加] tileSize の既定を 32 -> 48 に上げた (sub-10 の 2 は「据え置くか上げるか数字つきで決める」)。20 x 10 x 20 の床 + 箱 12 個を Release で計測: cs 0.3 / tile 32 (旧既定) = 3x3 タイル 2.1 ms 28 KB; cs 0.15 / tile 32 = 5x5 タイル 25 層 6.3 ms 79 KB; **tile 48 = 3x3 タイル 9 層 5.0 ms 63 KB**; 64 = 3x3 5.3 ms 111 KB; 96 = 2x2 4.9 ms 111 KB; 128 = 2x2 5.7 ms 197 KB。48 が旧既定と同じタイル数で最速・最小
  - [逸脱] 「maxClimb を少し超える段差は経路にならない」の NavAgentSelfTest は +5 cm (0.35) ではなく +1 セル高 (0.4) で検査した。理由: Recast は段差をボクセル (cellHeight 0.1) の整数で比べる。庭は地面の天面がボクセル境界ちょうど (y=0) で、0.35 は 3.5 セル = ceil で 3 セル差になり接続されてしまった (実測: 0.35 の台は Moving のまま台の縁で押し続ける)。0.3 ちょうどは 3 セル差で接続 (受け入れ 2 を満たす)、0.4 は 4 セル差で切れる。CC 側の検査 (PhysicsSelfTest / MeasureClimb) は 0.35 で「登れない」を確認済み。不安・質問 1 を参照
  - [追加] SimSnapshotSelfTest に旧版 blob の拒否検査、AcousticAudioSelfTest の版の期待値を 26 に (kSimSnapshotVersion を上げたことによる追随)

検証:
  - ビルド: Release / Debug とも警告 0・エラー 0 (replay_verify.bat 内と単独の両方)
  - `Editor.exe --selftest` Release / Debug -> **既知の FAIL 2 件 (Source control self test の cherry-pick / revert) だけ**。Fracture weight cache / DllReloader 停止 / Server-client net V1 の flake は再現せず。NavAgent / NavSurface / NavEditor / SimSnapshot / Physics / AcousticAudio は ALL PASS。NavAgent の庭のワールドハッシュは Debug = Release = 7C631F3532A75C26 (焼いた)
  - `Server.exe --selftest` Release / Debug -> ALL PASS (exit 0)
  - `tools\replay_verify.bat` -> 全 15 ジョブ PASS (Debug record -> snapshot stress verify -> Release -> Server、nav ジョブ含む、静的規則 0 error / 0 warning)。2 回 (中間 + 最終に近いコードで) 実施
  - `tools\check_rules.ps1` -> replay_verify 内の rules ジョブで 0 error / 0 warning
  - `tools\shot_verify.bat` -> 着手前と同じ 5 枚だけ FAIL (maxDiff 198/208/83/82/150、画素数も着手前と同一)。**撮り直した全 PNG が着手前 HEAD の actual とバイト一致** (比較で相違 0 件)。`--update` はしていない
  - 1b (0): 着手前 HEAD のビルドを cache\base10_rel\ に退避し、acoustic / nav / 三校 / HAL Collector の dump を 60/300/599 で保存
  - 1b (i): stepOffset を一時プローブ (全 CC を 0 に強制 + nav デモを旧設定) で 0 にして dump -> 4 シーン x 3 tick 全てで、新フィールドの行 (stepOffset / autoCellSize) と #entity 行 (それを畳む。CC / Surface 持ちだけ) 以外が着手前と一致。さらに旧 NavAgentSelfTest (HEAD 版 + 全 CC を stepOffset 0 / autoCellSize false) を新エンジンで走らせると、[climb] 19 行・[yard] 位置・[pass] 最小距離・Nav 節 38782 bytes が着手前とバイト一致 (世界ハッシュだけ D97085D12C7C561C -> 2FAD71AB74A29EAE: 新フィールドを畳むため。スナップショットが 24 bytes = 6 CC x 4 bytes 増える)
  - 1b (ii): 既定 0.3 で acoustic (12 点) / 旧 nav デモ (12 点) / 三校 verify シーン (18 点) / HAL Collector main (18 点) / HAL Collector title ブート (5 点) を着手前と比較 -> **全点で差なし**。最初に割れる tick は存在しない (stepOffset を scale 倍にする実装では tick 26 に acoustic の Agent Eye から割れていたが、scale を掛けない仕様にして消えた)
  - SelfTest の期待値の書き換え (旧値 -> 新値): NavAgentSelfTest `kExpectedYardHash` D97085D12C7C561C -> 7C631F3532A75C26 (原因 = 庭の作り直し (段差 0.3 / 坂 30 度 / 台 / Deck) + 既定 Surface 設定 + 新フィールドの畳み込み。stepOffset 自体の差ではない。上の (i) の旧 fixture 実行で軌跡の一致を確認済み)。AcousticAudioSelfTest の `kSimSnapshotVersion == 25` -> 26 (原因 = 版)。NavSurfaceSelfTest の `kExpectedAssetHash` A9EF6D223C161FE4 は**不変** (fixture を旧設定のまま = ベイク方式が変わっていないことの証拠)。ほかの SelfTest の期待値は 1 つも書き換えていない
  - 1c: 版を 25 -> 26 に上げた。着手前ビルドで録った .rep (サーバ selftest の desync バンドル `local.rep`、snapshot 6662 bytes) を新 Runtime で `--replay-verify` -> `[snapshot] incompatible blob (v25, input 112 bytes)` / `[replay] embedded snapshot could not be restored` で exit 1 (明示拒否)。スナップショットを持たない旧 .rep (デモの録画) は tick 0 の `HASH MISMATCH` で止まる (黙って途中から割れない)。SimSnapshotSelfTest にも旧版 blob の拒否を固定
  - 1d: `plans\m82-navmesh\sub-10-external-check.md` に結果。三校 verify.bat は着手前 HEAD でも FAIL (shot の golden 不一致 + 「敵が巡回を出ない」)。画像はバイト一致、replay のハッシュ照合は PASS。HAL Collector は Runtime の dump 比較で差なし
  - `--nav-demo` スクリーンショット (Release、frame 120 / 500): cache\s10\navdemo_120.png / navdemo_500.png。坂 (30 度) の面にも輪郭の線が乗り、台の輪郭が切れていない。Inspector の Surface (自動 / 手動 + 傾斜超過の警告) は Editor の `--screenshot`: cache\s10\insp_surface.png / insp_warn2_NavMeshSurface.png。**Agent 側の警告 (stepOffset < maxClimb) は Inspector の高さが足りず画像に写らなかった** (判定関数は NavEditorSelfTest で検査済み)
  - 変異検査: 着地面の傾斜判定 (supportNy < cosSlope) を無効化すると PhysicsSelfTest の「急な面へ登らない」と NavAgentSelfTest の台のテストが FAIL する (元に戻して確認済み)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1/1b/1c/2/3/4/5 を満たす。ただし「+5 cm の段差は経路にならない」は Recast のボクセル量子化で +1 セルに緩めた (逸脱)。Agent 警告の画像確認は未
  正しさ: 4 — 段差登りは速度 0.5〜6 m/s・段高 0.2/0.3/0.35・斜面・壁・天井・stepOffset 0/負/巨大を物理テストで固定し、傾斜判定を外す変異で FAIL することを確認。既存 4 シーンのサンプル全点で挙動不変。実プレイ (入力あり) の目視は未
  コード品質: 4 — 従来の押し出し経路を動かさず、段差登りは独立関数 1 つ + 呼び出し 1 か所。定数は名前付き。二分探索 12 回・5 段階は固定回数で決定論
  テスト: 4 — 物理 / ナビ / 警告判定 / 旧シーン / 旧 blob を固定。Agent 警告の画面確認と、実プレイでの段差登りの見た目 (1 tick に最大半径ぶん前へ出る) は未確認

不安・質問:
  1. (Recast の段差量子化) 実効の段差は cellHeight の整数倍で量子化され、地面の天面がボクセル境界ちょうどにあると maxClimb + 1 セル未満 (庭では 0.35) の段差もナビメッシュでは接続される。すると CC (stepOffset 0.3) が登れない段差を越える経路が作られ、Agent は縁で押し続ける (status は Moving のまま)。対策案: (a) cellHeight を既定で細かく (0.05) し autoCellSize 時は cellHeight も maxClimb/6 程度へ自動化、(b) 現状を制約として Inspector に注記、(c) NavSystem 側で「前進できていない」を検出して NoPath 扱い (sub-05 以降)。planner の判断を求める
  2. (stepOffset のスケール) ワールド m で scale を掛けない仕様にした (差分欄)。Unity の挙動に合わせて掛けるなら、acoustic デモの Agent Eye (scale.y 1.6) が衝撃板 (0.45) に乗り上がり acoustic の golden / replay が変わる。ユーザーに確認が要るなら `[ユーザーに聞ける]`
  3. 登る tick の前進 (最大 歩幅 + 半径) の見た目: 壁際で止まっていたキャラが段に当たった瞬間に最大 0.3 m 前へ出る。ゲーム側で気になるなら半径刻みを細かく / 上限を下げる調整は可能 (定数 kStepAdvanceTries)
  4. 三校 verify.bat が着手前 HEAD から FAIL している (shot golden 不一致 / 敵が巡回を出ない)。外部プロジェクト側の既存問題で M82 の範囲外だが、台帳の申し送りに載せてほしい
  5. 台帳 harness.md の既知 flake (Fracture weight cache / DllReloader 停止 / Server-client net V1) は今回 Release / Debug の Editor・Server selftest とも再現しなかった

触ったファイル:
  src/Engine/Core/Ecs/Components.h, src/Engine/Core/Ecs/Components.cpp,
  src/Engine/Core/Localization/LocalizationTable.inl,
  src/Engine/Engine/Physics/Rigid/PhysicsSystem.cpp, src/Engine/Engine/Physics/Rigid/PhysicsSelfTest.cpp,
  src/Engine/Engine/Replay/SimSnapshot.h, src/Engine/Engine/Replay/SimSnapshotSelfTest.cpp,
  src/Engine/Engine/Navigation/NavBakeInput.h, src/Engine/Engine/Navigation/NavBakeInput.cpp,
  src/Engine/Engine/Navigation/NavAgentSelfTest.cpp, src/Engine/Engine/Navigation/NavSurfaceSelfTest.cpp,
  src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp, src/Engine/Engine/Demo/DemoContent.cpp,
  src/Editor/Windows/Scene/InspectorWindow.cpp, src/Editor/Tools/NavEditorSelfTest.cpp,
  plans/m82-navmesh/sub-10-external-check.md (新規), plans/m82-navmesh/sub-10.md (実装メモ節のみ)

申し送り:
  - ABI: 変更なし。CC は EngineApiTable.cpp の関数経由でしか触らず (構造体のミラー・C# 側の定義なし)。ABI 版 v23 のまま。ただし C++ スクリプト DLL (GameLogic.dll) は Components.h を include するので外部プロジェクトの DLL は焼き直しが要る (三校 / HAL Collector の cache\GameLogic.dll はもともと v22 で読み込めない状態だった)
  - sub-04: golden `nav` は新しい既定 (庭: 段差 0.3・坂 30 度・自動セル 0.15・tileSize 48) のまま撮ってよい。--nav-demo の絵は着手前から変わっている (坂が急になり台が高くなった)
  - 外部プロジェクト: 三校の cache\GameLogic.dll を build_scripts.bat で焼き直した (git 管理外)。HAL Collector は cache\s10\collector\ にコピーして焼いた (元は無変更)。scratch は C:\HAL\MyEngin\cache\s10\ に残してある (git 管理外。削除は承認が要るので残置): base10_rel (着手前 HEAD の exe 36 MB)、collector (HAL Collector のコピー 26 MB) ほか dump / 画像 / ログ。cache\base10_rel\ も同様
  - Inspector: Surface の cellSize フィールドは autoCellSize が true のとき使われないが、灰色にならず編集できる。必要なら sub-04 で表示条件を付ける
  - NavMeshAgent が台の縁で Moving のまま押し続ける現象 (不安 1) は sub-03 の到着判定 (ナビメッシュの縁と CC の停止位置の差が stoppingDistance を超える) の既存挙動。sub-05 以降で扱うなら planner 判断

### SELF_EVAL: sub-10 (round 2)

実装 (指摘番号ごと):
  - #1 cellHeight の自動決定: src/Engine/Engine/Navigation/NavBakeInput.h/.cpp。`NavCellSize` に `cellHeight` を追加。autoCellSize が true のとき目標 = max(0.02, min(cs / 2, maxClimb / 6)) を、maxClimb がちょうど整数セルになる分割数 N = ceil(maxClimb / 目標 - 1e-3) に丸めて ch = maxClimb / N x 0.9999 (既定 0.3 / 6 -> 0.05)。0.9999 は maxClimb / ch が 5.9999995 になって Recast の floor で 5 セルに落ちるのを防ぐ余裕 (0.45 / 0.075 で実測した浮動小数点の落とし穴)。下限 0.02 は Surface.cellHeight の最小値。`NavMakeBakeConfig` は解決後の cellHeight を入れる (入力ハッシュに実際の値が入る)。実効の坂の上限も解決後の ch で量子化した climb を使う。
    Inspector: 「セル 0.150 m、高さ 0.050 m (自動) / 実効の傾斜上限 45.0 度」(折り返し付き)。autoCellSize が true のとき cellSize / cellHeight を無効表示 (nit #5)。
    庭の検査を +5 cm (0.35) に戻し、地面の天面がボクセル境界ちょうど (yShift 0) と半セル (0.025) ずれの両方で、maxClimb ちょうど (0.3) は台の上へ Arrived、0.35 は部分経路で登らない (NavAgentSelfTest 1c)。NavSurfaceSelfTest の固定ケース (autoCellSize=false) と kExpectedAssetHash A9EF6D223C161FE4 は不変。
  - #2 stepOffset x |scale.y|: PhysicsSystem.cpp の収集で `stepHeight = min(stepOffset * asy, 全高)` (asy は height と同じ取り方のワールド |scale.y|、親を含む。radius の水平 max 規則は使わない)。0 以下 / NaN は登らない。Components.h のコメントと PhysicsSystem.cpp の「scale は掛けない」説明を書き直した。`NavAgentStepBelowClimb(cc, worldScaleY, surface)` が実効値と maxClimb を比べる (Inspector はエンティティのワールド行列の Y 行の長さを渡す)。PhysicsSelfTest: scale.y 2 の CC が 0.6 を登り 0.65 を登らない / scale.y 0.5 は 0.15 を登り 0.2 を登らない、を追加。NavEditorSelfTest の警告判定にも scale 込みのケースを追加。
  - #3 登る tick の velocity: SolveCharacters で、段差を登った tick だけ水平 velocity の大きさを moveInput の速さに頭打ち (登りのための前進を移動速度に数えない)。登っていない tick のコードは不変。
  - #4 Agent 側の警告の画面確認: 画像 C:\HAL\MyEngin\cache\s10\insp_agent_warn.png (Editor --screenshot、ウィンドウ 1000x2100。CC の stepOffset を 0.1 にした cache\s10\nav_warn.scene.json の NavAgent)。「CharacterController step (stepOffset x scale.y = 0.10 m) is below the surface's Max Climb (0.30 m)」が出ている。Surface 側: cache\s10\insp_surface2.png (自動表示 + 無効表示の cellSize / cellHeight)、insp_warn2_NavMeshSurface.png (手動セル + 傾斜超過の警告)。

仕様との差分:
  - [逸脱] round 1 の「stepOffset は scale を掛けない」を撤回し、掛ける (FIX_REQUEST #2、ユーザー回答)。
  - [追加] cellHeight の丸め (分割数 N と 0.9999 の余裕)。単純な maxClimb / 6 だと、maxSlopeDeg 60 のように cs が小さくなる設定で ch = cs / 2 が選ばれ、maxClimb / ch が整数にならず実効の climb が floor で最大 1 セル欠けて、実効の坂が設定を下回った (60 度 -> 56 度、2b の検査で検出)。N への丸めで maxClimb をちょうど整数セルにした。
  - [追加] 段差を登った tick の水平 velocity の頭打ち (should #3 の対処)。
  - [追加] Surface の cellSize / cellHeight の無効表示は、汎用の DrawField に「NavMeshSurface かつ autoCellSize」の条件を 1 か所足して実現 (フィールドメタデータには動的な無効条件が無いため)。
  - 変えていない: 1b の切り分け方法、SimSnapshot v26 と旧 .rep の拒否、SelfTest 期待値の書き換え 3 件、段差登りの構造 (独立関数・固定回数)、shot_verify のバイト一致での示し方。

検証:
  - tileSize の据え置き判断 (round 1 と同じ条件、20x10x20 + 箱 12 個、Release、最速 3 回): 旧既定 cs0.3 / ch0.1 / tile32 = 3x3 タイル 9 層 2.1 ms 28 KB; 新既定 cs0.15 / ch0.05: tile32 = 5x5 25 層 6.6 ms 79 KB; **tile48 = 3x3 9 層 5.7 ms 63 KB**; tile64 = 3x3 6.0 ms 111 KB; tile96 = 2x2 5.7 ms 111 KB; tile128 = 2x2 6.7 ms 197 KB。ch が 0.1 -> 0.05 になってもタイル数・サイズは同じで時間が約 10% 増えただけ。**48 を据え置き**。
  - 登る tick の移動量と velocity (PhysicsSelfTest のログ、stepOffset 0.3 の CC で 0.15 m の段): 0.5 m/s -> 歩幅 0.0083 m のところ登る tick は 0.158 m 動く (頭打ち前の velocity は 9.5 m/s)。3.5 m/s -> 歩幅 0.0583 m のところ 0.133 m (同 8.0 m/s)。頭打ち後は全 tick で水平 velocity <= moveInput の速さ (最大 0.50 / 3.50 m/s) を検査に入れた。位置は 1 tick で 0.13〜0.16 m 前へ出る (見た目の跳びは残る)。
  - ビルド: Release / Debug とも警告 0・エラー 0。
  - Editor.exe --selftest Release / Debug: 既知の FAIL 2 件 (Source control の cherry-pick / revert) だけ。NavAgent / NavSurface / NavEditor / SimSnapshot / Physics / AcousticAudio / AcousticSelfTest は ALL PASS。庭のワールドハッシュ Debug = Release = 2ABC7F449D843943。Server.exe --selftest Release / Debug: exit 0。
  - tools\replay_verify.bat (acoustic と nav を含む全 15 ジョブ): PASS。Debug / Release / Server の照合と snapshot stress とも一致。静的規則 0 error / 0 warning。
  - 1b (i) (scale 込みでやり直し): 全 CC の実効段差を 0 に強制した実行ファイル (一時プローブ、コミットしない) の dump を、着手前 HEAD の実行ファイルの dump と比較 -> acoustic 17 点 + HAL Collector 15 点 + 三校 15 点 = **47 点すべて差 0** (新フィールドの行と #entity 行を除く)。
    golden の画像: 0 に強制した実行ファイルで撮った acoustic_forward / acoustic_deferred は、着手前 HEAD の actual と**バイト一致** (cache\s10\ac_forward_forced0.png / ac_deferred_forced0.png)。
  - 1b (ii): 既定 0.3 x scale.y。
    - `--acoustic-demo`: **tick 26 に Agent Eye から割れる** (tick 25 まで一致。期待どおり)。Eye の足音の波スロットが tick 150 で 1 ボクセルずれ、Agent Ear の耳 (lastHeardPos / target) が割れ、CC の位置は 250 以降。CC を持たないエンティティから割れた例は無い (Agent Eye 以外の CC が先に割れた例も無い)。
    - `--nav-demo` (新しい庭): **tick 70 / entity 13:1 (NavAgent の 2 体目)** から (tick 68 まで一致)。tick 72 には 6 体と NavSystem の状態に連鎖。
    - 三校 verify シーン (15 点) / HAL Collector main (15 点): 差なし。scale != 1 の CC (三校 AgentEar scale.y 1.6) も差なし。
    - 結果は sub-10-external-check.md の表に更新。
  - shot_verify: 着手前と同じ 5 枚だけ FAIL。**既定 (0.3 x scale.y) で撮った全 PNG が着手前 HEAD の actual とバイト一致** (相違 0 件)。FAIL 値 (maxDiff / 画素数) を着手前と並べる: acoustic_forward / acoustic_deferred を含む 5 枚が 着手前 198/3625、208/137、83/596、82/594、150/192 -> 今回 198/3625、208/137、83/596、82/594、150/192 (同一)。つまり acoustic の golden 画像は frame 120 の撮影では Agent Eye の違いが 1 画素にも出なかった (replay の dump は tick 26 以降割れる。撮影実行は物理が同じ tick 数だが画面外 / 隠れる位置にいるためと推測、未調査)。`--update` はしていない。
  - SelfTest 期待値: AcousticSelfTest など acoustic 関係の期待値は動かなかった (書き換え 0 件)。NavAgentSelfTest `kExpectedYardHash` を 7C631F3532A75C26 -> 2ABC7F449D843943 に更新 (原因 = autoCellSize の cellHeight 0.05 でベイクが変わったこと + 庭の Deck 0.35。stepOffset x scale の差ではない。庭の CC は scale 1)。
  - PhysicsSelfTest: 新規 (scale.y 2 / 0.5 の段差、velocity の頭打ち) を含め PASS。

自己採点 (1-5):
  仕様適合: 4 — 指摘 1/2/3/4/5 を実装・検証した。庭の +5 cm の検査に戻せた。
  正しさ: 4 — 47 点で (i) 差 0、(ii) の最初の割れが期待どおり (tick 26 / Agent Eye、nav は 70 / 13:1)。重大な未確認は無いが、実プレイの目視は未。
  コード品質: 4 — 変更は小さい独立箇所。cellHeight の丸めは理由をコメントに書いた。
  テスト: 4 — 量子化 2 配置 x 2 高さ、scale 2 / 0.5、velocity、警告判定を固定。acoustic golden の frame 120 で差が出ない理由は未調査。

不安・質問:
  1. shot_verify の acoustic 2 枚は、既定で撮っても着手前 actual とバイト一致になった (frame 120 の撮影では Agent Eye の差が絵に出ない)。replay の dump は tick 26 で割れるので、撮影実行は別の時間経過か視界外。golden を撮り直す必要は出なかったが、「acoustic の golden が Agent Eye の乗り上がりを守らない」点を planner が知っておく価値がある。
  2. 三校の cache\GameLogic.dll を round 2 で誤って 1 回焼き直した (Release、指示は round 1 以降は書かない)。内容は round 1 と同じ構造体で、git 管理外。
  3. Stuck (sub-05) は未実装なので、庭の Curber 検査は「status != NoPath かつ部分経路かつ登らない」で見ている (Moving のまま台の縁で押し続けるのは既存挙動)。

触ったファイル (round 2 で追加・変更):
  src/Engine/Core/Ecs/Components.h, src/Engine/Core/Localization/LocalizationTable.inl,
  src/Engine/Engine/Physics/Rigid/PhysicsSystem.cpp, src/Engine/Engine/Physics/Rigid/PhysicsSelfTest.cpp,
  src/Engine/Engine/Navigation/NavBakeInput.h, src/Engine/Engine/Navigation/NavBakeInput.cpp,
  src/Engine/Engine/Navigation/NavAgentSelfTest.cpp, src/Engine/Engine/Navigation/NavSurfaceSelfTest.cpp,
  src/Editor/Windows/Scene/InspectorWindow.cpp, src/Editor/Tools/NavEditorSelfTest.cpp,
  plans/m82-navmesh/sub-10-external-check.md, plans/m82-navmesh/sub-10.md (実装メモ節のみ)

申し送り:
  - 一時プローブは全て削除済み (PROBE10 / MUTATION10 なし)。強制 0 の実行ファイルは cache\probe0_rel\ に残してある (git 管理外)。ほかの scratch は cache\s10\ と cache\base10_rel\ (削除していない)。
  - GameLogic.dll は Components.h を include するので外部プロジェクトの DLL は焼き直しが要る (round 1 の申し送りのまま。構造体は round 2 で変わっていない)。
  - sub-05 (Stuck): cellHeight が細かくなっても、ボクセルの量子化と CC の停止位置の差で「台の縁で Moving のまま」は残る。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-04)。must 1 件: cellHeight の自動決定 (やること 6、spec 2. #20)。採用した点: stepOffset をワールド m にする (spec 2. #21)、tileSize 48、1b の切り分けの方法と結果 (既存 4 シーンで差なし)、旧 .rep の明示拒否、SelfTest の期待値の書き換え 3 件。should: 段差を登る tick に最大で歩幅 + 半径ぶん前へ出る件。0.5 / 3.5 m/s で、その tick の移動量と CC.velocity (x/z) の値を測って SELF_EVAL に書く。velocity が跳ねるなら『実効の変位 / dt』から登りの前進を除くか、上限を付ける。
- round 1 追記 (2026-10-04、ユーザー回答の反映): #20 は裁定どおり。#21 はユーザーが『scale を掛ける』を選んだので、やること 7 を追加し、round 2 の must に入れた。
- round 2: VERDICT OK (planner、2026-10-04)。must 1 (cellHeight の自動決定。丸めて maxClimb が整数セルになるようにした方法も採用) と must 2 (|scale.y| 倍、acoustic は tick 26 に Agent Eye から割れることを確認) は解消。should 3 は velocity の頭打ちで解消した。ただし登る tick に 0.13〜0.16 m 前へ出る見た目の跳びは残る → reviewer が観察し、ADR に記録 (sub-09)。should 4 は画像で確認した。nit 5 は解消。残り: 折り返しを足した後の Inspector の画像 (insp_surface3.png) は未確認 → reviewer。
