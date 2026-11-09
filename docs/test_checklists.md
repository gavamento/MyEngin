# 手動テスト手順

自動化済みの検証 (`--selftest-full`, `tools\replay_verify.bat`, `tools\check_rules.ps1`) に加えて、
対話的な機能はこの手順で確認する。ウォッチャー/ローダ周りを変更したら必ず再実施すること。

## Editor 起動スプラッシュ (`--project` 対話起動)

- [ ] `Editor.exe --project <dir>`: スプラッシュが中央に出る。メビウスの輪が回り続け、段階名と進捗が更新される
- [ ] 表示倍率 100% 以外 (125% / 150% / 200% 等): ワードマークから進捗バーまで欠けず、作業領域内に収まる (Hub から開く)
- [ ] 準備完了後、スプラッシュが閉じてメイン窓だけが表示される (白画面のちらつきなし)
- [ ] プロジェクト Hub から開く経路でも同様 (Hub 経由の `--project` 再起動)
- [ ] `<project>\assets\editor\splash.png` を置くと中央が画像になり、輪は出ない
- [ ] `--selftest` / `--frames` / `--screenshot` 等の自動化フラグではスプラッシュが出ない

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

`Editor.exe --project <Shadow_Sound のパス>` → Play (stage1)。

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

イベントはアニメプレビュー窓のタイムラインで編集できる (M89o)。`.controller.json` の `"clipEvents"` を手で書いてもよい。

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

### 2 ボーン IK の自動設定

- [ ] 人型キャラ (例: `assets/models/player_fp_v01/Player_Researcher_FP.fbx`) の SkinnedMesh に Add Component で「2 ボーン IK」を付けると、鎖 0 / 1 に左右の手首 (モード「位置」・重み 0)、鎖 2 / 3 に左右の足首 (モード「接地」・重み 1) が入り、子に「IK Target Hand L / R」が手首の位置にできる
- [ ] 付けただけではアニメの手の動きは変わらず、Play すると足が段差に乗る
- [ ] 手の鎖の重みを 1 にして「IK Target Hand L」をギズモで動かすと左手が付いてくる
- [ ] Undo 1 回でコンポーネントと目標の子がまとめて消え、Redo で戻る (目標の参照も戻る)
- [ ] 先端ジョイント・骨盤ジョイントはスケルトンの骨のドロップダウンから選べ、モードは名前付きのコンボで選べる
- [ ] 鎖を 1 本手で書き換えてから注意欄の「自動設定」を押しても、その鎖は変わらず、目標の子も増えない
- [ ] 手足の語が無い骨名のモデル (例: CesiumMan) では鎖が空のままで、注意欄に見つからない手足が出る

### 骨アニメのプレビュー窓 (M89n)

- [ ] Window メニューの「アニメプレビュー」で開き、キャラを選ぶと窓の中にキャラが立つ。ドラッグで回り、ホイールで寄る
- [ ] クリップモードでクリップを選ぶと再生され、一時停止・|<・-1・+1・スライダで止めた tick の姿勢になる
- [ ] ステートモードで 1D / 2D ブレンドのステートを選ぶと、スライダでブレンドが変わる (シーンの Inspector のパラメータは変わらない)
- [ ] 「骨」を入れるとメッシュの上に骨の線が重なる
- [ ] イベントのあるクリップでは、スライダの下に印が出て、乗せると名前と tick が出る
- [ ] `Editor.exe --anim-demo --anim-preview <キャラ名> --anim-preview-tick 30 --screenshot out.png --shot-frame 30` で窓に 30 tick の姿勢が写る
- [ ] Play 中に開いても、シーンの再生やハッシュに影響しない

### タイムラインでのイベント編集とルートの軌跡 (M89o)

- [ ] コントローラを持つキャラで、スライダの下の帯の空いた所をダブルクリックするとイベントが足され、選ばれて (橙) 欄が出る
- [ ] 「イベントを追加」で今の tick に足される。ブレンドツリーでは最も重い層のクリップに足される (帯の上に対象のクリップ名が出る)
- [ ] 印をドラッグすると位置が動き、乗せている間は名前と tick が出る。「削除」で消え、最後の 1 件を消すとそのクリップの項目が保存から消える
- [ ] 種類を sound / effect / noise / script に切り替えると欄が変わり、音・プレハブのキーは空に戻る。ジョイントはモデルの関節から選べる
- [ ] 窓の「保存」で `.controller.json` の `clipEvents` に書かれ、読み直しても同じ。保存前に Git 操作をすると、コントローラの未保存として扱われる
- [ ] Play 中に足したイベントが、その位置で発火する (BT の eventName / 音 / エフェクト)
- [ ] 「ルートの軌跡」で、Walk / Run のルートの 1 周の道筋が緑の線で出て、今の位置に丸、イベントの位置に点が付く。Idle はほぼ 1 点
- [ ] Z-up のモデル (`anim_test_zup.glb`) でも軌跡が水平に前へ伸びる

### glTF の STEP / CUBICSPLINE (M89p)

- [ ] STEP のクリップを持つ glTF が、キーの時刻ちょうどで値が切り替わって再生される (間で補間されない)
- [ ] CUBICSPLINE のクリップを持つ glTF (Khronos の `InterpolationTest` など) が、接線を値と取り違えずに滑らかに再生される
- [ ] LINEAR だけの既存のモデル (CesiumMan / `anim_test.glb`) の姿勢と replay のハッシュが変わらない
- [ ] 版 5 への上げで古いクック済みキャッシュが読み直される (ログに再クック、姿勢が正しい)

## M90: 描画の軽量化 (LOD / オクルージョン / 影・スキンのカリング / URO)

実装の根拠と測定は `docs\adr\ADR-029-render-optimization.md`。決定的な数 (draw / tri / カリング数) は selftest と `--render-stats-dump` が見る。ここは目で見ないと分からない項目だけ。
画素の A/B は WARP (`--warp`) で取る (実 GPU は M90 の前から run 間で 51 画素・最大差 1 揺れる)。

