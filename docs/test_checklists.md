# 手動テスト手順

自動化済みの検証 (`--selftest`, `tools\replay_verify.bat`, `tools\check_rules.ps1`) に加えて、
対話的な機能はこの手順で確認する。ウォッチャー/ローダ周りを変更したら必ず再実施すること。

## M3: シェーダ / アセット / シーンのホットリロード

- [ ] 実行中に `assets/shaders/forward_lit.hlsl` を編集 → 1 秒以内に見た目へ反映
- [ ] `common.hlsli` を編集 → forward_lit / deferred 系など依存シェーダ全部が再コンパイル
- [ ] 構文エラーを保存 → Console に赤エラー (ファイル/行)、旧シェーダで動作継続 → 修正で復帰
- [ ] `assets/textures/test.png` を上書き → 地面のテクスチャが変わる
- [ ] `assets/models/BoxTextured.glb` を上書き → メッシュ/テクスチャが変わる (エンティティは不変)
- [ ] シーンを保存 → VS Code で JSON の position を編集 → 実行中のオブジェクトが動く
- [ ] JSON を壊して保存 → 警告のみでエンジン継続 → 直すと反映

## M4: GameLogic.dll のホットリロード

- [ ] Play 中に `Rotator.cpp` の定数を変更 → GameLogic のみビルド → 1-2 秒で挙動変化
- [ ] 状態保持: `angleDeg` が飛ばない / 「Rotator started」が再ログされない
- [ ] フィールド追加 → 「layout migrated」ログ + Inspector に新フィールド (既存値は保持)
- [ ] フィールド削除 → クリーンにリロード
- [ ] VS デバッガをアタッチしたままリロード → 新コードのブレークポイントが効く
- [ ] 5 回以上連続リロード → クラッシュ/リークなし (タスクマネージャでワーキングセット確認)
- [ ] ビルドエラーの DLL (リンク失敗) → 旧ロジックのまま継続

## M5/M6.5: 切替系

- [ ] Particle Settings: CPU ⇔ GPU 切替でクラッシュなし (パーティクル再スタート)
- [ ] Compare mode: 2 つの雲が同じ動き / ms 表示 / SIMD トグルで CPU 時間変化
- [ ] View > Render Path: Forward ⇔ Deferred 切替で見た目一致、繰り返してもリークなし
  (Debug 実行で終了時の D3D レポート確認)

## M46: ハイブリッド・パストレーシング (RT GI / RT 影 / RT 反射)

前提: **Deferred パスのみ**効く。Forward / AssetPreview では自動的に無効。
CLI は Editor / Runtime 共通:

| フラグ | 意味 |
|---|---|
| `--rt-gi` / `--rt-shadow` / `--rt-refl` | 各レーンを最終画像へ合成 (既定 off) |
| `--rt-debug N` | 中間バッファ表示 (下表) |
| `--rt-no-temporal` / `--rt-no-svgf` | デノイズ段の A/B |
| `--rt-freeze-seed` / `--rt-anim-seed` | 乱数列の凍結 / 自動凍結の解除 |
| `--rt-demo` | コーネル箱のショーケースシーンを構築 (M46i) |

`--rt-debug N`: 1=BVH ヒート / 2=ヒット法線 / 3=インスタンス ID / 4=生 GI / 5=蓄積 GI /
6=履歴長 / 7=SVGF 後 / 8=分散 / 9=RT 影可視率 / 10=生の反射 / 11=デノイズ後の反射。

### 非干渉 (既定 off) — サブごとに必須

- [ ] 何も触らずに `Runtime.exe --deferred --replay-verify cache\golden.rep --shot-frame 3
      --screenshot a.png` を**変更前後のバイナリ**で撮り `fc /b` でバイト一致
- [ ] 同じことを `--deferred` 無し (Forward) でも実施 — `common.hlsli` を触ったら必ず
- [ ] RT off のまま `--selftest` / `tools\replay_verify.bat` / `tools\check_rules.ps1` が全 PASS

