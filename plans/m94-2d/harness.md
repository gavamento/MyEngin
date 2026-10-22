# harness 台帳: m94-2d

- 依頼原文: 2D レーンと2D分野をさらに拡充させたい
- 開始: 2026-10-20 / 基点コミット: 25fb355
- フェーズ: レビュー (round 1 FAIL → sub-25 で修正中。全 24 サブ OK 2026-10-22。策定完了 2026-10-20。spec.md 確定: 疑い 31 件、受け入れ条件 40 件、サブ 24 本)

## ユーザー判断 (策定前に確定、2026-10-20)

1. 2D スケルタルアニメ (ボーン・ウェイト・スワップ) は**やらない**。スプライト列アニメ (スプライトシート) があればよい。
2. 見下ろし (Y ソート・XZ) は**やらない**。横スクロール (XY 平面、Z は奥行き) だけ。
3. それ以外の論点は司会 (前セッション) の**推奨案で進める**: 3D ソルバへの平面拘束で 2D 物理 / XY 平面 /
   タイルは `.tilemap.json` アセット / ピクセルパーフェクトは黒帯を作らず整数倍 (Unity の Crop Frame = None 相当) /
   2D ライトは範囲に含める / 規模は design-draft の段階 1〜5・7・8 (段階 6 の骨を除く)。
4. **planner の質問は推奨案で通す。** planner は AskUserQuestion で止まらず、自分の推奨で裁定して
   `[ユーザーに聞ける]` の印で返す。司会はそれを Notion 活動記録「M94 2D: ハーネスの保留質問」へ記録し、
   ユーザーが後で変えられるようにする (M80 の Q-13〜16 と同じ運用)。
5. planner / coder は **AGENTS.md を最初にしっかり読む** (第 4 章の技術制約、第 5 章の規約、第 7 章の検証)。
6. 事前調査は `plans/m94-2d/design-draft.md` (司会が書いた)。計画の仮定は planner が実コードで裏取りする。
7. (2026-10-20 追加) **coder はサブエージェントを同時 3 台まで使ってよい** (調査・裏取り・検証の並列化)。
   司会は各 TASK の prompt にこれを書く。coder の定義で許されている型は Explore (読み取り専用) なので、
   実装そのものの並列化ではなく調査の並列化に使う。
8. (2026-10-20 追加) **reviewer もサブエージェントを同時 3 台まで使ってよい** (4 軸の分担、コミット範囲の読み分け、
   検証の並列実行)。司会は REVIEW の prompt にこれを書く。定義上の型は Explore (読み取り専用)。
