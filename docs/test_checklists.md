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
- [ ] (M72i) 動いている物は**分岐色の半透明メッシュ** (陰影付き、不透明度 40%) で出て、ライブの壁の
      向こう側は隠れる (深度テストあり)。メッシュが引けない物だけワイヤ箱。トレイルと Timeline の
      目アイコンはそのまま

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

## M73: タイムラインのホールドと帯

前提: Play 中。機械検査は `Editor.exe --timetravel-selftest 400` (hold / step の段) と
`--whatif-selftest 400` (編集点を跨ぐ前進シークの段)。ここは手触りの表。

### 一時停止 = ホールド (M73a)

- [ ] Play → ツールバーの ⏸ → Timeline の「tick N」と帯の縦線が**止まる** (M72 までは 60Hz で進み続けた)。
      ステータスバーは「一時停止」、Timeline の状態語は「tick N で停止中」
- [ ] ⏸ のまま数秒待つ → Timeline の「リング: tick A - B」の B が増えない (ポーズ tick が溜まらない)
- [ ] step (ツールバー / Timeline) → tick が**ちょうど 1** 進んで止まる。連打しても 1 回に 1 tick
- [ ] ホールド中に Inspector で位置を動かす → ▶ → Console に `fork ... the live state was edited,
      re-captured` → 戻っても編集が残り、self-check が OK
- [ ] ホールド中にステップした直後、SceneView の物が「1 フレームだけ進んで戻る」ように見えない
      (描画補間が前 tick を描いていた M73a 以前の症状)
- [ ] `--replay-record` 中 (リング無し) は従来どおり: ⏸ でも tick は進む (Timeline は「記録中」表示なし、
      窓の中身は出ない)

### トランスポートと帯 (M73b)

- [ ] Timeline 上段: ⏮ -30 -1 [⏸/▶] step +1 +30 ⏭ と、右に状態語 / `tick N` / `mm:ss:ff`。
      再生中は ⏸ がオレンジ、止まると ▶。過去で止まっているときは「(過去)」+ 「ここから再開すると分岐」
- [ ] 帯をクリック → その tick へシーク (ポーズを伴う)。ドラッグ → 追従してシーク。ホバーで
      `tick N  mm:ss:ff` + 操作説明のツールチップと薄い縦線
- [ ] 帯の上でホイール → ±1 tick、Shift+ホイール → ±30。窓はスクロールしない
- [ ] live 行の下辺に 1px のスナップショット目盛り (30 tick ごと)。分岐点で撮り直した pinned は 2px
- [ ] 分岐すると B1 の帯が fork 点から伸び、live からの縦線で枝分かれが見える。乖離があれば ▲ (黄)
- [ ] 「入力」行: ジャンプ等を押した tick が薄い帯で出る。「上書き」行: Hold した区間が黄で出て、
      適用が終わると淡くなる
- [ ] レーン表の `B1 @fork` と乖離 `tick N` はリンク: 押すとその tick へシーク
- [ ] Diff を押すと「フィールド差分」の見出しが自動で開く。畳んでもう一度 Diff → また開く
- [ ] 戻る → 編集点より**手前**へさらに戻る → 前進で編集点を跨いで進める → self-check が OK
      (M73b 以前は HASH MISMATCH)
- [ ] `--lang ja` で文字幅が崩れない (トランスポートの状態語 / 帯のラベル列 64px)

## M81: 専用サーバ (Server.exe) と GameLift

自動化できる部分は先に通す。実プロセスの `server_verify.bat` は全ケースで約 13 分かかり、CI には載せない
(UDP + 複数プロセス + 実時間。`net_verify.bat` と同じ理由)。Debug と Release の `Editor.exe --selftest` は直列で回す
(同時に回すとシェーダキャッシュ置き場を共有して M79 の項目が落ちる)。

### 自動検証 (M81a〜M81i)

- [ ] `bin\x64\Debug\Editor.exe --selftest` / `bin\x64\Release\Editor.exe --selftest` (直列): Session self test と
      Server/client net self test が PASS。既知の失敗は Source control の 2 項目 (`external cherry-pick state closes the normal
      write gate` / `external revert state survives status refresh`) のみ
- [ ] `bin\x64\Debug\Server.exe --selftest` / `bin\x64\Release\Server.exe --selftest`: 全項目 PASS (exit 0)。GameLift の偽 SDK、
      Terminate の 1 本の経路、実プロセスへの Ctrl+Break、`.rep` の逐次書出しと救済を含む