### RT GI の合成 (M46f)

- [ ] View > Rendering > **RT GI (Deferred)** を on/off → SceneView が即座に切り替わる
      (off で BVH の構築も走らない = Profiler の `rt bvh` 行が消える)
- [ ] **明るさの段差が無いこと**: スカイ (= IBL) のあるシーンで on/off し、遮蔽の無い
      開けた床の輝度がほぼ変わらない (GI は IBL 拡散項と同次元の入射放射輝度で置換される)
- [ ] 色移り: 有色の床の上に置いた白い箱の**影側の面**に床の色が回り込む
- [ ] SSAO 併用: GI on では拡散環境項に AO が掛からない (二重遮蔽にならない)。
      IBL スペキュラ項には従来どおり掛かる
- [ ] Unlit / Wireframe 表示モードでは GI が掛からない (環境項が定数のまま)
- [ ] SceneView と GameView を同時に開いても互いのノイズ/履歴が混線しない
- [ ] Debug 実行で D3D デバッグレイヤの警告 0 (GBuffer を CS の SRV で読むため
      RTV のアンバインド漏れがあるとここに出る)

### RT 影 (M46g)

- [ ] View > Rendering > **RT Shadow (Deferred)** を on/off → 影の**位置は変わらず**
      輪郭だけが変わる (CSM とシルエットが一致していること)
- [ ] 8 倍に拡大して比較: RT 側は輪郭が 1 画素精度で立ち、CSM 側 (3x3 PCF @2048) はにじむ。
      遠景 (粗いカスケードの領域) ほど差が開く
- [ ] **アクネ (照らされた面の暗点) が無い / ピーターパン (影が浮く) が無い**
- [ ] `--rt-debug 9` で可視率がグレースケール表示になる (半影がグラデーションになる)
- [ ] `enableShadows` を off にすると RT 影も連動して off になる
- [ ] 透明物 (Forward 後段) は従来どおり CSM を見る = 混在するのが仕様どおり

### RT 反射 (M46h)

- [ ] View > Rendering > **RT Reflection (Deferred)** を on/off → 鏡面 (roughness 低) の
      映り込みが IBL の近似から実際のジオメトリへ変わる
- [ ] **画面外のジオメトリが映る** (SSR との決定的な差。鏡球を視野の端に置いて確認)
- [ ] **粗い面は完全に従来経路**: 全マテリアルを roughness 0.9 にすると on/off の差が
      浮動小数の再結合レベル (数画素 × 1 LSB) に収まる
- [ ] roughness を 0.0 → 0.85 までスイープしてカットオフ (0.6) に**段差が出ない**
- [ ] `--rt-debug 10` (生 1spp) と `11` (デノイズ後) でフィルタの効きを確認

### 自己発光と GI 光源化 (M46i)

- [ ] `.mat.json` の `emissive` を 0 → 6 に編集 → **ホットリロードで面が光る**
      (Inspector の emissive スライダでも同じ)
- [ ] `--rt-demo` でショーケース起動 → **RT 全 off では箱の中が定数アンビエントで平坦、
      金属球は真っ黒** (アナリティックライトも IBL も無いので正しい)
- [ ] `--rt-demo --rt-gi --rt-shadow --rt-refl --rt-anim-seed` → 発光パネルだけを光源として
      箱の中が照らされ、**左壁の赤 / 右壁の緑が床と白い箱へ回り込む**
- [ ] 同じシーンを Forward で起動 → 発光パネルは光るが GI は無い
      (Forward は emissive を定数バッファ直渡しで加算、量子化なし)
- [ ] 発光を使っていない既存シーンは M46i 以前とビット一致 (上の「非干渉」項目で担保)
- [ ] `--rt-demo` は `main.scene.json` を上書きしない (`--save-scene-on-start` と併用しても)

### 品質 A/B (計測時の注意)

