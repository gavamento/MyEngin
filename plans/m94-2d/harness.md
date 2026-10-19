# harness 台帳: m94-2d

- 依頼原文: 2D レーンと2D分野をさらに拡充させたい
- 開始: 2026-10-20 / 基点コミット: 25fb355
- フェーズ: 実装 (策定完了 2026-10-20。spec.md 確定: 疑い 31 件、受け入れ条件 40 件、サブ 24 本)

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
| sub-04 | 実装中 | 0 | | Sprite2D と Sprite2DPass (Simple / flip / 整数ソート / Point・Linear)。依存: 02, 03 |
| sub-05 | 未着手 | 0 | | スプライトエディタ窓。依存: 03 |
| sub-06 | 未着手 | 0 | | Sliced / Tiled、SortingGroup2D、SpriteMask2D。依存: 04 |
| sub-07 | 未着手 | 0 | | スプライト列クリップ (.spriteanim.json、コントローラの "sprite")。依存: 04 |
| sub-08 | 未着手 | 0 | | 2D 形状 (kPolygon2D / kEdge2D) と C++ クエリ。依存: 01 |
| sub-09 | 未着手 | 0 | | CharacterController の plane2D (一方通行・移動床・剛体押し・報告)。依存: 01 |
| sub-10 | 未着手 | 0 | | タイルパレット / タイルマップ資産、Tilemap、チャンク描画。依存: 04 |
| sub-11 | 未着手 | 0 | | タイル衝突 + セル書き換えの sim 状態 + 破壊。依存: 08, 10 |
| sub-12 | 未着手 | 0 | | タイルパレット窓。依存: 10, 02 |
| sub-13 | 未着手 | 0 | | ルールタイル、アニメーションタイル、レイヤー UI。依存: 11, 12 |
| sub-14 | 未着手 | 0 | | ABI (10 本) + C# ミラー + MyeScript。依存: 08, 11。**版は着手時の末尾 (M75h より先なら v31)** |
| sub-15 | 未着手 | 0 | | Light2D とライトバッファ。依存: 06 |
| sub-16 | 未着手 | 0 | | ShadowCaster2D と法線マップ。依存: 15 |
| sub-17 | 未着手 | 0 | | スプライトのサーフェスシェーダ枝、Parallax2D、2D ポスト例。依存: 06 |
| sub-18 | 未着手 | 0 | | Line2D。依存: 06 |
| sub-19 | 未着手 | 0 | | SpriteShape2D (描画・衝突・ハンドル)。依存: 08, 18 |
| sub-20 | 未着手 | 0 | | 2D ジョイント検証と Effector2D。依存: 09 |
| sub-21 | 未着手 | 0 | | GridAgent2D、BT MoveTo の fallback、Perception forwardAxis。依存: 11, 09 |
| sub-22 | 未着手 | 0 | | Camera2DFollow と PixelPerfectCamera2D。依存: 02, 11 |
| sub-23 | 未着手 | 0 | | --2d-demo、golden 2d / 2d_lights、replay_verify 2d。依存: 01〜22 |
| sub-24 | 未着手 | 0 | | 文書 (engine_spec / ADR-033 / test_checklists / README)。依存: 23 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 保留質問 (planner の裁定で通したもの。Notion 活動記録「M94 2D: ハーネスの保留質問」にも写した)
- Q-1 plane2D の z を 0 に強制するか初期値を保つか / 裁定: 初期値 (z0) を保つ。異なる z の plane2D 物同士は当たらない (Unity と違う、注記する) / 逆を選ぶと: sub-01 の D 拘束が pz=0 固定になり、z をずらして配置した物理物が初回 tick で飛ぶ
- Q-2 CC の形状をカプセルのままにするか箱 CC も足すか / 裁定: カプセルのまま (既存の押し出し・段差・坂を流用) / 逆を選ぶと: sub-09 に箱 CC の押し出し経路が 1 本増える
- Q-3 一方通行床の判定を固定 180° 弧 + 前位置チェックにするか、Unity の Surface Arc 可変にするか / 裁定: `Collider.oneWay` bool で固定 / 逆を選ぶと: Collider に float 1 本増え、CC とタイルの判定にも角度が入る
- Q-4 ソート層の上限を 32 (安定 id 0..31、2D ライトの targetLayerMask が uint32) にするか無制限にするか / 裁定: 32 / 逆を選ぶと: Light2D の対象指定が名前リストになり POD に収まらず、sub-15 の設計が変わる
- Q-5 法線マップ付きスプライトを v1 (sub-16) に入れるか後回しにするか / 裁定: 入れる / 逆を選ぶと: sub-16 は影だけになり受け入れ条件 29 の後半が落ちる
- Q-6 グリッド A* を GridAgent2D 新設 + BT MoveTo の fallback にするか、Nav の抽象化にするか / 裁定: 新設 + 2 行の fallback (NavMeshAgent と同じ status 値) / 逆を選ぶと: M82 の NavSystem / BT に抽象層が入り sub-21 が大きくなる
- Q-7 ABI bump の位置を段階 3 末尾 (sub-14) にするか段階 8 にするか / 裁定: sub-14 (段階 3 で横スクロールが作れる = スクリプトからタイルを触れる必要がある。bump は 1 回のまま) / 逆を選ぶと: 段階 1〜3 の成果物がタイルのスクリプト操作抜きになる
- Q-8 タイルパレット窓 / スプライトエディタ窓の Undo を独自 deque (BT 窓の流儀) にし、Play 中は塗れない、でよいか / 裁定: よい / 逆を選ぶと: Play 中の塗りを sim の override として記録する経路 (sub-12) が 1 本増える

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