- [ ] `tools\server_verify.bat` (全ケース ABCD): A 2 クライアント / B Debug・Release 混在 3 クライアント + ロス 20% + 途中参加 +
      切断 → 再接続 / C desync 注入 → バンドル + 再同期 / D Release 4 クライアント。各ケースで、サーバ `.rep` と各クライアントの確定 tick が
      重なり区間で全 tick 一致し、サーバ `.rep` が Debug / Release の `Server.exe --replay-verify` と窓ありの `Runtime.exe --replay-verify` でも一致する。
      A (WARP のクライアント) はさらに、クライアント `.rep` (参加 tick から始まる) を単独で `--replay-verify` して 0 でない tick 数で PASS し、ロス 0 での各レーンの
      `late-subst` (確定を待たれた tick のうち代替入力になった割合) が 5% 以下、サーバが強制した再同期が 0、
      クライアントが要求した再同期 (desync / EventGap / BadSnapshot。ログの `requesting a resync` と `.rs1.rep`) も 0 であること。C はさらに、診断バンドルの
      `local.rep` が単独で再生でき (壊した tick の直前まで一致)、バンドルの `local.dump` とサーバ `.rep` の同じ tick のダンプの `--hash-diff` が
      壊した `LocalTransform` を名指しすること
      **D は 1 台の PC で WARP の Runtime 4 台とサーバが論理コアを奪い合う負荷試験**で、tick 時間 (R3) も `late-subst` も計測環境の制約を受ける。
      値は出すが合否には使わない。D のログの `tick time: avg ... max ...` を見て、avg が 4 ms を大きく下回ること (max は参加時に 7〜40 ms まで
      跳ねる = R-10)
- [ ] `tools\replay_verify.bat`: 9 シーンすべてに Release の `Server.exe --replay-verify` が含まれ PASS
- [ ] `tools\net_verify.bat`: P2P が従来どおり PASS (プロトコル版 6)
- [ ] `pwsh -File tools\check_rules.ps1`: 規則 13 (sim 側から Net / Hosting を include しない、GameLift の依存は Server.vcxproj だけ) を含め 0 error

### エディタの Network 窓 (M81i)

- [ ] セッションが無いときの Network 窓に、HOST:PORT と player session ID の入力欄、接続ボタンが出る。`--lang ja` / `--lang en` の両方で
      文字が崩れず、長い文は窓幅で折り返す
- [ ] HOST:PORT を空にする / 引用符か空白を含めて接続を押すと「起動できません」が赤で出る (エディタは増えない)
- [ ] `Server.exe --local-demo --synth-input --max-players 4 --net-delay 3 --port 7777` を起動し、別の Editor で
      `Editor.exe --local-demo --synth-input --net-connect 127.0.0.1:7777 --autoplay` → Network 窓が自動で開き、役割「専用サーバのクライアント」、
      自レーンと playerId、レーン 4 本の状態 (Empty / Connected / Reserved と playerId)、確定 tick が進む、先行 tick、到着余裕、再同期 / desync 0 回
- [ ] 接続欄から接続すると、もう 1 つのエディタが `--net-connect` 付きで起動する (この窓のセッションは変わらない)
- [ ] 接続したエディタのツールバーで、一時停止 / ステップが無効表示になりツールチップに理由が出る (`--lang ja` / `--lang en`)。
      停止を押すとサーバのログに `left` (Leave) が出て、そのエディタの sim は止まり、再生ボタンも無効のまま。
      サーバが止まらないので、一時停止を許すと tick ごとに desync → 再同期を繰り返してしまう (M81k)
- [ ] 接続して 2 分ほど放置し、Network 窓の到着余裕が目標 (ジッタが無ければ約 16 ms、あれば 16 ms + 2σ で最大 100 ms) の近くで安定している
      (以前は約 90 ms に張り付いた)。クライアントのログ `time sync: arrival margin X ms (target Y, sigma Z)` で確かめる
- [ ] 往復 250ms を超える回線 (selftest の `V15` が偽トランスポートで再現) では、サーバのログに `cannot keep up` の WARN が peer ごとに 1 回出る

### GameLift Anywhere (手動、AWS アカウントが要る。手順は `docs\gamelift-anywhere.md`)