- [ ] `--rt-no-temporal` で 1spp の生ノイズ / 外すと均される
- [ ] `--rt-no-svgf` で蓄積のみ / 外すとエッジを保ったまま平滑化
- [ ] **スクリーンショットは自動でシード凍結される** — デノイズの効きを写したいときは
      `--rt-anim-seed` を併用する (付けても `--replay-verify` ならフレーム決定的)
- [ ] Profiler の `rt` 行は**最終フレーム 1 サンプル**なので分散が大きい。
      同一条件で 5 回撮って**最小値**を代表値にする
- [ ] 既知のノイズ帯: roughness 0.3〜0.6 の金属はシード凍結中にファイアフライが残る
      (実行時は蓄積で解消する)

## M68: 音響 × オーディオ (耳で確認)

前提: **音を出す環境が要る** (`--no-audio` を付けると出力レーンは 1 行も走らない = 設計どおり)。
起動は `Runtime.exe --acoustic-demo`、またはエディタで `Editor.exe --acoustic-demo` → Play。
操作は M65g のまま (WASD 移動 / Shift 走り / Ctrl しゃがみ / **V で一人称** / F 長押しで光の設置・回収 /
Q 石・E 瓶)。波そのものを見たいときは SceneView の「音響」トグル。
配管の機械検査は `--acoustic-audio-log N` (`[acaudio]` 行 + 終了時 summary) で、
**耳の確認はこの表**が正本。

### 遮蔽と回折 (M68a)

- [ ] 起動直後 (部屋 A の隅) → 部屋 B の hum が**こもって小さく**聞こえる
      (壁越しなので `class=Detour` + lpf は床 0.25。**`Occluded` ではない** —
      L 字は開いていて経路が通っているため。`Occluded` になるのは密閉と経路上限超えだけ)
- [ ] 横の廊下を東へ歩く → hum が**次第に開いて**くる (回り込み量が減るので lpf が上がる)
- [ ] 縦の廊下 → hum が**戸口の方向 (前方) から**聞こえる (音源は部屋 B の奥にあるのに、
      音は角から来る = 仮想発音位置が効いている証拠)
- [ ] 戸口をくぐる → 素通しになる (`class=Direct`、lpf 1.0)
- [ ] 部屋 A へ戻る → **段差なく**こもっていく (`smoothTicks` = 100 ms 半減期の平滑化)
- [ ] グリッドの外まで離れる → 遮蔽が全部外れて素の 3D 音になる (`Bypass`)
- [ ] Inspector で `AcousticAudio.enabled` を off → **全部素通しに戻る** (on/off の A/B が
      そのまま「音響が効いているか」の判定になる。NoHash なので実行中に触ってよい)

### 部屋の残響 (M68b)

- [ ] 廊下 → 部屋 B と歩く → 残響が**段差なく**広がる (2 プリセットの連続補間。
      「切り替わった」と分かる瞬間があったら補間が効いていない)
- [ ] 部屋 A の中央 ⇔ 隅 → 隅のほうが響きが少ない (開放度 0.67 → 0.47)
- [ ] ミキサー窓を開く → リバーブの combo は**資産の値のまま**で、隣に
      「(音響が上書き中)」が出ている (資産を書き換えていないこと)
- [ ] `.mixer.json` を保存してホットリロード → override が**自動で再適用**される
      (戻ってしまうなら選択が `ApplyReverbParams` の 1 箇所から漏れている)

### 鳴る波 (M68b)

- [ ] 歩く → 足音が鳴る。**床材で音色が変わる** (廊下のタイル 6 枚を踏み比べる)
- [ ] 金属の上を歩く → 遠くまで届く / カーペットの上 → **数歩で消える**
      (`acousticLoudness` が波の振幅、`minWaveVolume` が足切り)