### メッシュ LOD の遠景 (M90e)

- [ ] `Runtime.exe --render-bench-demo --deferred --width 1280 --height 720 --frames 60` で、手前の LOD の見本 (14 m の 2 個) は元の形、遠ざかる 34 / 43 / 65 m の球が段階的に粗くなる。粗い段でも輪郭が極端に崩れず、陰影 (法線) が破れない
- [ ] カメラを前後に動かして段の境目を往復しても、境目の付近で段がチカチカ切り替わらない (ヒステリシス)。切り替わる瞬間のポップが気にならない程度か判断する
- [ ] `--lod-force 0` で全部が元の形、`--lod-force 3` で全部が最も粗い段になる。`--lod-bias 2` で詳細な段が遠くまで残り、`--lod-bias 0.5` で早く粗くなる
- [ ] `.meta` に `"lod"` を持たない既存のモデル (三校の素材など) の見た目と golden は変わらない
- [ ] Inspector でモデルアセットを選ぶと LOD 設定 (段数・比・画面高さ比) が出る。段数 2 で「適用」すると `.meta` が書かれ、メッシュが読み直されて遠景が粗くなる。段数 0 に戻して適用すると `"lod"` が `.meta` から消える
- [ ] `.meta` を手で編集してエディタに戻ると、キャッシュを消さなくても再クックされて段が変わる (Console に再クックのログ)
- [ ] 硬い面のモデル (`Lab_Door.fbx` など) に段を付けても、段が作れない旨の WARN が 1 回出るだけで、描画は元の形のまま

### オクルージョン (M90b / M90c / M90h)

- [ ] `--render-bench-demo` を `--deferred` あり / なしの両方で、壁の裏の物が描かれない状態で、壁の手前に回り込む (カメラを動かす) と、見えるはずの物が 1 フレームも欠けずに現れる。カメラカット (`--render-bench-cut-frame 40`) の直後も欠けない
- [ ] Rendering メニューの「GPU オクルージョンカリング」を切り替えても絵が変わらない (ProfilerWindow の落とした数だけが変わる)。Scene View と Game View を同時に出して、両ビューの統計が別々に出る
- [ ] `--hzb-debug 1 --hzb-debug-max` で max-Z のピラミッドが出て、落とした物の AABB が赤線で重なる
- [ ] メニューで OFF にして保存し、エディタを再起動すると OFF のまま。`assets\project_settings.json` の `rendering.occlusionCulling` が `false` になり、`rayTracingTags` などの他のキーは残っている
- [ ] `Runtime.exe` もその設定を読む。`--no-occlusion` で起動すると設定ファイルは書き換わらない。`--no-occlusion` で起動したエディタでメニューを操作すると、選んだ値が保存される
- [ ] ツールチップに「プロジェクト設定に保存される」旨が出る。日本語と英語の両方
- [ ] 半透明・水面・サーフェスマテリアル・スキンのキャラが、オクルージョン ON でも消えない・チラつかない
- [ ] 物が少なくユニークなメッシュが多いシーンで、ON が OFF より CPU が重いと感じる場合は ADR-029 の計測を参考に OFF にする

### 画面外キャスターの影とスキンのカリング (M90d)

- [ ] カメラの背後・画面外にある物 (`render_bench` の Tower) の影が、画面内の床や壁に落ちる (以前は消えていた)。カメラを回して物が画面に入る瞬間に影が急に現れない
- [ ] 画面の外へ出たスキンのキャラが消え、画面の端から入る瞬間に姿勢が崩れていない。ラグドール作動中のキャラは画面外でも消えない
- [ ] 腕を大きく伸ばす・IK で足を高く上げるクリップでも、画面の端でキャラの一部が欠けない

### URO (遠いキャラのアニメ間引き) (M90f)

- [ ] 遠くのキャラ (画面の高さの 2% 未満) の動きが、近くのキャラと比べてカクつくかを確認する。カクつきが気になる距離を控える (閾値は目視で調整する前提の初期値)
- [ ] Rendering メニューの「遠いキャラのアニメ更新を間引く (URO)」を切ると、遠いキャラも毎 tick 滑らかに動く。近いキャラの動きは ON/OFF で変わらない
- [ ] 遠いキャラが歩き始める・止まるとき、窓の途中で姿勢が数 tick 遅れて見えても、足が地面を大きく滑らない
- [ ] 巻き戻し (タイムライン) やシーン切り替えの直後に、遠景のキャラが 1 度だけ別の姿勢で出ても、数 tick で正しい姿勢に追い付く
- [ ] Scene View と Game View を同時に出しても、同じキャラの姿勢が両ビューで一致する

### 回帰

- [ ] `--no-jobs` と通常で、`render_bench` のスクショが WARP で画素一致する
- [ ] `tools\shot_verify.bat` で golden 30 枚が PASS する (M90d で更新した 3 枚を含む)

## M91: Compute.ForEach (C++ / C# 共通の計算 API)