- [ ] IAM / カスタムロケーション / Anywhere フリート / `RegisterCompute` / 認証トークン (約 15 分で失効する。取ったらすぐ起動する) を用意し、
      `MYE_GAMELIFT_AUTH_TOKEN` を環境変数に置いて `Server.exe --hosting gamelift ...` を起動する (トークンをコマンド履歴に残さない)
- [ ] `Server.exe --hosting gamelift` を接続情報なしで起動すると、足りない項目を 1 行で挙げて exit 1 (30 秒以内に戻る)
- [ ] `CreateGameSession` でゲームセッションが ACTIVE になり、`CreatePlayerSession` の ID を `--player-session-id` に渡した Runtime が接続できる。
      誤った ID は拒否される
- [ ] プレイ中に Runtime を落として (Bye 無し) 同じ `--net-player-id` / player session ID で再接続すると、同じレーンへ戻る。
      保持期間 (既定 30 秒、`myeRejoinTimeoutTicks`) を過ぎると席が解放される
- [ ] 参加の瞬間のサーバの tick 時間 (`tick time: ... max`) をログで確認する。`AcceptPlayerSession` が詰まって tick が止まらないか (R-11)
- [ ] 終了 (`TerminateGameSession` または Ctrl+C) でゲームセッションが TERMINATED になり、Server.exe が exit 0 で終わる。
      サーバの `.rep` が最後まで読め、`Server.exe --replay-verify` で全 tick 一致する