- [ ] 立ち止まる → **呼吸は鳴らない** (0.07 < `minWaveVolume` 0.10)
- [ ] Shift で走る → 足音の間隔が詰まる (歩幅が縮む。1 歩の大きさは床材のまま)
- [ ] 箱が金属板へ落ちる → 衝撃音が鳴る (自分が出していない音も同じ経路で整形される)
- [ ] 敵 (赤 / 緑) が近づく → 自発音が聞こえ、**壁越しならこもる**
- [ ] Q で石 / E で瓶を投げる → 着弾音が投げた先から聞こえる
- [ ] 部屋 B から遠く離れる → 足音は聞こえるが hum は消える (波の到達範囲 = 鳴る範囲)

### 壊れ方の切り分け

- [ ] **耳で聞こえないのに `[acaudio] summary` の `playFailed=0`** → エンジンは voice を
      立てている。疑うのは XAudio2 側 (ミキサー窓のミュート / バス音量 / OS の出力デバイス)
- [ ] `unknownKey` > 0 → `AcousticAudio.toneSound0..3` の名前が `.sound.json` の `name` と
      食い違っている (起動ログの `[audio] unknown sound key` も見る)
- [ ] `dropped` > 0 → 1 フレームに 64 個以上の波が生まれている (キュー上限)
- [ ] `class=Bypass` ばかり → リスナーがグリッドの外か `AcousticVolume` が無い
- [ ] 同じコマンドを 2 回回して `[acaudio] t=` 行が**バイト一致しない** → 出力レーンに
      実時間かポインタが混ざっている (決定論の契約違反。`--synth-input` を付けて比較する)

## M72: 分岐デバッグ (What-if リプレイ)

前提: Play 中 (タイムトラベルのリングは Play 中しか回らない)。機械検査は
`Editor.exe --whatif-selftest 400` (Debug / Release、`replay_verify.bat` の whatifdebug / whatifrelease)。
ここは**手で触ったときの感触**の表。

### 分岐と編集 (M72a / M72b)

- [ ] Play → 数秒 → ツールバーの巻き戻し (または Timeline で戻る) → 「Branch and resume」→
      Console に `[timetravel] fork at tick T -> branch B1 (N ticks moved)` が出る (未来は捨てられていない)
- [ ] そのまま何も触らず走らせる → 元の未来の終端に追いついた tick で
      `[timetravel] branch B1 collapsed: identical to the live lane` (同じ入力なら分岐は残らない)
- [ ] 戻る → **ポーズ中に Inspector で何かの位置を動かす** → 再開 → `fork ... - the live state was
      edited, re-captured` → もう一度その tick へ戻る → **編集が残っている** (M52e では消えていた) かつ
      Timeline の self-check が `OK` (赤の HASH MISMATCH にならない)
- [ ] 編集後に走らせた run は畳まれない (Console に collapsed が出ない)
- [ ] 分岐ができた次のフレームに Console へ `[timetravel] ghost of B1 baked: ticks F-N (...),
      N entities (M moving), K keys, S KB, T ms -> hash OK` が出る (M72d)。続けて
      `seek to tick F+1 ... -> hash OK` (焼いた後にライブへ戻した印)。既定デモ (527 体が動く) で
      100 tick = 約 3 MB / Debug 0.5 秒

### Timeline のレーン (M72c)

- [ ] Window > Timeline。Play 中に戻って再開すると「Lanes」の帯にライブ (オレンジ) の下へ
      分岐の帯 (水色 = B1) が出る。帯の横軸は tick で、分岐の帯はライブより先まで伸びている
- [ ] 表の行: `live` / `B1 @<fork>`、範囲 `[fork, end)  N ticks`、「diverges from live」が
      同じ入力なら `none (identical so far)`、編集して再開した後なら `tick <fork>`
- [ ] **Switch** → ポーズして、その分岐の世界が復元される (self-check が `OK`)。
      いままでのライブは新しい行 (B2…) として残る。もう一度 Switch で元へ戻れる
- [ ] **Delete** → 行が消える。8 本を超えると最古の葉が自動で消える (表の下の注記)
- [ ] `Editor.exe --whatif-selftest 200 --screenshot cache\tl.png --shot-frame 262 --frames 270`
      で Timeline が開いた状態の画が撮れる (プローブは Timeline を自動で開く)