実装の根拠と測定は `docs\adr\ADR-030-compute-foreach.md`、仕様は `engine_spec.md` の 10.12。決定的な部分 (決定表・採番・ハッシュ・C++ と C# のバイト一致・外部プロジェクトの生成) は `Editor.exe --selftest` と `tools\replay_verify.bat` (compute / computeab) が見る。ここは GUI と実機 GPU でしか確かめられない項目。
数値の確認は Release の `bin\x64\Release\Editor.exe` で行う (Debug は State のハッシュが約 5 倍遅い)。

### デモの見た目 (受け入れ 12)

- [ ] `bin\x64\Release\Runtime.exe --compute-demo --font-embedded --width 960 --height 540 --frames 130 --shot-frame 120 --screenshot <png>` で、左に火花 (線)、中央に黄色い C++ の群れ (512 体)、右に青い C# の群れ (48 体) が写る
- [ ] 同じ引数に `--compute-force-cpu` を足しても、群れ 2 つは同じ絵になる (火花だけが少し違うのは Fx の GPU 遅れと誤差で、正常)
- [ ] `--warp` を足しても 3 つとも写る

### スクリプトのインスペクターの Compute 欄 (M91h)

- [ ] Editor で `--compute-demo` を開いて Play し、`ComputeDemoSwarm` (C++ State)、`ComputeDemoSparks` (C++ Fx)、`ComputeCsSwarm` (C#) のエンティティを順に選ぶと、スクリプトの欄に「Compute」(日本語表示では「Compute (計算)」) の折り畳みが出る
- [ ] 各行に、カーネル名 (`DemoSwarmStep` / `DemoSparkStep`)、状態 (実行済み)、最終の実行先、要素数、upload / readback (B)、CPU ms / GPU 回数、CPU 版 (一致)、上書き (Auto) が出る。日本語表示 (言語を日本語に切り替える) で文字化けや欠けがない
- [ ] **実機 GPU** で `ComputeDemoSparks` の行が「GPU」を示し、GPU 回数・upload / readback のバイト数が増えていく (sub-08 では GUI の目視を取れていない)。デバイスが無い構成 (`Server.exe`) ではこの行は CPU
- [ ] 上書きを「CPU 固定」にすると、`ComputeDemoSparks` の最終の実行先が CPU になり、「そのカーネルを使うすべてのスクリプトに効く」旨の注記が読める。「Auto」に戻すと GPU に戻る
- [ ] Project Settings の「Compute (計算)」で「上書きをプロジェクト既定として保存」を押すと `assets\project_settings.json` の `"compute"."kernelOverrides"` が書かれ、押さない限り書かれない
- [ ] 狭いインスペクターの幅でも Compute 欄の項目が切れない (縦並び)
- [ ] 宣言していないカーネルを呼ぶように `COMPUTE_KERNELS` から名前を一時的に外して GameLogic を再ビルドすると、行が「失敗: 宣言なし」になり、直し方 (COMPUTE_KERNELS に足す) が出る。C# は `[ComputeKernels(...)]` から外して [Compile C# Scripts]

### `.kernel.hlsl` の編集と版ずれ (受け入れ 11)

- [ ] Play 中に `assets\shaders\kernels\DemoSwarmStep.kernel.hlsl` を保存すると、コンソールに `[compute] kernel source changed - recompiling C# scripts` が出る。数秒フレームが止まるのは既知の制限 (ADR-030)
- [ ] 保存の直後、C++ の `ComputeDemoSwarm` の行は「CPU 版: 古い — GameLogic を再ビルド」になり、群れが止まる (State の ForEach が失敗し、バッファの中身は変わらない)
- [ ] C# の `ComputeCsSwarm` の行は再コンパイルのあと「一致」に戻り、群れが動き続ける
- [ ] GameLogic を Rebuild (または [Rebuild Scripts]) すると、C++ の行が「一致」に戻り、群れが再び動く
- [ ] `ComputeDemoSparks` (Fx) は保存の直後も描かれ続ける (GPU 版がシェーダのホットリロードで作り直される)
- [ ] 編集で `1.0` (f の無いリテラル) を書いて保存すると、検査違反 (`KL006`) がコンソールにファイル名と行つきで出て、行が「検査違反」になる。直して保存すると戻る
- [ ] メニューの [Rebuild Scripts] と [Compile C# Scripts] をそれぞれ押して、エラーなく終わり、Compute 欄が「一致」になる

### 外部プロジェクト (受け入れ 17)

- [ ] 外部プロジェクト (例: 三校) を開いて [Rebuild Scripts] を押すと、`<project>\cache\Generated\Kernels.gen.{h,cpp}` ができ、`<project>\assets\shaders\kernels` にカーネルを置いて宣言したスクリプトが State の ForEach に成功する。エンジンの `SelfTest*` カーネルはそこには出ない
- [ ] カーネルを編集して再ビルドしないで Play すると、コンソールに `hash-mismatch` が出て結果が変わらない

### 改行 (sub-13 の GUI 確認と docs\line-endings.md)

- [ ] 外部プロジェクトまたはこのリポジトリでシーンを GUI から保存し、`git status` に改行だけの差分が出ない (LF のまま)
- [ ] アセットの `.meta` を作り直す操作 (アセットの再インポートや新規ファイルの追加) のあとも、`git status` に改行だけの差分が出ない
- [ ] [Rebuild Scripts] のあとも、`git status` に改行だけの差分が出ない。出たら `tools\check_rules.ps1` の規則 14-c の指示に従う
- [ ] `docs\line-endings.md` に書いた Visual Studio のメニュー名 (編集 > 詳細 > 改行コードの設定 / Edit > Advanced > Set Line Endings) が実機の VS 2022 にある。無ければ文書を直す (実機で未確認)

### 実機 GPU

- [ ] 実機 GPU (RTX 3060 以外でもよい) で `--compute-demo` を Release で出すと、`ComputeDemoSparks` の最終の実行先が GPU になる。Saxpy 相当の GPU と CPU の差 (最大 64 ULP の見込み) は Fx だけに現れ、State (C++ / C# の群れ) の絵は WARP や `--compute-force-cpu` と同じ
- [ ] 実機で `Editor.exe --selftest` を回して落ちる項目は、WARP 前提の許容差 8 項目 (「GPU と CPU の差が 1〜2 ULP 以内」) だけであること。それ以外が落ちたら不具合 (ADR-030 の決定 14)

### 回帰

- [ ] `tools\check_rules.ps1` が 0 error
- [ ] `tools\replay_verify.bat` が PASS し、PASS 行に `compute` と `compute A/B` が含まれる
- [ ] C# と C++ のバイト一致を JIT 設定を変えて確かめ直す場合は ADR-030 の D24 の手順に従う (`KernelCompat.cs` / `KernelCSharp.cpp` を変えたとき)

## M94: 2D レーン (横スクロール)

仕様は `plans\m94-2d\spec.md`、まとめは [engine_spec.md](../engine_spec.md) §6.15 / §10.13、判断は [ADR-033](adr/ADR-033-2d-lane.md)。決定的な部分 (平面拘束・タイル衝突・GridAgent2D・Camera2DFollow・ハッシュの焼き込み・版の固定) は `Editor.exe --selftest` (`TwoDDemoSelfTest` ほか) と `tools\replay_verify.bat` の `2d` ジョブ、画素は golden `2d` / `2d_lights` が見る。ここは **GUI の実クリックと目視でしか確かめられない項目**で、各サブの判定が「ユーザー目視」に回したものを全部集めてある。アセットは `python -I tools\gen_2d_assets.py` で `assets\2d\` に作り直せる (GUID と seed は固定)。

### デモの見た目 (`--2d-demo`)

- [ ] `bin\x64\Release\Runtime.exe --warp --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --2d-demo --screenshot <png>` で、320x180 を 3 倍にした画素の絵になり、左端の壁・壊れるブロック (2 列 3 段)・一方通行板の上の主人公・左の丘・ロープのたるみ・視差の山と木・段差・敵・右端の箱の山が写る
- [ ] `--2d-night` を足して `--frames 125 --shot-frame 120` で撮ると、Global の暗い青と、吊りランプ・主人公のランタンの 2 灯、箱・主人公・移動床の影が写る。主人公の法線で明暗が付く
- [ ] `--froxel --deferred` を足しても真っ黒 / NaN にならず、昼の絵と同じ (golden には入れない。2D の経路には froxel / deferred が効かない)
- [ ] Editor で `--2d-demo` を開いて Play すると、主人公が (入力なしでは) 立ったまま、敵が段差と一方通行板を越えて主人公へ向かう。矢印や A / D / Space / J で主人公を操作でき、壁へ押し付けると前のブロックが削れて壊れる。S (下) で一方通行板を降りられる
- [ ] 移動床 (kinematic) に乗った主人公が床と一緒に運ばれる。箱 (plane2D 剛体) を押して動かせる
- [ ] Edit モード (Play していない) ではアニメーションタイルがコマ 0 のまま止まって見える (仕様どおり。Play 中と Runtime では動く)
- [ ] 実機 GPU でも `--2d-demo` の絵が WARP と大きく違わない (ライトバッファ RGBA16F、ステンシルマスク、ピクセルパーフェクトの blit)。違う場合は Deferred / Forward / `--froxel` の組み合わせも試す

### Create メニューと Inspector

- [ ] Create → 2D の各項目 (Sprite / Tilemap / Light 2D / Sprite Shape / Line / Effector 2D / Grid Agent 2D / Camera 2D) で、エンティティが作られ Undo で戻る。Shadow Caster 2D には Create 項目が無い (Add Component の 2D 分類で足りる)
- [ ] **Create → 2D → Camera 2D** の Camera が正射影で、`orthoSize` が 0.9 (= 基準解像度 Y 180 / (2 × PPU 100)) に初期化されている。Inspector に「PixelPerfect と合わせる」注記が出る (sub-23 で直したが実際の Create 操作は未実施)
- [ ] Camera 2D の Inspector (PixelPerfectCamera2D) の有効値が、直近のゲームビューの寸法に対する RT と orthoSize を示す。Play 前に何も描かれていないときの表示も確認する
- [ ] Add Component の「2D」分類に Sprite2D / SortingGroup2D / SpriteMask2D / Tilemap / Light2D / ShadowCaster2D / Parallax2D / Line2D / SpriteShape2D / Effector2D / GridAgent2D / Camera2DFollow / PixelPerfectCamera2D が並ぶ
- [ ] Sprite2D の `sprite` に AssetRef ピッカーが出て (一覧にスライスが名前で並ぶ)、テクスチャの D&D は全面スプライトとして受理される。`normalMap` のピッカーはテクスチャだけを一覧し、ドロップも効く
- [ ] Inspector の警告行が読める: Sprite2D の srgb = on の警告 / normalMap の srgb 警告 / ソート層が無い id の警告 / Effector2D の警告 (Surface でトリガー Collider など) / GridAgent2D の tilemap 未設定 / Tilemap のワールドスケール無視の注記 / plane2D の「異なる z は当たらない」注記
- [ ] スプライトのサーフェスシェーダ (`SpriteFx.surface.hlsl` を Material に割り当てた Sprite2D) で flash / 輪郭 / dissolve が効く。使えない材質を割り当てると内蔵で描かれ Inspector に理由が出る
- [ ] Joint の Inspector の **「2D (Z 軸)」ボタン**を押すと axis が (0,0,1) になる (Hinge)
- [ ] Light2D の Inspector の実操作: type の切り替えで項目が出入りし、色・強度・半径・角度のドラッグが Scene View のライトに追従し、Freeform / Sprite の形の指定と Undo が働く

### Project Settings のソート層

- [ ] Project Settings の Sorting Layers で追加・削除・改名・並べ替え (Up / Down) の実クリックが働く。id 0 "Default" は削除できず、32 を超えて追加できない。Remove のあとの空き id が再利用される

### Scene View の 2D モード

- [ ] ツールバーの「2D」トグルで XY の正射影になり、ホイールでズーム、中 / 右ドラッグでパン、F で選択へ寄る。OFF で元の 3D 視点に戻る。ImGuizmo が XY 並進 / Z 回転 / XY 拡縮だけになり、XY グリッドが出る
- [ ] ギズモの色: Sprite2D の矩形 (白) と pivot (黄の十字)、2D コライダー (緑)、Tilemap の衝突箱 (`showCollision`、緑)、Light2D (黄。type ごとの輪郭)、ShadowCaster2D (紫)、Camera2DFollow の deadZone と bounds (青)、GridAgent2D の次セル (橙)、Effector2D の矢印 (橙) が出る
- [ ] Perception の `forwardAxis` が XY (2D) のとき、知覚扇形のギズモが XY 平面に出て、視野角と距離が Inspector の値に追従する
- [ ] Line2D / SpriteShape2D の点ハンドル: 点を選ぶまでは変形ギズモのまま。点のドラッグ・点選択中の辺クリックで追加・Delete で削除・Esc が働き、1 ドラッグ 1 Undo
- [ ] SpriteShape2D は透視カメラ・回転 / スケール付きの親の下でも、縁と塗りの見た目が正射影・親なしのときと破綻しない (縁がずれず、塗りが裏返らない)
- [ ] `.shape2d.json` (Polygon2D / Edge2D Collider) の点ハンドル: ドラッグ、辺クリックで追加、Delete (Undo は無く、ドラッグ 1 回 = 保存 1 回)
- [ ] Scene View のクリック選択が Sprite2D を選ぶ (Line2D は選べない = 仕様の限界。Hierarchy か点ハンドルから)
- [ ] 非 Play の Scene View で Light2D が効いた見た目になる (ライトの配置が見える)。Scene View の色はエディタカメラの ACES で淡く出る (ゲームカメラの契約とは別)
- [ ] GameView 上の PixelPerfect: ウィンドウサイズを変えても整数倍の拡大で黒帯が出ず、ドットが滲まず、UI の文字はフル解像度で鮮明。TAA / FXAA を選んでいても滲まない

### スプライトエディタ

- [ ] Project 窓でテクスチャをダブルクリックするとスプライトエディタが開く (`.dds` など stb が読めない形式は OS 既定アプリ)。Inspector の [Open Sprite Editor] と Window メニューからも開く
- [ ] キャンバスの操作: ズーム / パン / 矩形のドラッグ移動・リサイズ / 矩形の追加 / Delete / Ctrl+S / Undo・Redo。グリッド分割・自動分割・pivot プリセット 9 種とカスタム・border・名前・PPU・filter を編集して Apply でき、Point 表示で滲まない
- [ ] 未保存のまま閉じる / 別テクスチャへ切り替えるとモーダルが出る。ゲート表示 (Play 中や Source Control の状態) が出る
- [ ] Apply 後に Sprite2D の表示がすぐ変わる。再スライスのあと古い id を指す Sprite2D がマゼンタに見える (id は使い回されない)
- [ ] Inspector の Import Settings (filter / PPU) の Apply で表示が変わる。srgb 警告行が読める

### スプライトアニメ / コントローラ窓

- [ ] Project 窓の Create → Sprite Animation で `.spriteanim.json` が作れ、Inspector でコマの追加・削除・順序・ticks・ピッカー・「スライスをまとめて追加」・ステップ再生・保存 / 戻すが働く
- [ ] コントローラ窓のステートに Sprite (スプライト列) を指定でき、骨クリップとは排他になる (併記は WARN)。ノードの 2 行目に "sprite: Walk"、ポーズ層の欄に現在コマ番号が出る。プレビュー窓 (M89n) でスプライトのコマが出てステップ再生できる
- [ ] Play 中にスプライトを手で書き換えても次の tick で Animator が書き戻す

### タイルパレット / タイルマップ

- [ ] タイルパレット窓で塗り (B)・消去 (D)・塗り潰し (G)・矩形・ピック (I)・選択 (S) とその Delete・Undo / Redo (Ctrl+Z / Y)・保存 (Ctrl+S) ができ、窓を開いている間は Scene View の 2D で左クリックでも塗れる (「Paint in Scene View」で外せる)。キーは窓フォーカスか Scene View 上のカーソルのとき、Ctrl+S / Z / Y は窓フォーカスのとき。Scene View フォーカスの Ctrl+Z はシーンの Undo
- [ ] タイルパレットのキー U (矩形) で矩形ツールに切り替わり、ドラッグで矩形に塗れる (キーは窓フォーカスか Scene View 上のカーソルのとき)
- [ ] 別アセットへ切り替えるときのモーダル、Tilemap コンポーネントの [Open Tile Palette] ボタン、Window メニューから開く。Play 中は塗れず、理由が折り返して全部読める
- [ ] パレットのタイル追加 (スプライトから / スライス済みテクスチャから一括) と削除、ルール (8 近傍トグル) とアニメ (ticksPerFrame・コマ) の編集、レイヤーの追加・削除・順序が働く。InputText で名前を編集できる
- [ ] 初回のドラッグが一瞬 (約 110 ms) 止まっても許容。連続ドラッグで目立って重くならない
- [ ] 保存すると ReloadHub が読み直して Scene View の絵が変わる。マップとパレットの Undo は別で、Ctrl+Z は直前に変わった側から戻る
- [ ] Tilemap の `showCollision` を ON にすると衝突箱 (緑) が出て、Play 中に SetCell で消したセルの箱も消える

### 回帰

- [ ] `tools\check_rules.ps1` が既存の 3 error のまま (15-e が 0)
- [ ] `tools\replay_verify.bat` が PASS し、PASS 行に `2d` が含まれる
- [ ] `tools\shot_verify.bat` が 32 枚 PASS する (`2d` / `2d_lights` を含む)。`demo_terrain_deferred` は golden と maxDiff 5 (WARP の版差で M94 以前から) のままで、基点の絵とは 0

## M87: エンジン MCP サーバ (mye_mcp)

使い方は `docs\mcp-server.md`、判断は `docs\adr\ADR-032-engine-mcp-server.md`。JSON-RPC の応答・パストラバーサルの拒否・notes の往復・固定 10 問は `tools\mcp_verify.bat` と `tools\mcp_verify.ps1 -RealIndex` が見る。ここは実クライアントと実端末でしか確かめられない項目。

- [ ] `cargo build --release --manifest-path tools\mcp\Cargo.toml` の後、リポジトリのルートで `claude` を起動して `.mcp.json` の承認をすると、`claude mcp list` で `mye-engine` が接続済みになる
- [ ] 実クライアントから `search` を呼んで (例: 「音の遮蔽」)、`AcousticVolume` など該当 component が返る
- [ ] 実クライアントから `verify_job` を 1 本 (例: `demo`、ticks 60。Debug と Release の `Editor.exe` をビルド済みで) start し、poll で `state: done` / `result: PASS` になる。この間に全体実行の `replay_verify.bat` を走らせていない
- [ ] 実端末ウィンドウ (ツールのコンソールではなく) から `claude` を起動して `verify_job` を完走させた前後で、その端末の `chcp` の出力が変わらない (ツールのコンソールでは 65001 のまま不変を確認済み)
- [ ] 実クライアントから `note_append` (topic 例: `m87-check`) → `note_read` で、追記した本文と UTC の見出しが返る。`git status` に `.mye` が出ない。確認後に `.mye\notes\m87-check.md` は手で消してよい
- [ ] 実クライアントから `search` で `kind: ["note"]` を指定すると、追記した topic が `src: "notes"` で返る
- [ ] `target` を一時的に別名にして Claude Code を起動すると、`mye-engine` だけが失敗扱いになり他の機能は使える (`docs\mcp-server.md` の「未ビルドのとき」)

## M94 (GitHub 側): 手触り (ヒットストップ・スロー・揺れ・振動・画面効果・カーブ・ばね・先行入力)

自動の確認: `Editor.exe --selftest` の `FeelMath` / `FeelSystem` / `FeelLibrary` / `SpringFollow` と、`GameFlow` / `SimSnapshot` /
`InputActions` / `Undo` / `Part` (ABI v31) の追加分、`tools\replay_verify.bat` の `feel` ジョブ。設計は [ADR-035](adr/ADR-035-game-feel.md)、
計画は `plans/m94-game-feel.md`。以下は CI では見られない見た目・実機・操作の確認。

### 見た目 (`Editor.exe --feel-demo --autoplay`、Game ビュー)

- [ ] 約 1 秒後 (tick 60) に「HeavyHit」: 物理 (ボール) が数 tick 止まり、その間もカメラが揺れ、画面が一瞬明るく周辺が暗くなる
- [ ] tick 150 のヒットストップでボールとリグ (黄色い箱) が同時に止まる (リグは `MyeIsSimStepping` を見ている)
- [ ] tick 200 からのスローで物理がゆっくりになり、イージングで等速へ戻る (低い % ではカクつく — 既知の制約)
- [ ] tick 300 の揺れが tick 310 で止まる (`MyeStopFeel`)
- [ ] カメラがリグを少し遅れて追い、わずかに行き過ぎて戻る (`SpringFollow`、減衰比 0.8)
- [ ] ボールが床で跳ねるたびに小さく揺れる (`FeelTrigger` → `Bump`)。弱い跳ねでは鳴らない (`minImpulse`)
- [ ] Scene ビューのカメラは揺れない (`CameraOverride` には掛けない)

### 実機のパッド

- [ ] XInput のパッドで `--feel-demo` を Play すると HeavyHit / Bump で振動し、時間で弱まって止まる
- [ ] スクリプトの `SetPadVibration` と重なったとき、強い方が出る (足し算で振り切れない)
- [ ] ウィンドウのフォーカスを外すと振動が止まる (既存の出力レーンの抑止)

### プリセットの編集 (Assets > `assets\feel\HeavyHit.feel.json`)

- [ ] Play 中に Inspector で揺れの振幅などを変えると、次の「プレビュー」から効く。プレビューはヒットストップ / スローを鳴らさない (sim を変えないため)
- [ ] 録画中 (`--record`) にプレビューを押しても、`--verify` の照合が割れない (プレビューは描画専用の表へ鳴る)
- [ ] 値を変えると「未保存の変更あり」が出て、Save で消える。Revert でディスクの値へ戻る
- [ ] 値の変更とカーブの編集を Ctrl+Z / Ctrl+Y で戻せる。ドラッグ 1 回が 1 件にまとまる
- [ ] カーブを「カスタム」にすると今の形の 3 点から始まる。点のドラッグ・ダブルクリックで追加・右クリックで削除・「接線をなめらかに」が効き、ドラッグ中に点がマウスから逃げない
- [ ] Assets の Create > 手触りプリセット で新規作成でき、`FeelTrigger` の「手触り」欄の候補に出る
- [ ] ファイルを外部エディタで書き換えると読み直される (コンソールに `[reload] feel reloaded`)

### 入力 (Project Settings > 入力)

- [ ] アクションの「先行受付」に 6 を入れて保存し、Play 中に押すと B 表示が 6 tick 点く。手触りモニタの先行入力にも出る
- [ ] 軸の「スティックの応答」で InQuad を選ぶと、スティックを半分倒したときの値が小さくなる (ライブ値のバー)
- [ ] 新しいキーを使っていない `actions.json` を保存しても `git status` に差分が出ない

### 手触りモニタ (Window > Feel Monitor)

- [ ] Play 中に鳴らすと再生中の表に出所 (プリセット名 / コード)・強さ・経過 / 持続・チャネルが出て、満了で消える
- [ ] 「エディタではカメラの揺れを切る」を入れると Game ビューの揺れが止まり、振動と画面効果は残る。エディタを再起動しても設定が残る

### タイムトラベル・What-if

- [ ] HeavyHit の直後へ Timeline をスクラブすると、揺れと画面効果がその tick の状態で出る (巻き戻ると揺れも戻る)
- [ ] 一時停止中に Inspector でプリセットを変えても、再生中の手触りの値は変わらない (発火時のコピー)

### 回帰

- [ ] `tools\check_rules.ps1` が 0 error (規則 10 のローカライズ、規則 11 の ABI v31 = 181)
- [ ] `tools\replay_verify.bat` が PASS し、PASS 行に `feel` が含まれる。既存のペアも全て一致する
- [ ] `tools\shot_verify.bat` の既存のスクリーンショットが変わらない (手触りを使わないシーン)
- [ ] 外部プロジェクト (三校など) は GameLogic.dll / MyeScripting.dll を再ビルドしないと v33 以前として拒否される

## DeepModalModel: DeepModal 用のプロシージャル形状生成器 (2026-10-11)

対象: `DeepModalModel\` (形状生成 → `Editor.exe --modal-voxelize` → 受理検査 → `dataset.py` 用 list)。手順は `DeepModalModel\README.md`。

- [ ] `cd DeepModalModel && python -m pytest` が全件 PASS する (Editor.exe があれば `test_editor_smoke.py` も走り、受理率 ≥ 85%・非連結 ≤ 2%・同じ OBJ の 2 回ボクセル化がバイト一致する)
- [ ] `run_pilot.bat` (1,000 件) の `stats.json` で、受理率 ≥ 80%、`disconnected` が 1% 未満、占有の中央値が 1,000〜1,800、`unique_coarse_keys / accepted` ≥ 0.9
- [ ] `data\pilot\qa\sheet_random.png` と `sheet_<family>.png` を目視し、カップ・トレイ・管に開口がある (interior が占有の半分未満)、歯車に穴と歯がある、人型・四足に手足がある
- [ ] `run_full.bat` を途中で Ctrl+C して同じコマンドで再開しても、`manifest.jsonl` の同じ index の `occ_hash` が一致する (決定性)
- [ ] `generate.py stats --out data\v1` の `fem_hours_est_jobs11` を確認してから `run_dataset.bat` を回す (50,000 件で 35〜40 時間の見込み)
- [ ] FEM 完了後、`tools\deepmodal\data\synth_v1\stats.json` の `count_error` が 0、`mode_count` の中央値と `coverage.mean_ratio` が ModelNet40 (67 / 0.63) と同程度で、ファミリ接頭辞別に「無音 (mode_count==0)」が偏っていない

## M95: アクション戦闘の土台

仕様は `engine_spec.md` §10.14、判断は `docs\adr\ADR-034-character-contacts.md`。契約の機械検証は `MYE_SELFTEST_ONLY=m95` (physics / animatorcontroller / skeleton / part / twodabi / simsnapshot / worldhasher) と `--selftest-full` が見る。ここは目視が要る項目。一時プローブのスクリプトで確かめ、絵は `--screenshot` で撮る。

- [ ] Animator Controller ウィンドウの遷移リストに「割り込み可」チェックがあり、ON にして保存した `.controller.json` に `"canInterrupt": 1` と `"controller": 3` が出る。再読込でもチェックが残る (受け入れ 20)
- [ ] 遷移中に割り込み可の遷移の条件を満たすと、別の遷移へ切り替わる (元の遷移の半分を過ぎていれば遷移先から、未満なら元から。ポーズが飛ばない)。割り込んだ tick の `stateEntered` は 0
- [ ] Animation Preview ウィンドウのイベント帯で、tick スライダの右に `= 0.500 s` のような秒が併記される (tick / 60)。スライダを動かすと値が追従し、保存した JSON のイベントは `tick` のまま (受け入れ 28)
- [ ] `clipEvents` を `{"fps": 30, "events": [{"frame": 12, ...}, {"time": 0.5, ...}]}` で手書きして読み込むと、イベントが tick 24 / 30 で発火する
- [ ] ヒットストップ: スクリプトから `AnimatorSetPause(entity, 6)` を呼ぶと、そのキャラのアニメーションと移動が 6 tick 止まり、Inspector の `pauseTicks` が 6 から 0 へ減る。止まっている間イベントは発火せず、止まりが明けたら続きから再開する
- [ ] 部位ヒット: タグ付きの部位に `OverlapSphereParts` / `SphereCastParts` / `OverlapCapsuleParts` を当て、当たった部位だけがデバッグ描画などで確認でき、`dir` を正規化しないと距離が狂うことを踏まえてスクリプトで正規化している
- [ ] CC 同士: `collideCharacters` を両方 ON にした 2 体を体当たりさせ、`pushPriority` が高い側が押されず低い側だけが押し出される。同値なら互いに止まる。`reportContacts` の側で `OnCollisionEnter` が届く。互いに近づく同優先度の 2 体は最大 1 歩ぶん重なることがある (仕様)
- [ ] 壁際の体当たり: 壁の手前の低優先度 CC をボス (高優先度) が体当たりで押し続けても、相手が薄い壁を抜けず、ボスが相手の手前で押し続ける
- [ ] 跳び込み斬り中のヒットストップ: `rootMotionY` のステートで宙にいる最中に `AnimatorSetPause` を呼ぶと、止まっている間は高さを保って宙で止まり、明けたら続きから再開する
- [ ] 縦ルートモーション: `"skel":{"rootMotionY":1}` のステートで CC / 非 kinematic Rigidbody がクリップの高さどおりに跳び、クリップの終端で重力へ戻って着地する。歩き・待機のステートには付けていないので段差で浮かない
- [ ] 一方通行床の下から縦ルートモーションで跳ぶとき、通過が縦入力の初回 tick だけ 1 tick 遅れる (`jumpSpeed` と同じ。仕様)

### 回帰

- [ ] `MYE_SELFTEST_ONLY=m95` を設定した `Editor.exe --selftest` が PASS する。`Editor.exe --selftest-list` で名前一覧が出る
- [ ] `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full` が全スイート PASS する (既知の不安定: `fractureeditor` の cancel 系と `computemanaged` の FileWatcher 系は負荷や実行順で時間依存の FAIL を起こすことがある。M95 とは無関係)
- [ ] `tools\check_rules.ps1` が 0 error
- [ ] `tools\replay_verify.bat` が PASS する
- [ ] 三校の `verify.bat` は `tools\build_scripts.bat` で GameLogic.dll を ABI v32 で作り直してから回す。既存の 2 件 FAIL (shot main の img-diff / patrol) は M95 以前から

## M96: かき乱しフィールド（動く霧と、その汎用化）

仕様は `engine_spec.md` §10.16、判断は `docs\adr\ADR-036-disturbance-field.md`。契約の機械検証は `MYE_SELFTEST_ONLY=disturbance` (全部は `disturbancelong` を含む `--selftest-full`)、`Editor.exe --field-bench`、replay_verify の `field` ジョブが見る。ここは目視・実機・Editor の操作が要る項目。デモは `Runtime.exe --field-demo`、絵は `--screenshot` / `--shot-frame` で撮る。

### 霧と描画

- [ ] `--field-demo` で、球が通った筋の霧が薄くなり、時間で戻る。霧の広場の絵に**流れの市松模様のムラ**が出ていない (投影は幅 2 の 5 点なので、出たら planner へ。ADR-036 の制限)
- [ ] 斬撃 (Cone。デモのスクリプトと `assets\anims\field_swing.controller.json` のアニメイベント `disturb`) の扇が霧の絵で読める (帯状にぼやけて読みにくければ霧のコントラストを調整する)
- [ ] 霧のある場面で雪の跡 (地形の跡) が読める (通常のデモの濃さの霧では読みにくい場合は、霧を薄くした変種で確認する。shot_verify に跡の golden を足すなら霧を薄くした場面が要る)
- [ ] 池の波紋が霧の層の下で見える (`--field-demo` の池)。波紋の注入は有界の寄せなので小さめ。ripple の強さと水面のスペキュラの調整はユーザーの目視で決める
- [ ] Gerstner サーフェス (`WaterGerstner.surface.hlsl`) の水面に波紋が出る。サーフェスのマテリアルの水面と波紋の層を置いた一時の場面で撮る (fxc のコンパイルだけでは絵を確認していない)
- [ ] 草 (`GrassField.surface.hlsl`) を 441 本置いた場面で、草なしと比べた GPU 時間の増分を測る (絶対値だけでなく差を見る)。倒れの係数 (`_BendPerSpeed` / `_MaxBend`) を目視で調整する
- [ ] GPU 粒子と水面が、丘の上の層 (`followTerrain`) で流れ・波紋に乗る
- [ ] `water_surface.hlsl` の `ApplyFog` の引数の並びの修正 (W4 統合後の追補) で、Fog コンポーネントを持つ水面のシーン (三校・HAL Collector にあれば) の見た目が「正しい霧」へ変わる。三校の verify の絵の比較が落ちたら、この修正が原因か断定する
- [ ] 霧のノイズ: `Runtime.exe --fog-noise-demo` で、左の「ノイズなし」の帯は一様、中の「数値」の帯に斑のムラ、右の「マップ」の帯に横に流れる筋が出て、Play 中に模様がゆっくり流れる。奥の Volume の煙にもムラが出る。球が通った筋はノイズの帯でも晴れる (sim はノイズを読まない)
- [ ] 霧のノイズ: Inspector で「ノイズの強さ」「マップノイズの強さ」を動かすと、その場の霧の絵だけが変わる。「ノイズマップ」に別の画像を入れて差し替わる。強さ 0 に戻すとノイズなしの絵に戻る。編集中 (Play 前) は模様が止まっている
- [ ] 霧のノイズの調整値 (デモの強さ・大きさ・流れの速さ、`fog_noise.png` の模様) を目視で決める。Volume の煙は箱の形が残りやすい (フロクセルの解像度と縁の立ち上げ 2 マス) ので、見え方を確認する
- [ ] 霧の嵩: `Runtime.exe --fog-kick-demo` で、人が蹴った所に穴が開き (床が見える)、穴の周りに霧が盛り上がり、十数秒かけて穴が埋まって平らに戻る。WASD で歩き J / 左クリックで蹴れる。操作をやめて 5 秒で自動に戻る
- [ ] 霧の嵩とコライダー: `--fog-kick-demo` (または `C:\HAKtokyo\FogKickDemo`) で人が歩いた跡に溝が残り、ゆっくり埋まる。箱や球のコライダーを層に置くと、その中の霧が消えて周りに積もる。「コライダーを無視」を on にすると押しのけなくなる
- [ ] 霧の嵩の調整値 (蹴りの半径・速さ・tick 数、`massGravity`、`fogDensity`) を目視で決める。縁の盛り上がりが霧の明るさで読めるか (霧には自己影が無いので、起伏は厚み = 白さの差で見せている)
- [ ] Forward でスカイボックスの無い背景にも地面の霧が掛かり、Deferred と同じ霧に見える (地平線向きのカメラ)。霧の後ろの粒子・文字が霧色に潰れない

### エディタ (GUI)

- [ ] Play / Stop で霧が元の静止状態に戻る (Stop の後に Play 前の絵になる。sub-02 の確認は Editor の操作では行っていない)
- [ ] Scene View で霧が見える。Play 前も静止状態の霧が見え、Play で動く。追従の層 (`followTarget`) がプレイヤーに付いてくる
- [ ] ヒットストップ (手触りの `MyeHitStop`) で霧も止まり、明けたら続きから動く
- [ ] Asset Browser のサムネイル (別の RenderSystem) が地面の霧に染まらない (sub-03 は CLI で撮れなかった)
- [ ] Add Component の「VFX」の分類に「かき乱しフィールド」「かき乱しの源」が出て、Inspector の欄が日本語名で表示される。`autoFroxel` の説明に、自動で回ること・GPU の約 7 MB と注入パスの時間・止め方が書いてある
- [ ] Profiler に `disturbance` のスコープが出る (場のあるシーンの Play 中)
- [ ] 場だけのモード: `restDensity > 0` の Volume を置くと Play 前に Scene View で煙が見える。GroundLayer だけのシーンではフロクセルのパスが走らない (Profiler にフロクセルの時間が出ない)。Volume を消す / `autoFroxel` を切ると、退出の INFO ログが 1 行出る

### 回帰

- [ ] `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full` が全スイート PASS する
- [ ] `tools\check_rules.ps1` が 0 error。`tools\check_rules_selftest.ps1` が PASS する (規則 18 の自己検査。replay_verify の `rules` ジョブも回す)
- [ ] `tools\replay_verify.bat` が全ジョブ PASS する (`field` ジョブと jobs A/B を含む)
- [ ] `tools\shot_verify.bat` が PASS する (撮り直した `fog.png` を含む。`flow_title` の FAIL は M96 着手前からの別件)
- [ ] 三校の `verify.bat` が既存の 2 件 FAIL 以外増えない