- [ ] 片付け (フリート / ロケーション / コンピュートの削除) まで行い、AWS 側のログを `plans\m81-dedicated-server\anywhere-log\` に残す

## M82: NavMesh (Recast Navigation)

自動検証は `Editor.exe --selftest` (NavDeterminism / NavSurface / NavAgent / NavEditor / Physics の段差)、`tools\replay_verify.bat` の `nav` ジョブ、
`tools\shot_verify.bat` の golden `nav`。下は画面と実操作で確かめる項目 (設計: `docs\adr\ADR-023-navmesh.md`)。

### Surface とベイク (M82b / M82d / M82e)

- [ ] Hierarchy の Create → 3D Object の下に NavMesh Surface / Obstacle / Modifier / Link があり、どれも Undo / Redo できる
- [ ] Surface の Inspector で Bake を押すと進捗が出て、終わると `assets\NavMesh\<名前>_<16桁>.mnav` ができ、タイル数・ポリゴン数・時間が出る。Clear で参照が外れる
- [ ] Bake 直後に Play せず、SceneView に半透明のエリア色と輪郭線が出る。床とちらつかない (Z ファイトしない)
- [ ] `autoCellSize` が on のとき、Inspector に実際のセル (例 0.150 m、高さ 0.050 m) と実効の傾斜上限が出る。`maxSlopeDeg` を実効の上限より上げると警告が出る
- [ ] Surface の範囲箱ギズモが SceneView に出て、動かすと範囲が変わる

### Agent の移動 (M82c / M82d / M82f)

- [ ] `Runtime.exe --nav-demo` で Agent が段差 (0.3 m)・坂 (30 度)・台を越えて目的地へ歩き、届かない島では止まる。壁を貫通しない
- [ ] NavMeshAgent を Add Component すると CharacterController も一緒に付く。CC を外すと Inactive になり警告がログに 1 回出る
- [ ] 2 体が向かい合ってすれ違うとき、回避あり (`avoidanceQuality` 1 以上) は重ならず、0 は重なりうる
- [ ] 縁で前へ進めなくなった Agent は約 1 秒後に `Stuck` になり、WARN が 1 回出る。目的地を変えると解除される
- [ ] CharacterController の `stepOffset` を 0 にすると段差を登らない。Transform の scale.y を倍にすると登れる高さも倍になる

### Obstacle / Modifier / Link (M82f〜M82h)

- [ ] Play 中に Obstacle (carve) を経路上へ動かすと、同じ tick で切り抜きが表示に反映され Agent が迂回する。消すと元に戻る。`carve` を外した Obstacle の Inspector に注意書きが出る
- [ ] Modifier でエリアを高コストにした帯を Agent が避ける。`areaMask` で除いたエリアには入らない。Inspector のコストをドラッグすると 1 Undo
- [ ] Link の Linear / Jump / Manual をそれぞれ Agent が渡る。Manual は `NavCompleteLink` まで入口で止まる。片方向の Link は逆向きに使われない
- [ ] Link の入口が Surface の外、または出口が 2 タイル以上離れるとき、Inspector に警告が出る
- [ ] 渡っている最中に Link を削除・移動しても、Agent は出口まで渡り切って NavMesh に戻る

### 回帰 (M82j)

- [ ] 外部プロジェクト (三校 / HAL Collector) の `GameLogic.dll` は ABI v24 で再ビルドが要る (古い DLL は読み込みを拒否される)

## M84: NavMesh の拡張

自動検証は `Editor.exe --selftest` / `Server.exe --selftest` (NavSurface / NavAgent の M84 の節)、`tools\replay_verify.bat` の `nav` ジョブ
(`NavDemoDriver` が ABI v26 を使う)。下は画面と実操作で確かめる項目 (設計: `docs\adr\ADR-023-navmesh.md` の決定 14)。

- [ ] Project Settings の「NavMesh の Agent Type」で種別を足す・消す・保存できる。飛び降りの高さ・飛び越えの距離の列がある。id 0 は消せない
- [ ] Surface / Agent の種別を名前で選べる。表にある種別の Surface は寸法 (飛び降り・飛び越えを含む) が読み取り専用で、表を変えると Inspector に食い違いの警告が出る
- [ ] 同じ種別の Surface を 2 つ並べて Bake すると両方に同じ `.mnav` が付き、Surface をまたいで Agent が歩く。Clear も両方に 1 Undo で効く
- [ ] `.navfilter.json` を作って Inspector でエリアのコストと「通らない」を編集でき、Agent に付けると経路が変わる
- [ ] Agent の isStopped を Play 中に切り替えると止まって再開する。updatePosition を切ると Agent が動かず、desiredVelocity が更新される
- [ ] Surface の「リンクを自動生成」を入れて Bake すると、段の縁と隙間にオレンジの Link が描かれ、Inspector に本数が出る。段の上の Agent が飛び降りる
- [ ] 生成の設定や飛び降りの高さを変えると Inspector に「もう一度ベイク」の警告が出る。生成したリンクの渡り方は Bake し直さなくても効く
- [ ] 外部プロジェクト (三校 / HAL Collector) の `GameLogic.dll` は ABI v26 = 151 スロットで再ビルドが要る (その後 M85 で v27 = 158 に上がった。現在の要件は M85 節)

## M83: AI の知覚 (AIPerception)

自動検証は `Editor.exe --selftest` / `Server.exe --selftest` (PerceptionSelfTest)、`tools\replay_verify.bat` の `perception` ジョブ、
`tools\shot_verify.bat` の golden `perception`。下は画面と実操作で確かめる項目 (設計: `docs\adr\ADR-024-ai-perception.md`)。

- [ ] Add Component の AI カテゴリに「AI 知覚」「AI 刺激源」がある
- [ ] AIPerception を選ぶと、SceneView に視野の扇形 (見える距離と見失う距離の 2 本の弧)・必ず気付く距離の円・聞こえる距離の円・目の印が出る。視野角や距離を変えると扇形が追随する。選択を外すと消える
- [ ] `Editor.exe --perception-demo` で Play すると、見張りが侵入者に気付いて振り向き、見えている間は緑の線、見失うと黄の十字 (最後の位置) と橙の線 (予測位置) が出る。柱の陰に入ると見失う
- [ ] Play 中の見張りの Inspector に「知覚している相手」の一覧が出て、感覚 (視覚・聴覚・ダメージ・接触) と何 tick 前かが更新される。名乗らない音は「(名乗らない音源)」
- [ ] 聴覚の方式を Acoustic にして AcousticListener を付けないと、Inspector に警告が出る
- [ ] 外部プロジェクト (三校 / HAL Collector) の `GameLogic.dll` は ABI v25 で再ビルドが要る (古い DLL は読み込みを拒否される)

## M85: ビヘイビアツリー (BT + Blackboard + 巡回ルート)

自動検証は `Editor.exe --selftest` / `Server.exe --selftest` (BehaviorTreeSelfTest)、`tools\replay_verify.bat` の `bt` ジョブ
(Debug / Release / `Server.exe`、snapshot stress)、`tools\shot_verify.bat` の golden `bt`。下は画面と実操作で確かめる項目
(設計: `docs\adr\ADR-025-behavior-tree.md`)。

### デモ `--bt-demo`

- [ ] `Editor.exe --bt-demo` で Play すると、見張り A / B が次の 5 段階を順に踏む (ログと動きで確認。`stage` が BB に書かれる)
  1. 巡回: ルートの点を順に歩き、点で待つ
  2. 発見: 侵入者に気付いて振り向く (A が僚機 B へイベントを送り、B も警戒する)
  3. 追跡: 侵入者へ向かって走る
  4. 見失う: 侵入者が見えなくなり、`target` が外れる
  5. 捜索 → 巡回へ戻る: 最後の位置 / 予測位置の周りを回り、見つからなければ巡回に戻る
- [ ] ログの段階の tick が毎回同じ (巡回 → 発見 → 追跡 → 見失う → 捜索 → 巡回へ戻る)

### BT 窓のライブ表示 (Play 中)

- [ ] Play 中に Guard A (`drawDebug` が立っている) を選んで `guard.bt.json` を開くと、ヘッダに実行中の状態が出る
- [ ] 実行中のノードが緑の太枠で追従し、巡回から追跡へ切り替わる瞬間に Abort したノードから橙の矢印が出て、約 30 tick で薄れる
- [ ] BB パネルに現在値 (`target` / `lastKnownPos` / `route` / `buddy` / `stage`) が出て、編集中の欄と表示が混ざらない
- [ ] SceneView に MoveTo の目的地・SearchArea の点・実行中のタスク名が出る (`drawDebug`)
- [ ] タイムラインを巻き戻すと、巻き戻した tick の実行中ノードと BB の値が出る (巻き戻し直後は Abort の矢印が空でよい)
- [ ] SubTree (`guard_sense.bt.json`) を含む木で、実行中の枠が部分木のノードにも付く (元の id で表示される)

### BT 窓の編集

- [ ] Asset Browser の作成メニューで「ビヘイビアツリー」「ブラックボード」を作れ、`.bt.json` のダブルクリックで窓が開く。Window メニューからも開く
- [ ] パレットからノードを置き、親の下端から子の上端へドラッグで接続できる。子の順序は x 座標の左から (番号が出る)
- [ ] ドラッグ 1 回・パラメータの編集確定 1 回が Undo 1 段。Ctrl+Z / Ctrl+Y と Delete は窓がフォーカスを持つときだけ効く
- [ ] BB パネルでキーを追加・削除・改名・型変更でき、編集中の木のキー参照が追従する (ほかの木は追従しない)
- [ ] 検査エラー (SubTree の BB 不一致、LowerPriority の位置、SimpleParallel の左がタスクでない、未設定のキー参照、子の数) がノードの赤枠と一覧に出る
- [ ] 保存すると未保存の印が消え、保存直後にエンティティの木が 2 回やり直されない
- [ ] CppTask のタスク名のピッカーとフィールド欄、CsTask のクラスのピッカーとフィールド欄が使える。C# タスクを含む木は「決定論の保証外」の警告が出る

### 巡回ルートと Inspector

- [ ] PatrolRoute を選ぶと、SceneView に点の球・点の間の線・向きの矢印・番号が出る。点をドラッグで動かせ、1 ドラッグ = 1 Undo
- [ ] Inspector で点の追加・削除・並べ替えができる
- [ ] BehaviorTree の Inspector で「Entity キーの初期値」(4 組) を選べ、Play 開始時にその Entity が BB に入る
- [ ] BehaviorTree と AgentBrain を同じエンティティに付けると警告が出る

### 回帰

- [ ] `activeNodeId` は SubTree を展開した後の実行木の id である (SubTree を含む木ではアセット上の id と一致しない)
- [ ] 外部プロジェクト (三校 / HAL Collector) の `GameLogic.dll` は ABI v27 で再ビルドが要る (古い DLL は読み込みを拒否される)

## M88: GPU デバイス消失 (DEVICE_REMOVED / RESET) の復旧

自動検証は `Editor.exe --selftest` (DeviceRecoverySelfTest: 参照数ゲート・アセット復旧・Surface マテリアル復旧・退避保存)、
`tools\replay_verify.bat` (`MYE_EXTRA_ARGS="--simulate-device-lost 1"` 付きでも 17 job 一致)、`--simulate-device-lost` 付きの実行
(設計: `docs\adr\ADR-026-device-lost-recovery.md`)。下は画面と実操作で確かめる項目。

### (a) メニューの偽装で復旧する

- [ ] `Editor.exe` を普通に起動し、View > Rendering > 「デバイス消失を偽装」を押す。数秒 (Release の既定デモで約 4 秒) 画面が止まった後、同じ見た目に戻る
- [ ] 復旧後、Scene ビュー・Game ビュー・Hierarchy / Inspector / Asset Browser のフォントとアイコンが描かれ、ドッキング配置が変わっていない
- [ ] 復旧後、Asset Browser のモデル / プレハブのサムネイルが再生成され、Inspector のマテリアルプレビューも出る
- [ ] 復旧後、オブジェクトのクリック選択・ギズモの移動 (Undo / Redo を含む)・Play / Stop が使える
- [ ] 反射プローブを焼いてあるシーンで偽装すると、復旧後に自動で焼き直され、プローブの反射が消失前と同じになる
- [ ] Console に `[device] recovery started` → `old device external references: 0 (allowed 0)` → `device recovered in ... ms (... world hash ... unchanged=1)` が出る。`unchanged=0` や `cannot continue` は失敗
- [ ] Play 中に偽装しても Play が続き、ゲームの状態 (位置・スコア) が飛ばない
- [ ] 60 秒以内に 3 回続けて偽装すると、3 回目は復旧せずメッセージボックス (理由 + 退避ファイルの場所) を出して終了する (終了コード 6)

### (b) 本物の TDR (`dxcap -forcetdr`)

実機 GPU でだけ確かめられる。**管理者権限の PowerShell** で実行する (DirectX の `dxcap.exe` は Graphics Tools の機能: 設定 > アプリ > オプション機能 > 「グラフィックス ツール」)。

1. `Editor.exe` を起動して、シーン (できればサムネイルや反射プローブがあるもの) を開いたままにする
2. 管理者 PowerShell で `dxcap -forcetdr` を実行する (画面が数秒ちらつく)
3. 期待する結果:
   - [ ] エディタが落ちずに復旧し、(a) と同じ項目が満たされる。Console のログの `present hr=0x887A0005` (DEVICE_REMOVED) または `0x887A0007` (DEVICE_RESET) と `removed reason` の値が出ており、`(simulated)` は付かない
   - [ ] 復旧直後にもう一度 `dxcap -forcetdr` を打ってもう一度復旧する
   - [ ] 復旧後、ドライバが新デバイスを作れない期間があっても、再試行 (最大 10 回 × 500 ms。Console の `device recovered` の `N attempt(s)` で回数が分かる) で戻るか、戻らなければ (c) の退避と終了になる
4. TDR が起きなかった場合 (設定やドライバで無効) は、(a) の偽装で代用したことを記録に残す。本物の TDR を確認できていない旨を書く

### (c) 復旧できなかったときの退避ファイル

- [ ] `Editor.exe --simulate-device-lost 60 --simulate-device-lost-fatal` で、メッセージボックスに理由と退避先の絶対パスが出る (`--frames` / `--screenshot` を付けた非対話実行ではログだけ)
- [ ] `Editor.exe --frames 120 --simulate-device-lost 30 --simulate-device-lost-stale` (復旧の途中で旧デバイスの子を握らせ、参照数ゲート不合格 → 致命停止) が exit 6 で終わり、クラッシュも assert ダイアログも出ない (RecoverDevice で ImGui を畳んだ後に致命停止する経路。Debug / Release の両方)
- [ ] 退避ファイル `<project>\crash\device_lost_<日時>\<シーン名>.scene.json` ができ、元のシーンファイルは変わっていない (更新日時が同じ)
- [ ] 退避ファイルをエディタで開くと、消失時の編集状態 (配置したオブジェクト・未保存の変更) が復元される
- [ ] Play 中に消失させた場合、退避されるのは Play 開始前の状態で、Play 中に動いた位置は入っていない
- [ ] 終了コードが 6 で、以降に D3D のエラー (Debug のデバッグレイヤ) が出ていない

### (d) 専用サーバのセッション中の復旧

- [ ] `tools\server_verify.bat 600 E` (ケース E: 参加中のクライアントが疑似消失 + 復旧 5 s 遅延。サーバが timeout で切らず、クライアントが正常終了し、`.rep` がサーバと一致し、サーバ `.rep` の再生検証が一致する。UDP と複数プロセスを使うので CI では回らない)

## M89: 骨アニメの深化

自動検証は `Editor.exe --selftest` (AnimatorControllerSelfTest / SkeletonSelfTest / BehaviorTreeSelfTest)、`tools\replay_verify.bat` の
`anim` ジョブ (Debug / Release / `Server.exe`、snapshot stress)。素材は `tools\gen_anim_test_gltf.ps1` が作る
`assets\models\anim_test.glb` / `anim_test_zup.glb` (設計: `docs\adr\ADR-027-skeletal-pose-program.md`、計画: `plans\m89-skeletal-animation.md`)。

### デモ `--anim-demo` (M89b)

- [ ] `Editor.exe --anim-demo` で Play すると、2 体 (左 = Y-up、右 = Z-up の素材) が同じ姿勢で Idle → Walk → Run → Attack → Idle と回り、切り替えの瞬間に飛ばずに混ざる
- [ ] Walk / Run の間は足元の板 (Root) が前へ進み、1 周ごとに元の位置へ戻る (デモは applyRootMotion を切っている。入れたときは下の M89j 節)
- [ ] Animator Controller 窓で `anim_test.controller.json` を開いて保存しても、ステートの骨クリップ (`"skel"`) が消えない

### 型付きパラメータ (M89c)

- [ ] Animator Controller 窓の左の欄で、パラメータの名前・型 (int / float / bool / trigger) を変えられ、値の欄が型に合った部品 (整数 / ドラッグ / チェック) になる。16 個まで足せる
- [ ] 遷移の条件でパラメータを名前で選べる。float は小数で比べ、bool はチェック、trigger は演算と値の欄が消えて「(立っていれば真)」と出る
- [ ] Play 中に trigger のチェックを入れると、その trigger を条件に持つ遷移が始まった瞬間にチェックが外れる
- [ ] 保存した `.controller.json` の `parameters` に `"type"` が入り、float の条件の `"value"` が小数で書かれる。型の無い古いファイルは int として開ける

### 骨アニメの描画補間 (M89f)

- [ ] 144Hz などのリフレッシュレートで `Editor.exe --anim-demo` を Play すると、骨の動きが 60Hz の段々にならず滑らかに見える (補間しない旧経路の SkinnedMesh と見比べる)
- [ ] ループの折り返し (Walk / Run の 1 周の境目) で、1 コマ止まったり跳ねたりしない
- [ ] Play を一時停止・ステップ実行・タイムラインのスクラブ中は、最新 tick の姿勢で止まって見える (補間で前の姿勢へ戻らない)
- [ ] キャラを非アクティブにしている間、姿勢がその場で止まり、細かく揺れ続けない

### コントローラ窓の骨の駆動 (M89g)

- [ ] `Editor.exe --anim-demo` でキャラを選ぶと、ノードの 2 行目に骨クリップ名 (ブレンドなら「1D ブレンド (n)」) が出て、既定ステートに `default` が付く
- [ ] ステートの「骨の駆動」を なし / クリップ 1 本 / 1D / 2D で切り替えられ、行き来しても子やクリップ名が消えない (なしだけが駆動を止める)
- [ ] 骨クリップのピッカーにモデルのクリップ名 (Idle / Walk / Run / Attack) が並び、モデルに無い名前を書くと「(モデルに無い)」と出る
- [ ] 1D / 2D ブレンドで、パラメータの値を動かすと図の赤線 (2D は十字) が動き、近い子の点が太り、子の行の % が変わる
- [ ] Play 中、左の欄の「ポーズの層」に駆動中の SkinnedMesh ごとのクリップ名・時刻・重みが出て、遷移中は 2 層 (ブレンドなら子の数だけ) になる
- [ ] 保存した `.controller.json` に、選んだ種類の `"skel"` だけが書かれる

### アニメイベントの定義と発火 (M89h)

イベントを編集する UI は M89o で入る。それまでは `.controller.json` に `"clipEvents"` を手で書いて確かめる。

- [ ] `"clipEvents":{"Walk":[{"tick":10,"name":"Step"}]}` を書いたコントローラをキャラに付け、同じキャラの BT に `eventName` = `Step` の待ちを置いて Play すると、Walk の 1 周ごとに 1 回だけ反応する
- [ ] Animator Controller 窓で開いて保存しても `"clipEvents"` が消えない

### エンジンが直接処理するアニメイベント (M89i)

- [ ] `{"tick":10,"kind":"sound","sound":"<既存の音のキー>","joint":"<足のジョイント名>"}` で、Walk の 1 周ごとに足の位置から音が鳴る (左右に定位が動く)
- [ ] `{"kind":"effect","prefab":"<既存のエフェクトのプレハブ>","joint":"<足>"}` で、足元にエフェクトが出る (キャラが歩いても 1 tick 遅れ程度で付いてくる)
- [ ] `{"kind":"noise","loudness":1,"range":10}` を付けたキャラの近くに hearingMode = Distance の AIPerception を持つ見張りを置くと、足音で気付く
- [ ] 音のキーを綴り間違えると、無音ではなく `[audio] unknown sound key` の警告が出る
- [ ] 窓で開いて保存しても `kind` と種類別の欄 (`sound` / `prefab` / `loudness` など) と `joint` が残る

### ルートモーション (M89j)

- [ ] `Editor.exe --anim-demo` で 2 体の AnimatorController の「ルートモーションを適用」を入れて Play すると、Walk / Run の間は 2 体 (Y-up と Z-up) がそろって前へ進み、1 周ごとに引き戻されない (足が地面を滑らない)
- [ ] 入れていない間も Inspector の「ルートモーションの速度」に Walk / Run の前進速度が出る (Idle は 0)
- [ ] CharacterController を付けたキャラでは「移動入力」に速度が入り、壁に当たると止まる。Rigidbody を付けたキャラは落下 (縦の速度) が消えない
- [ ] NavMeshAgent の「位置を更新」が入っている間は Nav だけが動かす (速度が倍にならない)。切るとアニメの歩幅で進む
- [ ] 描画補間 (144Hz) でも、前進中のキャラの体が前後に震えない

### ルートモーションのヨー (M89k)

- [ ] ルートが回るクリップ (その場の旋回・曲がりながら歩く) で「ルートモーションを適用」を入れると、エンティティの向きが回り、体はエンティティに対して正面を向いたまま (二重に回らない)。1 周ごとに向きが引き戻されない
- [ ] 曲がりながら歩くクリップでは、1 周目の足元の通り道がクリップのルートの通り道と一致し、2 周目以降は回った向きへ続く
- [ ] Inspector の「ルートモーションの回転」に毎 tick の回転が出る。回らないクリップでは恒等のまま
- [ ] NavMeshAgent の「回転を更新」が入っている間は Nav の旋回だけが向きを決め、体はクリップのひねりを残す。切るとアニメの回転で向きが変わる
- [ ] 描画補間 (144Hz) でも、旋回中の体が左右に震えない

### 2 ボーン IK (M89l)

- [ ] キャラの SkinnedMesh に「2 ボーン IK」を付け、鎖 0 の先端ジョイントに手首 (足首) の名前、モード 1、目標に空のエンティティを入れて Play すると、目標を動かすと手 (足) が付いてくる。届かない位置では腕が伸び切って目標を指す
- [ ] 「曲げる側」を前 / 後ろに置くと肘 (膝) がその側へ曲がる。(0, 0, 0) のままならアニメの曲げ方のまま
- [ ] 重みを 0 → 1 へ動かすと、アニメの姿勢から IK の姿勢へ滑らかに移る (肘が反対側へ飛ばない)
- [ ] モード 2 では手の向きも目標の回転になる
- [ ] 手首に付けた部位 (武器など) が IK 後の手に付いてくる
- [ ] ラグドールが作動すると IK は効かなくなり、骨は物理に従う
- [ ] 先端ジョイントの名前を間違えると、Console に WARN が 1 回だけ出て、その鎖は効かない

### 足の接地 (M89m)

- [ ] 両足の鎖をモード 3 にして段差の上に立たせると、高い側の足は膝を曲げて段に乗り、低い側の足は骨盤ごと下がって地面に着く
- [ ] 「骨盤を下げる上限」より深い段差では骨盤は上限で止まり、足は伸び切る
- [ ] 歩いている間、振り上げた足は浮いたまま、着いた足は斜面・段差の高さに合う
- [ ] キャラ自身のカプセルや手に持った武器のコライダーに足が乗らない