9. (2026-10-20 追加、sub-01 実装中) **重い検証 (`--selftest` 全体、`replay_verify.bat`、`shot_verify.bat` と golden 30 枚の照合) は
   サブ 5 本につき 1 回でよい** (回すのが長すぎる)。チェックポイント = sub-05 / sub-10 / sub-15 / sub-20 / sub-24 の完了時
   (sub-23 のデモは golden を新設するので sub-23 でも回す)。各サブで必須なのは Debug / Release の 0 警告ビルド、
   `check_rules.ps1`、そのサブが**新設・変更した selftest 節だけ**の実行、サブ固有の軽い検証 (hash-diff 等)。
   planner は VERDICT で「全体 selftest 未実行」を REWORK の理由にしない。チェックポイントで割れたら、
   その 5 本の範囲で原因サブを特定して差し戻す (M89 の「テストは最後にまとめる」と同じ運用)。

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 86dec43 | 平面拘束 `Rigidbody.plane2D` と一方通行 `Collider.oneWay`、2D 不変量の selftest。依存: なし。**最大の未知 (平面拘束がビット単位で成立するか) を最初に潰す** |
| sub-02 | OK | 1 | 9142d88 | 正射影カメラと Scene View の 2D モード。依存: なし |
| sub-03 | OK | 1 | 03521a8 | スプライト資産 (.meta の sprites / filter / PPU、SpriteLibrary) とソート層。依存: なし |
| sub-04 | OK | 1 | ffcf47a | Sprite2D と Sprite2DPass (Simple / flip / 整数ソート / Point・Linear)。依存: 02, 03 |
| sub-05 | OK | 1 | bd506db | スプライトエディタ窓。依存: 03。**チェックポイント 1 (sub-01〜05) 通過**。Point 表示 / .dds ダブルクリックは sub-06 追補 8 (a)(b) |
| sub-06 | OK | 1 | 484e482 | Sliced / Tiled、SortingGroup2D、SpriteMask2D + sub-05 追補 8 (a)(b)。依存: 04。TypeId 82/83、snapshot v54。Deferred のマスク確認は sub-15 へ |
| sub-07 | OK | 1 | 52c973b | スプライト列クリップ (.spriteanim.json、コントローラの "sprite")。依存: 04。版 bump なし |
| sub-08 | OK | 1 | 4c4c154 | 2D 形状 (kPolygon2D / kEdge2D) と C++ クエリ + Fracture plane2D コピー。依存: 01。版 bump なし。静的形状の供給口 AddStaticShapeSource (sub-11 / sub-19 が使う) |
| sub-09 | OK | 2 | 02e4022 | CharacterController の plane2D (一方通行・移動床・剛体押し・報告)。依存: 01。snapshot v55。round 1 REWORK (must: kinematic + plane2D は velocity で積分) |
| sub-10 | OK | 1 | cc01485 | タイルパレット / タイルマップ資産、Tilemap、チャンク描画。依存: 04。**チェックポイント 2 (sub-06〜10) 通過**。TypeId 84、snapshot v56。NavAgent 庭ハッシュの再採取 (sub-09 起因) は追補 9a49655 |
| sub-11 | OK | 1 | 1eaee36 | タイル衝突 + セル書き換えの sim 状態 + 破壊。依存: 08, 10。snapshot v57。前カリング (CollectWithin / staticOnly) と WakeBodiesNear を spec 4.1.7 に採用 |
| sub-12 | OK | 1 | 3a11cdb | タイルパレット窓。依存: 10, 02。版 bump なし。編集中の反映は TilemapLibrary::Register (TilemapSystem に編集専用の口を作らない) |
| sub-13 | OK | 1 | ec7a3a0 | ルールタイル、アニメーションタイル、レイヤー UI。依存: 11, 12。3b (タイル追加 UI) 実装、3c は計測の結果 dirty のチャンク化を見送り (焼き直し CPU 1.2〜2.8 ms、33 ms 超は WARP のエディタ全体が主因)。版 bump なし |
| sub-14 | OK | 1 | 4fe503f | ABI (10 本) + C# ミラー + MyeScript。依存: 08, 11。**ABI v31 = 188 スロット** (M75h は v32 以降)。空間クエリがタイルの箱を見る (physicsquery::Install) |
| sub-15 | OK | 1 | 971e6a4 | Light2D とライトバッファ。依存: 06。TypeId 85、snapshot v58。チェックポイント 3 通過 (replay 20 ジョブ個別 PASS、golden 30 枚 tol 0、build_managed 成功) |
| sub-16 | OK | 1 | 03ff599 | ShadowCaster2D と法線マップ。依存: 15。TypeId 86、snapshot v59。押し出し距離は「頂点→画面中心 + 対角」に変更 (spec 4.1.9) |
| sub-17 | OK | 1 | 3e54872 | スプライトのサーフェスシェーダ枝、Parallax2D、2D ポスト例。依存: 06。TypeId 87、snapshot v60。スプライトのエントリは新規 hlsli 2 本 (既存サーフェスはバイト同一) |
| sub-18 | OK | 1 | a5004df | Line2D。依存: 06。TypeId 88、snapshot v61。ライトのあるシーンでは常に lit (Q-17) |
| sub-19 | OK | 1 | 15276f6 | SpriteShape2D (描画・衝突・ハンドル)。依存: 08, 18。TypeId 89、snapshot v62。点編集は EditPointList に共通化、縁の厚みはワールド単位 (Q-18) |
| sub-20 | OK | 2 | a2b5d90 | 2D ジョイント検証と Effector2D。依存: 09。TypeId 90、snapshot v63。チェックポイント 4 (sub-16〜20) 通過。round 1 REWORK = 眠った剛体を起こす規則 (spec 2. #21b) |
| sub-21 | OK | 1 | 55ae902 | GridAgent2D、BT MoveTo の fallback、Perception forwardAxis。依存: 11, 09。TypeId 91、snapshot v64。展開上限超えは最良ノードへの部分経路 (spec 4.1.13) |
| sub-22 | OK | 1 | a038819 | Camera2DFollow と PixelPerfectCamera2D。依存: 02, 11。TypeId 92/93、snapshot v65。RT は画面を覆う最小の偶数 (spec 4.1.12) |
| sub-23 | OK | 1 | c1948da | --2d-demo、golden 2d / 2d_lights、replay_verify 2d。依存: 01〜22。チェックポイント 5 (sub-21〜23) 通過。版 bump なし、焼き値 2D デモ 55DEACAD78012785 |
| sub-24 | OK | 1 | 962431b | 文書 (engine_spec / ADR-033 / test_checklists / README)。依存: 23。文書のみ、最終チェックポイントは sub-23 の結果を流用 (受け入れ条件 4) |
| sub-25 | 実装中 | 0 | | レビュー round 1 の修正 (review-1 #1 #2 #3 #4 #5 #7 #9 #10)。依存: 24。チェックポイント 6 (replay_verify 全文は cache\m94\replay_verify_cp6.log) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 4/3/4/3 | major 2 (#1 s1 の比較サンプラで D3D ERROR 196 件、#2 ReleaseGpu の sprite 3 本で M88 復旧打ち切り)、minor 8。planner 宛 #6 #8 #10 は spec で処理、coder 宛は sub-25 に集約 |

## 保留質問 (planner の裁定で通したもの。Notion 活動記録「M94 2D: ハーネスの保留質問」にも写した)
- Q-1 plane2D の z を 0 に強制するか初期値を保つか / 裁定: 初期値 (z0) を保つ。異なる z の plane2D 物同士は当たらない (Unity と違う、注記する) / 逆を選ぶと: sub-01 の D 拘束が pz=0 固定になり、z をずらして配置した物理物が初回 tick で飛ぶ
- Q-2 CC の形状をカプセルのままにするか箱 CC も足すか / 裁定: カプセルのまま (既存の押し出し・段差・坂を流用) / 逆を選ぶと: sub-09 に箱 CC の押し出し経路が 1 本増える
- Q-3 一方通行床の判定を固定 180° 弧 + 前位置チェックにするか、Unity の Surface Arc 可変にするか / 裁定: `Collider.oneWay` bool で固定 / 逆を選ぶと: Collider に float 1 本増え、CC とタイルの判定にも角度が入る
- Q-4 ソート層の上限を 32 (安定 id 0..31、2D ライトの targetLayerMask が uint32) にするか無制限にするか / 裁定: 32 / 逆を選ぶと: Light2D の対象指定が名前リストになり POD に収まらず、sub-15 の設計が変わる
- Q-5 法線マップ付きスプライトを v1 (sub-16) に入れるか後回しにするか / 裁定: 入れる / 逆を選ぶと: sub-16 は影だけになり受け入れ条件 29 の後半が落ちる
- Q-6 グリッド A* を GridAgent2D 新設 + BT MoveTo の fallback にするか、Nav の抽象化にするか / 裁定: 新設 + 2 行の fallback (NavMeshAgent と同じ status 値) / 逆を選ぶと: M82 の NavSystem / BT に抽象層が入り sub-21 が大きくなる
- Q-7 ABI bump の位置を段階 3 末尾 (sub-14) にするか段階 8 にするか / 裁定: sub-14 (段階 3 で横スクロールが作れる = スクリプトからタイルを触れる必要がある。bump は 1 回のまま) / 逆を選ぶと: 段階 1〜3 の成果物がタイルのスクリプト操作抜きになる
- Q-8 タイルパレット窓 / スプライトエディタ窓の Undo を独自 deque (BT 窓の流儀) にし、Play 中は塗れない、でよいか / 裁定: よい / 逆を選ぶと: Play 中の塗りを sim の override として記録する経路 (sub-12) が 1 本増える
- Q-9 (sub-05 VERDICT) check_rules の既存 3 error (`EditorMain.cpp` スプラッシュ表示文字列の `#ifdef _DEBUG` = 規則 1、`tools/engine_dist/Editor.bat` / `Setup.bat` の LF = 規則 14-b) を M94 の中で別コミットとして直してよいか / 裁定: 直したい (replay_verify の `rules` ジョブが毎チェックポイントで赤くなり、本物の失敗と区別しにくい。`#ifdef` は定数で表示だけ分ける、.bat は CRLF へ。どちらも挙動不変) / 逆を選ぶと: M94 の間ずっと `rules` が FAIL のまま、reviewer も毎回除外して読む。**未着手 (司会がユーザーの答えを待ってから別コミットで)**
- Q-10 (sub-05 VERDICT) Project 窓でテクスチャをダブルクリックしたときの既定を「スプライトエディタを開く」に変えてよいか (従来は OS 既定アプリ) / 裁定: 変える。非対応形式 (.dds 等) だけ OS 既定へ (sub-06 追補 8 (b)) / 逆を選ぶと: 窓は Inspector の [Open Sprite Editor] と Window メニューだけから開く (sub-06 (b) が「全部 OS 既定へ戻す」に変わる)
- Q-11 (sub-06 VERDICT) SpriteMask2D の効き方を「描画順で先に出たマスクの range に入るスプライトだけ」(現行 v1) のままにするか、Unity 互換 (描画順に関係なく range 内の全スプライトに効き、マスクが無い VisibleInside は見えない) にするか / 裁定: v1 のまま (マスクの order を小さくすれば同じ使い方ができ、ステンシル 1 bit の制約はどちらでも同じ)。Unity 互換は spec 3. 後回しに記録 / 逆を選ぶと: Sprite2DRenderer の Collect で「マスクを先に全部描く」並べ替えと、マスク 0 個の VisibleInside を落とす分岐が増える (1 サブの追補規模、sub-17 の前に挟める)
- Q-12 (sub-10 VERDICT) タイルの絵を「パレットのスプライトの矩形をセル全面 (cellSize) へ伸ばす」にするか、Unity の原寸 + pivot 配置にするか / 裁定: 伸ばす (標準の 16 px = 1 セルでは同じ結果、サイズ違いの絵がセルからはみ出さない、可逆) / 逆を選ぶと: `BakeTileChunk` のクアッド寸法を `LocalRect` (原寸 + pivot) 基準に変える (sub-13 の rule / anim の前に決めるのが安い)
- Q-13 (sub-12 VERDICT) タイルパレット窓を開いている間、Scene View 2D の左クリックを「塗り」にする既定を ON のままでよいか / 裁定: ON (Unity の Tile Palette と同じ。窓の「Paint in Scene View」チェックで外せる) / 逆を選ぶと: 既定 OFF にして、塗るたびにチェックを入れる操作が 1 つ増える (コードは既定値 1 行)
- Q-14 (sub-13 VERDICT) エディタの Edit モード (Play していない) でアニメーションタイルが止まって見える (コマ 0 のまま) のでよいか / 裁定: 止まったままでよい (アニメは sim tick から導出、描画専用の時計を足すと決定性の境界が 1 つ増える。Play 中と Runtime では動く) / 逆を選ぶと: Sprite2DRenderer に「Edit 中だけ実時間から仮の tick を作る」描画専用の時計を足す (小規模。sim には触れない。スクショ / golden は tick 基準のまま)
- Q-15 (sub-15 VERDICT) どのライトにも照らされない層の lit スプライトを黒にする (Unity と同じ) でよいか / 裁定: 黒 (Global ライトを置けば底上げできる。ライトを入れたら暗所は暗く見えるのが 2D ライトの意図) / 逆を選ぶと: 集合が空の層だけ従来色に戻す (`SwitchLightGroup` の 1 分岐。ただし「ライトの当たらない層だけ明るい」逆転が起きる)
- Q-16 (sub-16 VERDICT) 法線マップ付きスプライトが 1 枚でもあるシーンで、法線マップを持たないスプライトにも N·L (ライトの真下以外が少し暗くなる) を掛ける (Unity の Normal Maps 有効時と同じ) でよいか / 裁定: 掛ける (手前と奥で照らされ方が揃い、法線マップの有無で浮いて見えない) / 逆を選ぶと: 法線 RT のアルファで「法線あり」を区別し、無しのスプライトは N·L を 1 にする (clear 値とシェーダの小変更)
- Q-17 (sub-18 VERDICT) Line2D を「ライトのあるシーンでは常に照らされる」(lit の切り替え欄なし) でよいか / 裁定: 常に lit (Tilemap と同じ。夜のロープは暗く見えるのが自然) / 逆を選ぶと: Line2D に `lit` (bool、既定 true) を末尾に足す (NoHash なので sim には影響しない。Sprite2D の lit と同じ扱い、小規模)
- Q-18 (sub-19 VERDICT) SpriteShape2D の縁の帯の厚み (`height`) をエンティティのスケールに追従させるか / 裁定: 追従させない (ワールド単位。スケールで丘を大きくしても縁の太さが変わらず、ピクセルアートの縁が太らない) / 逆を選ぶと: `EmitShape` で height にワールドスケール (y 方向) を掛ける 1 箇所の変更 (Unity の Sprite Shape は Transform に追従する)
- Q-19 (review round 1 #6) 一方通行床 (oneWay) を v1 では水平な床だけとし、傾いた一方通行床は対象外にしてよいか / 裁定: 水平だけ (「上」= ワールド +Y・天面 = AABB の y 最大で実装済み、テストも水平のみ。限界欄に明記) / 逆を選ぶと: 判定を床のローカル +Y と形状の上面に切り替える修正と傾いた床の selftest が sub-25 に 1 項目増える (中規模、決定性への影響なし) / ユーザーの方針 (推奨案で通し Notion に記録) により裁定どおり採用 (2026-10-22)

## 申し送り (セッション跨ぎ)
- 基点の直前に無関係の 2 コミット (`2ac3b4f` DeepModal 再学習、`25fb355` スプラッシュ) を司会が切り出した。
- (sub-01 VERDICT) **平面拘束は A (慣性 zz 限定) + B (速度ソルバ直後) + D (積分直後) の 3 点で成立**。R1 の追加拘束は不要だった。
  焼いたハッシュ 639107476EA401A3 (Debug == Release)。ADR-033 (sub-24) の材料。
- (sub-01 VERDICT) `check_rules.ps1` の既存 error 3 件は **M94 の範囲外の別件**: `EditorMain.cpp:741` の `#ifdef _DEBUG` (規則 1、
  `25fb355` スプラッシュの残り) と `tools/engine_dist/Editor.bat` / `Setup.bat` が LF (規則 14-b、`bfc48ff` M92 配布物の残り)。
  以降の VERDICT では「check_rules 0」を「M94 の変更で新しい error / warning を増やさない」と読む。
- (sub-01 coder) ビルド癖: 変更後の初回ビルドが `SelfTestUnused.kernel.hlsl` の C4100 で落ち、同じコマンドの再実行で通る。
  着手前の HEAD でも再現。M94 の範囲外。
- (sub-01 coder) `MYE_SELFTEST_ONLY=m94` の分岐は `EditorMain.cpp` にある。以降のサブは自分の suite をそこへ足す。
- (sub-01 coder → sub-08) `FractureSystem.cpp:188` は破片へ `freezeRotation` をコピーするが `plane2D` をコピーしない。sub-08 やること 6。
- (sub-01 coder) 親付き (frame が identity でない) plane2D は未検証 (ルート剛体のみ検証)。複合コライダーの子形状の oneWay は v1 未対応 (spec R9)。
- (sub-02 VERDICT) 正射影 + froxel の描画は未確認 → sub-23 やること 7 (`--2d-demo --froxel --deferred` を 1 枚、spec R11)。
  SceneView 2D の入力 (ホイール / パン / F) の実操作はユーザーの目視 (test_checklists の M94 節、sub-24)。
- (sub-02 coder) Camera.orthographic / orthoSize が使える。SceneView の 2D 状態は `SceneViewWindow::mode2d`、`orthoSize2d_`、
  `lastView_` / `lastProj_` (ピッキング・ギズモ共通)。sub-12 の塗りは `MouseRay` と z=0 平面の交点でセルを取れる。
- (sub-02 coder) エディタのスクショ撮影は、リポジトリ直下の `imgui.ini` (Git 管理外) が窓位置を覚えているため、一時的に退避して撮り、元に戻す。
- (sub-03 coder) 撮影手順: Editor.exe は `--frames N --shot-frame M --screenshot <png>` の組で、`--shot-frame` を付けないと保存されない。
- (sub-03 VERDICT) 契約: `SpriteDef` は rect w/h = 0 (`IsFullTexture()`) が全面で描画側が Texture から寸法を引く / `filter` / `pixelsPerUnit` も
  既定値なら .meta に書かない / `OrderOf` の無い id は Default の配列位置 / `.meta` 単体変更は Apply (スプライトエディタ + Inspector Import
  Settings) が `SpriteLibrary::Register` を直接呼ぶ (sub-05) / `SortingLayers` の起動 Load は EngineLoop 初期化で sub-04 が接続。
- (sub-03 VERDICT) `.meta` の重複 id の「後勝ちで WARN」(spec R6) は未確認 → sub-05 やること 4。GUI の Up / Down / Remove の実クリックはユーザー目視 (sub-24)。
- (sub-04 VERDICT) **2D の色の契約 (spec 4.1.3)**: テクスチャは `.meta` srgb = auto (UNORM ロード)、2D カメラは `CameraPostFx.tonemapMode = 0`
  (Passthrough) で元画素が ±1 で出る。ACES 既定では白が 231 になる。Light2D の乗算はガンマ符号化値に掛かる (Unity の Gamma 色空間と同じ割り切り)。
  srgb = on のテクスチャは Inspector で警告 (sub-06)。`--2d-demo` のカメラは tonemapMode 0 (sub-23)。
- (sub-04 coder) Sprite2DRenderer の `Item.groupKey` は 0 のまま (sub-06 で SortingGroup2D)。`sprite2d.hlsl` の t1 / s1 は空き (sub-15)。
  Inspector / ギズモ / Create メニューの GUI 部は実操作未確認 → sub-05 の受け入れ条件 2 で通す。
- (sub-04 VERDICT) Renderer 層との受け渡しは `Renderer/Passes/Sprite2DDrawList.h` の POD。`TextureLibrary::IsPending` で非同期ロード中は描かない。
- (sub-05 coder) `SpriteEditModel` の BeginGesture / EndGesture と deque Undo の形は sub-07 (アニメプレビュー) / sub-12 (タイルパレット窓) で流用できる。`SpriteEditModel::Save` は `TextureLibrary` を触らない (窓が `ReplaceFromFile` を呼ぶ)。窓以外から保存するなら同じ組を呼ぶ。
- (sub-05 VERDICT) チェックポイント 1 通過。`demo_terrain_deferred` だけ golden と maxDiff=5 / 2 画素ずれるが基点 `25fb355` でも同じ (WARP の版差、M94 の外の別件。録り直すなら golden-diff-triage の 4 点計測の後)。以後この 1 枚は「基点の絵と maxDiff=0」で見る (spec 5. 条件 4)。次のチェックポイント (sub-10) からは replay_verify の全文ログを `cache\m94\replay_verify_cpN.log` に残す (sub-10 検証コマンドに追記済み)。
- (sub-05 VERDICT nit) `DrawCanvas` が入力と描画を 1 関数に持っていて長い → sub-12 の窓では分ける。窓の実操作一式 (ズーム / パン / ドラッグ / Delete / Ctrl+S / 未保存モーダル / Inspector のボタン / Window メニュー / ゲート表示) と Inspector のスプライトピッカーはユーザー目視 (sub-24 checklists)。
- (sub-06 coder) `sprite2d.hlsl` の PS は b1 (マスクのクリップ) を使用済み。t1 / s1 は空きのまま (sub-15)。`Sprite2DBatch` に stencil / alphaCutoff が増えたので、Light2D のバッチ分割は `Sprite2DRenderer::Push` の比較に足す。Pass の DSV 差し替えは UsesStencil (マスクがあるとき) だけ。
- (sub-06 coder) `Sprite2DRenderer` の Item キーは外側 / 内側の 6 整数 + isMask。タイルマップのチャンク (sub-10) を同じ整列に混ぜるなら Item に種別を足す。そのとき Collect / EmitSprite を種別ごとの小関数に割る余地あり (planner nit)。
- (sub-06 coder) スプライトエディタの Point 表示は `ImGuiDrawListSetPointSampling(dl, true/false)` で AddImage を挟む (Renderer/ImGui 層の関数。他の ImGui 画像にも使える)。一時シーンの 64 px/unit は orthoSize = 540 / 2 / 64 = 4.21875、画素は px = 480 + 64x, 270 - 64y (sub-23 のデモの座標確認に流用可)。
- (sub-06 VERDICT) Deferred 経路のマスク (書き込み DSV と深度 SRV の同時束縛) は未確認 → sub-15 やること 5 の temp シーンに SpriteMask2D を 1 個入れて Forward / Deferred で同じ形を見る。Inspector の srgb 警告行と Add Component / Create の実クリックはユーザー目視 (sub-24)。
- (sub-07 coder) SpriteAnim のコマ選びは `SpriteAnimFrameAt` 1 本 (sub-13 のアニメタイルで流用可)。Sprite2D への書き込みは `AnimatorController.cpp` の `WriteSpriteFrame` 1 箇所。`MYE_SELFTEST_ONLY=m94` は Physics / UI / Sprite2DAsset / SpriteEditor / AnimatorController / AnimatorControllerEdit / AssetDatabase / ReloadHub / AssetOps。hash-diff の `UIInteraction.prevSurfX/Y` は実マウス座標のミラー (入力) なので run 間で違って当然、replay は割れない (planner 確認)。
- (sub-07 → sub-24) engine_spec に書く: スクリプトが Sprite2D.sprite を書いても次 tick で Animator が書き戻す。書き込み先は自身 → 前順で最初の子孫 (別の Animator の部分木は除く)。遷移中のコマは transitionTick*2 >= duration で先に切り替わる。イベントのキーは .spriteanim.json の name。GUI の実クリックはユーザー目視、Play 中の切替は sub-23 の --2d-demo で通る。
- 2026-10-20 ユーザー指示で一時停止 (sub-07 OK の直後) → 同日「続き」で再開し sub-08 から続行。sub-10 がチェックポイント 2 (sub-06〜10): replay_verify の全文ログを `cache\m94\replay_verify_cp2.log` に残す件を prompt に添える。
- (sub-08 coder → sub-11) 静的形状の供給口は `PhysicsSystem::AddStaticShapeSource` / `RemoveStaticShapeSource` + `StaticShapeSource::Collect(World&, std::vector<StaticShape>&)` (`StaticShape{owner, pose, col}`、`Physics\Rigid\StaticShapeSource.h`)。同じ持ち主の形状は並べて返す (bodies は stable_sort で収集順を保つ)。姿勢は呼び出し側がワールドで確定して渡す。`col` は tick の間変わらない ColliderComponent を指す (oneWay / 材料 / レイヤーの出所)。所有は呼び出し側。
- (sub-08 coder → sub-14) `OverlapBoxWorld` は既にマスク付き (新 overload 無し)。`BoxCastWorld` の C++ 引数順は `(world, center, half, dir, maxDist, outHit, mask, skip, skipUser)` (RaycastWorld 流。spec 当初の順は却下)。方式は箱の 27 点 Raycast の近似で「突端が撃つ点の隙間に入ると取りこぼす」→ ABI の docstring と engine_spec の限界欄 (sub-24) に書く。
- (sub-08 coder → sub-19) `shapes::ExpandEdge2D` は `Shape2D` 資産の点列から線分ごとの箱 (半厚 edgeRadius、両端を edgeRadius 延長、z 半厚 depth/2) を作る。SpriteShape の辺は同じ式で箱を作るか `StaticShapeSource` で返す。`ShapeLibrary2D` の項目は Register で同じ map 要素へ書き換わる (ポインタは有効なまま中身が変わる)。tick の途中で Register しないこと。
- (sub-08 VERDICT) spec 7. R9 (d): 複合コライダーの子の kPolygon2D は v1 スープのまま (動的剛体の一部としては未対応)、親付き (frame が identity でない) の多角形 / 辺は未検証 → sub-24 で engine_spec の限界欄へ。kinematic の多角形はスープ、動的 / kinematic の辺は WARN して無効。`MYE_SELFTEST_ONLY=m94` に Fracture suite を追加 (plane2D 破片コピーの 1 項目)。
- (sub-08 VERDICT nit) `HandleShape2DEdit` が点の描画・判定・ドラッグ・追加・削除を 1 関数に持つ → sub-19 の SpriteShape ハンドルで共通化の余地。点のドラッグ / 辺クリック追加 / Delete の実クリックはユーザー目視 (sub-24 checklists)。点編集は Undo 無し (ドラッグ 1 回 = 保存 1 回)。
- (sub-09 VERDICT) **plane2D かつ kinematic の剛体は物理のサブステップで velocity / angularVelocity.z を積分する** (`Body.kinDrive`、spec 4.1.6)。plane2D OFF の kinematic は従来どおり積分しない。移動床は kinematic Rigidbody に velocity を書くだけで動き、CC が乗る (Transform を別に動かさない)。sub-23 のデモの移動床はこの流儀で。
- (sub-09 coder) CC の 2D 報告欄: `wallContact` は接触中に立つ (押し付け不要)、`groundVelocity` は SolveCharacters 時点の床の velocity (kinematic は tick 開始時、dynamic は解決後) で y も変位に足す、乗っている間の `cc->velocity.x` は床に対する速度。剛体押しは |nx| > 0.5 かつ進行方向、`pushPower × moveInput.x`、1 tick 1 剛体 1 回・最大 8 体、押した剛体を起こす。sub-20 (Effector2D) / sub-21 (GridAgent2D) は `groundEntity` と `ticksSinceGrounded` を読める。
- (sub-09 coder → sub-11) タイル箱は `StaticShape` 経由で bodies に入るので CC の一方通行 / 接地プローブ / `groundEntity` (owner = Tilemap エンティティ) にそのまま効くはずだが、タイルでは未確認 → sub-11 の selftest で CC を載せて確認する。
- (sub-09 VERDICT nit → sub-24) spec 7. R9 (e): 回転する kinematic 床に乗る CC は運ばれない (groundVelocity は並進のみ)、親付き kinematic plane2D / 親付き plane2D CC の z 不変は未検証、`wallContact = 2` と複数剛体の押しは selftest に無い (sub-23 のデモで通る見込み)。`SolveCharacters` の報告部を小関数に割る余地。`cache\m94\s09\` の hash-diff ダンプは git 管理外のまま。
- (sub-09 coder) 焼いたハッシュ: CC2D C04B7FFA3E31E5D0 (Debug == Release)。既存の plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 は不変。
- (sub-10 VERDICT) **チェックポイント 2 (sub-06〜10) 通過**: 全体 selftest 両構成 exit 0、replay_verify 20 ジョブ全 PASS (全文 `cache\m94\replay_verify_cp2.log`、rules だけ既存 3 error で赤)、shot_verify 30 枚 PASS、golden `--tol 0` 29 枚 0 + `demo_terrain_deferred` は基点の絵 (`cache\m94\baseline_terrain.png`) と 0。割れたのは NavAgentSelfTest の焼いた庭ハッシュ 1 件 (sub-09 の CC 7 欄 append が原因、挙動は不変) → 追補 `9a49655` で再採取。
- (sub-10 VERDICT should 2 → sub-21 ほか append するサブ) **spec 4.4 の規則: hash 対象フィールドを既存型に append するサブは、焼いた WorldHash 定数 (`grep -rn "kExpected" src --include=*SelfTest.cpp`) を同じサブで再採取する。** sub-21 (Perception `forwardAxis`) の依頼文に添える。
- (sub-10 coder) `cache\m94\base25fb355` (基点 `25fb355` の git archive + Release Runtime、約 0.5 GB、git 管理外) と `cache\m94\baseline_terrain.png` を残してある。次のチェックポイント (sub-15) の `demo_terrain_deferred` 比較に流用できる。
- (sub-10 coder → sub-11) Tilemap の衝突は `TilemapSystem::GetChunkCells / EnumerateChunks` にアセットの値が出る。override を足すときは GetCell / GetChunkCells で重ね、EnumerateChunks に override だけのチャンクも含め、書き換え / 復元で `MarkAllDirty` を呼ぶ (描画側は `DirtyEpoch` の変化で全チャンクを焼き直す)。showCollision の描画は `DrawTilemapGizmos` に足す (緑)。静的形状は `PhysicsSystem::AddStaticShapeSource` (sub-08 の供給口)。`TilemapLibrary` は Register のたびに Epoch が増える。
- (sub-10 coder → sub-12) 窓から塗るときは `TilemapLayer::SetCell` (空になったチャンクは消える) と `TilemapLibrary::ToJson / Register`。Register で Epoch が増え、描画のキャッシュは自動で焼き直る。SceneView の 2D モードのセル格子は選択中の Tilemap だけ、1 方向 256 本を超える密度では引かない。sub-12 やること 7 (planner 追記): `SpriteLibrary::Epoch()` を足し、チャンクキャッシュのキーに (Tilemap / TilePalette / SpriteLibrary の epoch) を含める (スプライトエディタ Apply 後に古い UV が残らないように)。
- (sub-10 coder → sub-13) パレットの rule / anim は読み書きのみ。評価は `BakeTileChunk` の「def->sprite を引く」1 箇所に差し込む (隣接は実効セル、アニメは tick から。アニメを含むチャンクの焼き直しは dirtyEpoch か専用キーで)。コマ選びは sub-07 の `SpriteAnimFrameAt`。
- (sub-10 coder → sub-14) C++ 側のセル座標 API は `TilemapSystem` の静的関数。`CellToWorld` はセル中心 (左下角が要るなら足す)。
- (sub-10 VERDICT) 描画の確定 (spec 4.1.7): 絵はセル全面へ伸ばす (Q-12)、flip → 回転 (反時計回り 90°) の UV 隅入れ替え、行 0 が下、チャンク (cy, cx) 昇順、正射影だけチャンク切り、タイブレーク entity.index → レイヤー → チャンク。性能: 15000 セル全面で GPU 約 9 ms (WARP、空 2.5 ms)、CPU 増分 約 1.1 ms (毎フレームの原点加算コピー、nit)。Sprite2D 2000 枚との合算は sub-23。Scene View のギズモ / Create / ピッカー / D&D の実画面は sub-12 のエディタスクショで通す。
- (sub-11 VERDICT) **sub-11 は Broadphase (`staticOnly`) と PhysicsSystem (`MotionRegions` の組み立て) を触った。** チェックポイント 3 (sub-15) の replay_verify は全ジョブの個別 PASS 行をログで確認する (`cache\m94\replay_verify_cp3.log`)。Phase 3 の reviewer 依頼文に「PhysicsSelfTest の既存節と全 replay ジョブの照合を重めに」と明記する。
- (sub-11 VERDICT should 1 → sub-14 やること 6) PhysicsQueries (Raycast / OverlapSphere / OverlapBox / SphereCast / BoxCast) はタイルの箱を見ない。sub-14 で供給口の `Collect` 全体 (絞り込み無し) をクエリのコライダー収集に足す + selftest (spec 7. R12 (b))。
- (sub-11 VERDICT should 2 → sub-21 やること 4) Perception の LOS (`CollectLosColliders`) も同じ口でタイルを見せる。sub-21 の依頼文には append 再採取規則 (spec 4.4) も添える。
- (sub-11 VERDICT nit → sub-12) Scene View のエディタスクショに showCollision の緑の箱 (タイル衝突) も写す。nit → sub-24: 絞り込みの余裕 (速度 2 tick + 2 m) を超える探りはタイルを見ない (spec R12 (a))、チャンク境界 (16 セル) で箱が分かれる (R12 (c))、を engine_spec の限界欄へ。
- (sub-11 coder) Play 中の sim の書き換えは `TilemapSystem::SetCell / FillRect / DamageCell` (World と持ち主 EntityID を取る)、`tilemap::System()` が Editor からも引ける。アセット編集 (sub-12 のパレット窓) は `TilemapLayer::SetCell` + `TilemapLibrary::Register`。セル座標は -32768..32767、範囲外の書き込みは false。cellKey は各軸 +32768 の 16 bit 詰め (cy が上位)。
- (sub-11 coder → sub-13) `BakeTileChunk` は `tilemap::System()` の実効セルを読む。damage ビット (bit24-31) は実効セルに入っているので、ひび割れの見た目は評価時に `Damage(value)` を見る。
- (sub-11 coder → sub-14) TilemapSetCell / GetCell / DamageCell は TilemapSystem のインスタンス (EngineApiTable の文脈に `TilemapSystem*` を足す) に 1 対 1。座標変換は `CellToWorld / WorldToCell` (WorldMatrix 基準、1 tick 古い)。
- (sub-11 coder → sub-21) GridAgent2D の歩行可否は `TilemapSystem::GetCell` と palette の collider / oneWay で引く。→ sub-22 / 23: タイルの箱は動く物体の範囲に届くものだけ物理に渡る (動かさない面だけの Tilemap でも近づいた tick に箱が現れる)。
- (sub-11 coder) 焼いた値: タイル並走シナリオ 326BA3CD14F4BBB8 (`TilemapSelfTest.cpp` の `kTileScenarioExpectedHash`)。既存の plane2D / Shapes2D / CC2D / NavAgent 庭は不動。`MYE_SELFTEST_ONLY=m94` に Tilemap suite が入っている。通常の静的 Collider を消しても眠っている剛体が起きない既存挙動は別件のまま。`cache\m94\s11\` に着手前後のダンプ・.rep (git 管理外)。
- (sub-12 VERDICT should → sub-13) やること 3b: パレットのタイル追加 UI (スプライトから追加・スライス済みテクスチャから一括・削除、`TilePaletteEditModel` の独自 Undo)。やること 3c: 100×50×3 層で Brush を 60 フレームドラッグしたフレーム時間を計測し、33 ms を超えたら描画キャッシュの dirty をチャンク単位へ。
- (sub-12 VERDICT nit → sub-24 checklists) 実マウスのクリック判定、S/B/D/G/U/I・Ctrl+S/Z/Y、Delete 先取り、別アセット切替モーダル、Open Tile Palette ボタン、Window メニュー、TextWrapped 化後の Play 中表示はユーザー目視。Revert ボタンは無し (Undo で戻せる)。
- (sub-12 coder → sub-13) レイヤー編集で `TilemapEditModel` を拡張するなら `LayersEqual` の size 比較と `CellEdit.layer` 添字に注意。rule / anim が入っても窓の「編集中は Register で反映」はそのまま使える。キー: S B D G U I は窓フォーカスか Scene View 上カーソル時、Ctrl+S/Z/Y は窓フォーカス時のみ (Scene View フォーカスの Ctrl+Z はシーンの UndoStack)。Scene View 2D の色が淡いのはエディタカメラの ACES (色の契約はゲームカメラの話、spec 4.1.7 に明記)。
- **2026-10-21 ユーザー指示「40分後にいったん止めて」で sub-13 の判定前に一時停止。** 再開手順: (1) `git status` で sub-13 の変更 (SELF_EVAL の「触ったファイル」と一致、`plans\m94-2d\sub-13.md` の実装メモに SELF_EVAL round 1 の写し) が残っていることを確認 (2) planner を新規起動し (セッションを跨ぐと SendMessage 不可)、`MODE: VERDICT` で sub-13.md 実装メモの SELF_EVAL 全文を渡す。coder の不安・質問 5 件 (3c の 33 ms 判定は WARP 基準でも超えるので見送り / ドラッグ初回だけ約 110 ms / Ctrl+Z はマップとパレット別履歴 / Edit 中アニメタイルは止まる / レイヤー UI は Inspector でなく窓) を裁定させる (3) OK ならコミット (件名は VERDICT から) → 台帳 → sub-14 (ABI、M75h が v31 予約なので末尾確認、やること 6 = PhysicsQueries にタイルの箱)。REWORK なら新しい coder に FIX_REQUEST + sub-13.md を渡す。
- 2026-10-21 ユーザーの「続き」で再開。planner (同じセッションの agent) は SendMessage で再開でき、sub-13 を判定した。
- (sub-13 VERDICT nit → sub-23) デモ資産の `.meta` は spec 4.1.14 どおり `filter = 2 (point)` にする (`tools\gen_2d_assets.py`)。sub-13 の temp 資産は filter auto で滲んでいた。
- (sub-13 VERDICT nit → sub-24 checklists) パレット窓の初回ドラッグが一瞬 (約 110 ms) 止まらないか、GUI の実クリック (追加ポップアップ、8 近傍トグル、InputText、Ctrl+Z の宛先、パレットの Ctrl+S 往復)。Inspector のタイル資産要約から窓を開く導線は無し (Tilemap コンポーネント側の [Open Tile Palette] で足りる)。
- (sub-13 coder) ルール / アニメの評価は描画側だけ (TilemapSystem / ABI に足していない)。スクリプトの SetCell で変わったセルのルール変種は自動で追従。sub-15: Sprite2DRenderer の Collect に simTick 引数が増えた (呼び元は RenderSystem.cpp の 1 箇所)。sub-24 に書くこと: 評価順 anim > rule > sprite、neighbors の順 (NW N NE W E SW S SE、N = +y)、Edit モードではアニメが進まない (Q-14)、Play 中は編集不可、Ctrl+Z はマップ / パレット別で直前に変わった側から。
- (sub-14 着手時の確認) `MYE_API_VERSION` は 30。M75h は未着手なので M94 の sub-14 が v31 を使い、M75h は v32 以降になる (メモリの M75h 予約を更新すること)。
- (sub-14 VERDICT should 1 → sub-15) チェックポイント 3 で `tools\build_managed.bat Release` を 1 回回す (sub-14 は Debug だけ)。
- (sub-14 VERDICT → sub-15 の依頼文に添える) sub-11 (Broadphase / 動く物の範囲) と sub-14 (PhysicsQueries の供給口) を重点に、replay_verify 全ジョブの個別 PASS 行を確認し全文ログを `cache\m94\replay_verify_cp3.log` に残す。sub-15 やること 5 の Deferred でのマスク確認。
- (sub-14 VERDICT nit → sub-23 やること 8) 供給口の Collect を毎クエリ絞り込み無しで呼ぶコストを計測し、1 tick の sim が 2 ms を超えたら tick 単位でキャッシュ。C# の SpriteByName は実在スライスをデモスクリプトで引く。→ sub-22 やること 0: Camera2DFollow の boundsFromTilemap 用に `GetCollisionBounds` (ABI には出さない)。
- (sub-14 coder) Raycast の既存消費者 (AcousticField / FootIk / Perception) もタイルの箱に当たる (意図どおり、spec 4.1.10)。sub-21 の Perception LOS は `physicsquery::CollectStaticShapes(world, mask, out)` で同じ口を使える。sub-24 の限界欄: BoxCast 27 点近似、クエリは絞り込み無しの全箱、GetBounds の定義、CellToWorld は out ポインタ。ビルド癖: C4100 の既存癖は非 WX ビルドを 1 回挟んでから WX を 2 回重ねると通る。`plans\m75-ugui.md` の M75h 版注記は sub-24 で更新。
- **2026-10-21 ユーザー指示「sub14が終わったらいったん止めて」で sub-14 コミット後に一時停止。** 作業ツリーは clean。再開は `/harness resume m94-2d` (同じセッションなら planner は SendMessage で再開可、跨ぐなら新規起動して spec / 台帳を渡す) → sub-15 (チェックポイント 3) の coder を起動。依頼文に上の 3 行 (build_managed Release / replay の個別 PASS 行と全文ログ / Deferred のマスク) と、`demo_terrain_deferred` は基点の絵 `cache\m94\baseline_terrain.png` と比較することを添える。
- (sub-15 VERDICT) チェックポイント 3 通過。全文ログ `cache\m94\replay_verify_cp3.log`。`cache\m94\s15\base_head` (基点 044b508 の git archive、約 0.5 GB、git 管理外) は次のチェックポイントの基点比較に流用できる。
- (sub-15 VERDICT should 1 → sub-16 やること 5b) 5 点の凸 Freeform と丸テクスチャの Sprite ライトを置き、形の内側が明るく外側が Global のみであることを画素ログで確認する。nit 3 → sub-16: 影 RT / 法線 RT を入れるとき `Sprite2DPass::Render` のライトバッファ構築部を関数に割る余地。
- (sub-15 VERDICT nit 2 → sub-24 checklists) Light2D のギズモ、Create → 2D → Light 2D、Inspector の実操作、非 Play の Scene View でライトが効く見た目はユーザー目視。
- (sub-15 coder → sub-16) ライトの形状 PS は `sprite2d_light.hlsl` の LightCB (b0)。shadows / shadowStrength / normalMapDistance は Light2DComponent に既にある (未使用)。影 RT は群ごとの BuildLightBuffer 内、ライト 1 個ずつの描画の前に差し込める。法線 RT は BuildLightBuffer の前に作り、sprite2d_light.hlsl に t を足す。
- (sub-15 coder → sub-17 以降) Sprite2DBatch を増やすときは `Sprite2DRenderer::Push` の比較に足す (lit と forceNewBatch_ はここ)。→ sub-23: golden 2d_lights はこのライト経路、ライトのあるシーンでは Tilemap も常に lit。
- (sub-15 coder → sub-24 限界欄) 集合が空の層は黒 (Q-15)、凹 Freeform の外縁の重なりは加算で明るくなる、透視カメラではライト平面がずれる、ライトバッファは層集合が変わるたびに作り直し (R8、層 32 × 別集合で 32 回)、Spot の falloff は半径方向の指数で Unity の Falloff とは別。
- (sub-16 VERDICT nit 1 → sub-17 やること 4b) normalMap の sRGB 警告行を Inspector に (sub-06 の sprite 欄と同じ形)。nit 2 → sub-17: マテリアル別バッチを入れるとき `Sprite2DPass::Render` のバッチ描画ループを関数に割る余地。nit 4: ECS の AddComponent でカラムが動くと既存のコンポーネントポインタが無効になる (sub-16 の selftest で踏んだ)。後続サブの selftest でも注意。
- (sub-16 VERDICT nit 3 → sub-24 checklists) ShadowCaster2D の紫のギズモ、Inspector の normalMap ピッカー (ドロップ / 一覧がテクスチャだけ)、Add Component の 2D 分類はユーザー目視。ShadowCaster2D の Create メニューは無し (Add Component で足りる)。
- (sub-16 coder → sub-17 以降) Sprite2DBatch を増やすときは `Sprite2DRenderer::Push` の比較に足す。法線バッチは別系統 (PushNormal、Sprite2DNormalBatch)。Sprite2DPass は VB の前半がスプライト頂点、後半が法線頂点。→ sub-23: golden 2d_lights (影と法線入り) は sub-16 の temp シーンの座標・色を流用できる (`cache\m94\s16\` と screenshots の 4 枚)。
- (sub-16 coder → sub-24 限界欄) 影は硬い影のみ、スプライトのシルエット影は無し、法線 RT はステンシルマスク対象外、法線は sRGB オフ・緑 = 上、影 RT はライトごとに作り直し、透視カメラでは影もずれる、caster の Collider 回転・中心は Box / Sphere だけ反映し Polygon2D はローカル XY のみ、法線 RT が有効なら法線マップ無しのスプライトにも N·L (Q-16)。
- (sub-17 VERDICT nit → sub-23) 透視カメラでの Parallax、回転・スケール付きの親を持つスプライトの視差、大量バッチ混在は実描画で未確認 (2D は正射影前提で v1 は可)。sub-23 のデモ (視差背景 ×2) で正射影の実運用を通す。m94 suite に Surface 3 本 (Shader / Material / Deferred) が入り所要時間が増えた (メッシュ用サーフェス経路の不変を毎サブ見るため維持)。
- (sub-17 coder → sub-20 以降) `Sprite2DPass::Render` に MaterialLibrary 引数 (呼び元は RenderSystem.cpp の 1 箇所)。Sprite2DBatch に material が増え Push の比較に入っている。`cache\m94\s15\base_head` は基点 edb87ac を Release ビルドした状態に更新済み (約 0.5 GB、git 管理外)。
- (sub-17 coder → sub-24 engine_spec) スプライトのサーフェスは MyEngineSurfaceSprite.hlsli を include して `float4 PSMain(MyeSpriteInput i)` (VS・ライト乗算・ブレンドはエンジン)、メッシュ用と併記可、判定は include 名。使えない材質は内蔵 + Inspector に理由、走行中に壊すと旧プログラムが残る、輪郭線は矩形の内側だけ、材質は色パスだけで法線 RT / 影 RT に効かない、Properties 名は `_` 始まり。Parallax2D の repeat は正射影だけ (片軸 64 / 両軸 8x8)、factor は描画位置だけ。
- (sub-18 VERDICT should 1 → sub-19 やること 5b) `HandleLine2DEdit` と `HandleShape2DEdit` が同じ 2 本の lambda (toScreen / mouseLocal) と点ハンドル処理を重複して持つ。SpriteShape を足す前に共通関数へ寄せ、SpriteShape もそれを使う (挙動は変えない)。
- (sub-18 VERDICT nit → sub-24) 点ハンドル (選択・ドラッグ・辺クリック追加・Delete・Esc) と Create → 2D → Line の実クリックはユーザー目視 (checklists)。限界欄: Scene View のクリック選択に Line2D は載らない (Hierarchy か点ハンドル)、最大 32 点、曲がり 120° 超はベベルで半透明の帯は内側で二重に乗る、端は平ら (丸キャップ / 破線 / テクスチャ繰り返し無し)、帯の UV は u = 全長 0..1 / v = 左右、塗りはローカル / 1 単位、サンプラ clamp、ライトのあるシーンでは常に lit。
- (sub-18 coder → sub-19 / 23) 三角形分割は `line2d::Triangulate` (= TriangulatePolygon2D)。`HandleLine2DEdit` が「コンポーネントのフィールドへ直接書く + UndoStack」版の手本 (HandleShape2DEdit はアセットへ書く版)。sub-23 のロープは Line2D (loop なし、幅 0.1 前後)。`TextureLibrary::White()` を使うので device の無い selftest で Collect するなら Init が要る。img-diff は Editor.exe で (Runtime.exe だとハング)。`cache\m94\s15\base_head` は基点 f694013 の Release ビルドに更新済み。
- (sub-19 VERDICT 裁定、spec 4.1.11) 点が変わっても眠っている剛体は起こさない (Tilemap の SetCell は明示 API なので起こす)。開いた線は「左から右が表」で Flip は作らず点の並びを逆にする運用。
- (sub-19 VERDICT nit → sub-23 / 24) 描画は目視 + Collect の頂点数までで画素の解析照合なし → sub-23 の golden `2d` に丘を入れて固定。点のドラッグ / 辺クリック追加 / Delete / Esc / Create → 2D → Sprite Shape の実クリック、透視カメラ・回転 / スケール付きの親の見た目はユーザー目視 (checklists)。`EmitShape` が長め、`SpriteShape.h` が json.hpp を `Sprite2DRenderer.h` 経由で配る (Shape2DLibrary と同じ形)。
- (sub-19 coder → sub-20 チェックポイント 4) sub-19 は PhysicsSystem (`ComposeEntityWorldScale`) と Shapes (`MakeSegmentBox2D` の抜き出し) を触った。replay_verify は全ジョブの個別 PASS 行を確認し全文ログを `cache\m94\replay_verify_cp4.log` に残す。`tools\build_managed.bat Release` も回す。`cache\m94\s15\base_head` は基点 26aa0fa の Release ビルドに更新済み。
- (sub-19 coder → sub-21 / 23 / 24) sub-21: physicsquery::CollectStaticShapes で SpriteShape の箱も返る (LOS も同じ口)。sub-23: 丘は SpriteShape2D (閉じた smooth、profile = 縁 3 + fill)、生成スクリプトは `cache\m94\s19\make_hill_scene.py` (git 管理外)、縁 32x16 / ppu 32 / height 0.5。sub-24 限界欄: 縁の角度は外向き法線 / 開いた線は左から右が表 / height はワールド単位 / 頂点 z はエンティティ原点 / 角スプライト・開いた形状の塗り無し / 衝突の箱は半厚 0.1 / 眠りは起こさない / クアッド上限 682・塗り 4096 / 塗りの格子は原点基準。
- (sub-20 チェックポイント 4、round 1) 全体 selftest Debug / Release FAIL 0 (PASS 8648)、replay_verify 20 ジョブ個別 PASS (rules は既存 3 error、全文 `cache\m94\replay_verify_cp4.log`)、shot_verify 30 枚 PASS (tol 0 で 29 枚 0、terrain は基点の絵と 0)、build_managed Release 0 警告。sub-16〜19 に割れ無し。round 2 の差分は Effector 存在ゲート内なので次のチェックポイント (sub-23) で被覆。
- (sub-20 VERDICT nit → sub-24) 限界欄: Effector の領域は tick 開始時の姿勢、Surface はソリッド Collider 必須で対象は plane2D の動的剛体のみ、有効な領域内の剛体は起こされ眠らない (島ごと)、Surface の起床は面 AABB + 0.05 の AABB 判定、親付きの領域と回転ベルトは未検証、Buoyancy の体積は 3D 体積 (density 既定 1000 は軽ければ必ず浮く)、領域にできない形状は kMesh / kTerrain / kEdge2D。checklists: 橙の矢印ギズモ、Joint の「2D (Z 軸)」ボタン、Effector の警告行、Create → 2D → Effector 2D。
- (sub-20 coder → sub-21 以降) TypeId 90 まで使用 (次は 91)、snapshot v63、ABI 不変。焼き値 joint2D A82F779A97738D34 / effector2D 9504211B9041ECC2。sub-23 のデモにベルトを置くなら乗せる物は接地状態から (半厚 0.1 以上、速度 3.0 で安定)。sub-21 の依頼に添える: forwardAxis は hash 対象の append なので kExpected の焼き値を同じサブで再採取、Perception LOS の CollectLosColliders は physicsquery::CollectStaticShapes でタイル / SpriteShape の箱も見る。
- (sub-21 VERDICT nit → sub-23 / 24) 部分経路が最終的に Arrived まで着くこと、一方通行床の上の歩行、親付き Agent は selftest 無し → sub-23 のデモで敵に一方通行床を含む面を追わせる。限界欄: 経路は 1 つ先のセルだけ記憶 (経路全体のギズモ無し)、頭上の空きは見ない、動く床と剛体の上は歩行不可、群衆回避なし、CC の jumpSpeed が maxJumpHeight / Length に足りるかは未検証。checklists: Create → 2D → Grid Agent 2D、橙ギズモ、XY の知覚扇形。`UpdateAgent` がやや長い (分割は任意)。
- (sub-21 coder → sub-23) 敵の GridAgent2D.tilemap をタイルマップのエンティティに向ける (Create では空)、CC は plane2D 必須で足元はタイル。BT の MoveTo は NavMeshAgent が無ければ自動で GridAgent2D。TypeId 91 まで使用 (次は 92)、snapshot v64、ABI 不変。焼き値 GridNav2D シナリオ A 05C77D287CAAA51B。m94 suite に Perception / BT を追加。
- (sub-22 VERDICT should 1 → sub-23 やること 9) Camera2DFollow の境界は CameraComponent.orthoSize を見るので、Create → 2D → Camera 2D が orthoSize を `refResolutionY / (2 × assetsPPU)` で初期化する (spec 4.1.12)。sub-23 で確認し未対応なら 1 行足す。
- (sub-22 VERDICT nit → sub-23 / 24) blit は自動テスト無し → sub-23 の golden `2d` (PixelPerfect 有効) で固定。`RenderSystem::Render` が長くなった (任意)。checklists: GameView 上の PixelPerfect、青い deadZone / bounds ギズモ、Create → 2D → Camera 2D、Inspector の有効値表示 (直近のゲームカメラ描画基準)。限界欄: PixelPerfect 中も UI / ImGui はフル解像度でポストは低解像度、cameraOverride (エディタ視界) には効かない、lastViewProjNoJitter は画面に見える範囲、GetCollisionBounds のギズモはアセット基準の近似。
- (sub-22 coder → sub-23) TypeId 93 まで使用 (次は 94 だが実行時スキーマの Health が 94 を使うログあり、着手時の末尾から取る)、snapshot v65、ABI 不変。焼き値 Camera2DFollow D5147EA06255DFC2。デモの Camera2DFollow.boundsFromTilemap はタイルマップを割り当てて useBounds、Camera.orthoSize は PixelPerfect の有効値 (PPU 16 / ref 180 なら 5.625)。全体 selftest で NavAgent 庭 8581BEE4A676337C を確認。
- (sub-23 チェックポイント 5、round 1) 全体 selftest Debug / Release FAIL 0 (PASS 8737、NavAgent 庭 8581BEE4A676337C)、replay_verify 22 ジョブ中 21 個別 PASS (rules は既存 3 error、全文 `cache\m94\replay_verify_cp5.log`)、shot_verify 32 枚 PASS (既存 maxDiff 0、terrain は基点と 0、`--update` 不使用、`cache\m94\shot_verify_cp5.log`)、build_managed Release 0 警告、cargo test 49 PASS。sub-21〜23 に割れ無し。
- (sub-23 VERDICT should 1 → sub-24 やること 5b) ADR-032 の「固定 21 ジョブ」を 22 に。5b: 各サブの確定事項と R9〜R12 の限界を engine_spec の 1 か所に集約 (敵は動く物・非タイル足場を見ない、froxel / deferred は 2D 経路に効かず forward と画素一致 = R11 は満たした)、`--2d-demo` / `--2d-night` / `--2d-stress` と性能値 (デモ 0.88〜1.39 ms、stress 4.43〜4.60 ms、sim 0.21 ms/tick、Raycast 12.7 us / BoxCast 67.5 us) を engine_spec へ、checklists に各 VERDICT で目視に回した GUI 操作を全部 (Create → 2D → Camera 2D の orthoSize 初期化を含む)。最終チェックポイントは sub-23 以降にソースが変わっていなければ sub-23 の結果を流用 (sub-24 受け入れ条件 4)。
- (sub-23 coder → sub-24 / reviewer) スクリプト名は ZTwoDHero / ZTwoDMover (既存スクリプトの TypeId を動かさない)。タイル配置を変えると TwoDDemoSelfTest の kExpectedHash と golden 2 枚が変わる (`MYE_SELFTEST_ONLY=twod` で採取)、gen_2d_assets.py の GUID は DemoContent.cpp の kTwoD*Guid と揃える。reviewer には物理 (sub-01 / 08 / 09 / 11 / 14 / 19 / 20) と描画 (sub-04 / 06 / 15 / 16 / 17 / 22) の変更範囲と `cache\m94\` の各チェックポイント全文ログを添える。
- (sub-24 VERDICT should 1、司会) coder が SELF_EVAL を sub-24.md に写さず harness.md の申し送り欄へ下書きを書いた (役の境界外)。司会が harness.md の追記を戻し、SELF_EVAL を sub-24.md の実装メモへ転記した。下書きの要点はこの欄に司会の文で残す: GUI の実クリックと実 GPU の目視は `docs\test_checklists.md` の M94 節に集約、v1 の限界は `engine_spec.md` §10.13 末尾に集約、長い関数 (`DrawCanvas` / `HandleShape2DEdit` / `UpdateAgent` / `SolveCharacters` の報告部 / `RenderSystem::Render` / `Sprite2DPass::Render` / `EmitShape` / `BuildTwoDShowcaseScene`) の分割は任意、合成入力の偏りで `2d` の replay 被覆は画面中央が薄い。`cache\m94\` (git 管理外、約 1 GB) の削除はユーザー許可が要る (未実施)。
- (sub-24 VERDICT nit) engine_spec §10.13 末尾の限界欄の Effector の行「a buoyant body in an active region never sleeps」を「a plane2D body in an active region never sleeps」に一般化する (本文 1929〜1932 行は正しい)。§12.2 に M92 / M93 の行が無いのは既存の欠落 (M94 外)。
- (sub-24 VERDICT 未決 [ユーザーに聞ける] = Q-9) check_rules の既存 3 error (EditorMain.cpp の `#ifdef _DEBUG`、tools\engine_dist の .bat 2 本の LF) を M94 の後に別コミットで直す。ユーザーの方針 (推奨案で通し Notion に記録) により planner の裁定どおり「直す」を採用。Phase 4 の後に別件として行う。
- (Phase 3 依頼に添える) 物理の変更範囲 (sub-01 / 08 / 09 / 11 / 14 / 19 / 20: 拘束 A/B/D・oneWay・kinDrive・StaticShapeSource と MotionRegions・Broadphase の staticOnly・physicsquery の注入・Effector と起床)、描画の変更範囲 (sub-04 / 06 / 15 / 16 / 17 / 22: Sprite2DPass・ステンシル・ライト / 影 / 法線 RT・サーフェス枝・PixelPerfect の低解像度 RT)、`cache\m94\` の cp1〜cp5 全文ログ、spec 4.1.x と engine_spec §6.15 / §10.13 の食い違いの重点確認、PhysicsSelfTest の既存節と全 replay ジョブの照合を重めに。
- (review round 1 → sub-25) 指摘は sub-01〜24 に差し戻さず新サブ sub-25 に集約。sub-25 はチェックポイント 6 (重い 3 本 + build_managed)。上昇床の判定を変えると動く oneWay 床を含む焼き値 (2d デモ 55DEACAD78012785 など) が動きうる → 理由を示して再採取。修正後は同じ reviewer へ REVIEW round 2 (消込から)。reviewer は撮影のたびに cache\2d_showcase.scene.json (再生成される生成物) を消していた。