### SceneView のゴースト (M72e)

- [ ] 分岐ができた次のフレームから、SceneView に分岐色 (B1 = 水色) の**ワイヤ箱**が
      動いている物にだけ重なり、前後 (過去 60 / 未来 180 tick) の**トレイル**が伸びる。
      同じ入力の分岐なら箱はライブの物にぴったり重なる (= 決定論の絵)
- [ ] 戻って**違う操作**をしてから再開すると、ゴーストの箱がライブから離れていく
      (元の未来がどこへ行ったかが見える)
- [ ] SceneView ツールバーの「Ghosts」を外すと全部消える。Timeline の分岐行の
      チェックを外すとその分岐だけ消える。表の ghost 列に `N moving (S KB)`、
      予算で打ち切られたら `up to tick T (S KB, budget)`、再現できなければ赤の `HASH MISMATCH`
- [ ] GameView / --screenshot (Runtime) には 1 本も出ない (SceneView の RT だけ)

### 入力の上書き (M72f)

- [ ] Timeline の「Input overrides」でアクション (例 Jump) を選び、tick 数 60 で「Hold」→
      現在 tick から 60 tick、そのアクションが押されっぱなしになる (Jump なら跳び続ける)
- [ ] 戻ってから Hold → 再開すると、元の未来がゴーストとして残り、押した分だけライブが離れる
      (表の「diverges from live」が押し始めの次の tick)
- [ ] その区間へシークすると同じ動きが再現する (上書きはリングの entry に記録されている)
- [ ] 軸 (MoveX 等) は値スライダー付き。負の値で negKey、キーの無い軸はパッドの値を置く
- [ ] 適用が終わった項目はグレーになる。「Clear all」で全部消える。Stop でも消える

### フィールド差分 (M72g)

- [ ] 乖離のある分岐の行に「Diff」が出る → 押すとポーズし、Console に
      `[timetravel] diff lane 0 vs B at tick T: ok, N field(s) differ, ... live restored`、
      Timeline の下に「live vs B<id> at tick T: N field(s) differ」の表 (エンティティ /
      コンポーネント.フィールド / ライブの値 / 分岐の値、hex)
- [ ] Jump を押し続けた分岐なら、最初に違うのは `PlayerController.jumpCount` と `prevSpace`
      (= 押した入力を読んだスクリプトの状態)。Inspector で位置を編集した分岐なら
      `LocalTransform.position` (= 編集そのもの)
- [ ] 表の後も self-check が `OK` (両レーンを再シムした後、ライブへ戻している)。
      「Branch and resume」でそのまま続けられる

### 三校ステージでの通し (コンテストの見せ方)

`Editor.exe --project C:\HAL\Shadow_Sound` → Play (stage1)。

- [ ] 敵に見つかるまで歩く → 見つかった tick から 5 秒ほど戻る → Timeline の「Input overrides」で
      `Light` を 150 tick 押す (= ビーコンを置く) → 「Branch and resume」
- [ ] SceneView: 元の未来では敵 (`AgentEar*`) がプレイヤーへ向かうゴーストのトレイルが伸び、
      ライブでは光に釣られて別の経路を歩く = **同じ tick の 2 つの未来が重なる**
- [ ] Timeline: 乖離 tick が「押し始めの次の tick」、Diff で最初に違うのが `PlayerInput` /
      `AgentBrain.state` 系のフィールド (敵の状態が変わった瞬間)
- [ ] Inspector で `GameRoot` の `SkTuning` (敵の速度など) を変えてから再開 → 乖離が分岐点で出て、
      Diff が `SkTuning.<field>` を名指しする (= 「何を変えたか」が機械で出る)
- [ ] Switch で元の未来へ戻り、もう一度別の操作で分岐 → 3 本のレーンを行き来できる
