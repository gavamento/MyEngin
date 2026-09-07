# MyEngine

[![CI](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml/badge.svg)](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml)

C++20 / DirectX 11 製の自作ゲームエンジン。**Unity 風の使いやすさ × ECS の性能 × 壊れない開発体験** をコンセプトに、就職活動用ポートフォリオとして開発。仕様は [engine_spec.md](engine_spec.md)、設計判断の記録は [docs/adr/](docs/adr/) を参照。

## ビルドと実行

1. Visual Studio 2022 以降 (C++ デスクトップ開発ワークロード + Windows 10/11 SDK)
2. `MyEngine.sln` を開く → 構成 `Debug|x64` → F5

外部ライブラリはすべて `external/` にソースごとコミット済み (クローン → F5 で動く)。

| プロジェクト | 種類 | 内容 |
|---|---|---|
| Engine | 静的ライブラリ | Platform / Core / Renderer / Engine の 4 レイヤ |
| Editor | exe | ImGui エディタ (開発時のホストプロセス) |
| Runtime | exe | エディタ UI 無しの配布用ランタイム (エンジンは Editor と完全共有) |
| GameLogic | DLL | ユーザースクリプト。**ホットリロード対象** |

sln の外にもう 2 本ある。どちらも無い状態でエディタは起動し、該当機能だけが OFF になる:

- **C# スクリプトホスト** — `tools\build_managed.bat Debug` / `Release` (.NET SDK が要る)。
  出力は `bin\x64\<Config>\MyeScripting.dll`。両構成とも起動時に読むので両方作る
- **Source Control のサービス** — `tools\build_collab.bat` (**rustup の stable ツールチェーン**が要る)。
  Rust の cdylib `MyeCollab.dll` + `MyeCollabCli.exe` を 1 回のビルドで作り、両構成の `bin\x64\` へ置く。
  無ければ Source Control 窓が「利用不可 (サービスがありません)」になるだけで他は無傷

## エンジンで作ったもの

エンジンのショーケース (`--physics-demo` / `--joint-demo` / `--acoustic-demo` 等) は
リポジトリ内の C++ (`DemoContent.cpp`) から組まれている。**それとは別に、`--project` で開く
外部プロジェクトとしてゲームを作り**、「プロジェクトを開く → スクリプトを書く → アセットを
置く → ゲームにする」という経路そのものを検証している。

- **仮ゲーム「HAL Collector」** — シーン 7 枚 (`main` は 39 エンティティ)、タイトル → ゲーム →
  リザルトの一周。歩く / 視点切替 / 拾う / 撃つ / 敵 / 車 / ポーズ / ハイスコアまで。
  ここで踏んだ穴が M64a (生マウスデルタ + カーソルロック + ABI v15) と
  M64b (`Active` の階層伝播 / 2 つ目以降の `Start()`) のエンジン修正になった
- **三校プロトタイプ** — 企画「暗闇 × 音のステルス」の縦切り。音の波が壁を照らすところまで。
  エンジン側の対応が M65 (音響伝播) と M68 (その波を耳へ出す)

**ドッグフーディングの記録は [docs/dogfooding.md](docs/dogfooding.md)** —
作者視点で踏んだ 20 件を「何をしようとした / 何が無かった / どう回避した / エンジンをどう
直すべきか」の形で残してある。**4 件は修正済み、16 件は未解決**で、その台帳も同じ文書にある
(最上位は「スキーマ未登録のコンポーネントがエディタ保存で黙って消える」= データ消失)。
エンジン単体の回帰テストでは絶対に出てこない種類の穴が並んでいる。

## 主要機能

- **ハイブリッド ECS** — 外部 API は `GameObject` / `GetComponent<T>()`、内部はアーキタイプ別 SoA。
  世代付き EntityID で破棄後のハンドルを検出。構造変更はコマンドバッファで tick 末一括適用
- **リフレクション基盤** — 1 つのフィールド表を Inspector 自動生成 / JSON シリアライズ /
  DLL リロード時の状態移行 / ワールドハッシュの 4 者で共用
- **ホットリロード** — シェーダ (include 依存グラフ + 失敗時は旧維持) / テクスチャ / glTF /
  シーン JSON (fileId 差分適用) / **C++ コード (GameLogic.dll)**。DLL は PDB ごとコピー +
  `/PDBALTPATH` でデバッガのブレークポイントを維持し、フィールドは名前+型一致で移行
- **パーティクル二重実装** — CPU (SoA + SSE、スカラー参照実装つき) と GPU (Compute、
  dead/alive リスト + DrawInstancedIndirect、リードバックなし)。実行時切替 + 並走比較モード。
  乱数は両者ともエンジンの決定論 RNG (GPU では乱数を生成しない)
- **レンダリングパス切替** — Forward / Deferred を実行時切替 (View > Render Path)。
  ライティング関数は common.hlsli を共用し見た目が一致。透明物とパーティクルは共通の Forward 後段
- **ハイブリッド・リアルタイムパストレーシング** — 一次光線はラスタのまま、**二次光線
  (拡散 GI / 平行光の影 / スペキュラ反射) を自前の BVH トラバーサルで置換**。
  Feature Level 11_0 縛りで DXR が使えないため `cs_5_0` のコンピュートシェーダで実装し、
  SVGF (テンポラル蓄積 → 分散推定 → A-Trous) でデノイズする。発光マテリアルはそのまま
  GI の面光源になる。詳細は [ADR-009](docs/adr/ADR-009-hybrid-path-tracing.md)
- **ReSTIR 反射 + ReflectionClass (既定 off)** — 反射レーンは 1spp なので、分散を隠す
  A-Trous が反射像のディテールごと溶かしてしまう。`--rt-restir` は**レイ数を増やさずに**
  時空間のサンプル再利用 (ReSTIR) で実効サンプル数を上げる。reservoir が持つのは方向ではなく
  **ヒット点そのもの**なので、借りた側は自分の視線・法線・粗さで重みを評価し直せるうえ、
  **そこに刺さっているオブジェクトのクラスが分かる** — これが `ReflectionClass` の土台。
  クラスは「反射する床」ではなく**反射に映る物体**の属性 (`Material` の 5 段: 主役 / 人型 /
  乗り物 / 小物 / 既定) で、主役ほど再利用を絞り (にじませない・ゴーストさせない)、
  小物ほど積極的に借りる。G-Buffer には 1 ビットも触れていない。
  **off の絵は現行とビット一致**で、それを golden 3 枚 (`demo_render_rtrefl` /
  `_rtgi` / `_rtrefl_restir`) が機械証明する。詳細は
  [ADR-016](docs/adr/ADR-016-restir-reflection.md)
- **剛体物理 (自作ソルバ)** — 蓄積インパルス + サブステップの接触ソルバに、空力 (等方抗力 /
  翼面 / マグヌス) / 浮力 / ジャイロ項 / 静動摩擦 / 転がり抵抗 / 材料資産 (`.physmat.json`) /
  スリープとアイランド / CCD / 地形ハイトフィールドを積んである。その上に**関節と機構**が
  重なる: ボール / ヒンジ / 固定 / スライダ / コーンの 5 種を単一 `Joint` の `type` で選び、
  角度・変位リミット / モータ / 破断 (breakForce・breakTorque) / 粘着 / 複合コライダー /
  凸包 (クック時生成) / **ラグドール** (スケルトンから自動生成 + 剛体 → 骨の逆駆動) /
  **車両** (レイキャストサス + タイヤ力) まで、Inspector でコンポーネントを足すだけで組める。
  **既存シーンは 1 ビットも変わらない** — 全部が「そのコンポーネントが在るときだけ効く」
  存在ゲートの内側にあり、`--physics-demo` / `--joint-demo` の 2 ペアが Debug ⇔ Release の
  ハッシュ一致でそれを守っている
- **XPBD 変形体 (M60′、途中まで)** — 剛体ソルバとは別の池に粒子と拘束を持つ変形体レーン。
  現在あるのは **ロープ** (`RopeComponent`、TypeId 44) だけで、`XpbdSolver` / `XpbdBackend` の
  核と、剛体との双方向アタッチまでが動く。粒子数がオーサリング依存で可変なので状態は
  ECS カラムではなく池に住み、`SimSources` 経由で `WorldHasher` と `SimSnapshot` (v6) の
  両方に載せてある。**布とソフトボディは未実装** (a〜d 完了 / e〜n 中断) で、粒子と世界の
  衝突もまだ無い。replay と golden の被覆はセルフテストのみ
- **音響伝播と、その波が実際に鳴ること (M65 + M68)** — 整数チャンファ距離 (26 近傍の
  `<11,16,19>`) の波面を **1 tick 1 リング**で広げ、**1 枚の場が 4 つの役**を持つ:
  残光ボリュームの描画 / 敵 AI の聴覚 (到来方向つき) / 同じ重みで引いたナビゲーション /
  **プレイヤーの耳に届く音** (M68)。オーディオ側はリスナーから 3 本目の Dial を一気に走らせ、
  経路長で遮蔽と回折を整形する — 角を回る音は**戸口の位置へ音源ごと移して**回折ローパスを掛け
  (パンナーは壁を知らないまま正しい方向へ鳴る)、局所の開放度から **2 つの I3DL2 プリセットを
  連続補間**して廊下 → 部屋の残響が段差なく変わり、発生した波そのものが足音・衝撃音として鳴る
  (**鳴る範囲 = 波の到達範囲**。`RolloffGain` が到達上限で厳密に 0 になることが根拠)。
  **sim 状態は波スロット表だけ** — 場も残光もリスナー場も派生値でハッシュに載らないので、
  リプレイ 7 ペアと golden 22 枚は M68 を通して 1 ビットも動いていない。
  耳を使わずに配管を検査する口が `--acoustic-audio-log N` (整形した voice と一発再生を 1 行ずつ +
  summary)、ショーケースが `--acoustic-demo`。詳細は
  [ADR-017](docs/adr/ADR-017-acoustic-audio.md)
- **解像度に依らないゲーム内 UI (M70b)** — UI の数値は基準 1920x1080 の**キャンバス単位**で、
  実 px へは `s = min(w/1920, h/1080)` の**一様スケール**だけを掛ける (Unity の Canvas Scaler
  = Expand / UE5 の UMG DPI スケーリングと同じモデル)。キャンバス矩形は画面のアスペクトへ
  伸びるので**レターボックスは出ず**、端アンカーの UI は必ず本当の画面端に付く。
  それまで描画は実 px、ヒットテストとフォーカスナビは 1920x1080 固定で、
  1920x1080 で走る構成がリポジトリに 1 つも無いため (既定 1600x900 / 撮影 960x540)
  **`anchor=0` 以外は「見えている場所」と「押せる場所」が常にズレていた**。
  ★キャンバス寸法とキャンバス座標のマウスは `InputSnapshot` に載せて `.rep` に記録する —
  UIElement は非ハッシュなので、記録せずに実解像度を渡すと「窓の大きさで当たり判定が変わるのに
  リプレイは緑」になる。ネット対戦はハンドシェイクでキャンバスを照合し、
  アスペクトの違う 2 台は接続を拒否する (16:9 同士は解像度が違っても通る)。
  被覆は golden の 23/24 枚目 (1280x720 = スケール経路 / 960x600 = 可変キャンバス経路)。
  詳細は `engine_spec.md` §6.11
- **エディタの日本語化** — UI 言語は**日本語が既定**で、View > 言語 から実行時に英語へ切替。
  文字列は X マクロ 1 ファイルに集約し、訳の書き忘れを**コンパイルエラー**にする。
  ウィンドウ名は `"表示名###英語ID"` 形式なので、切り替えても ImGui の ID —
  つまり `imgui.ini` とドッキング配置 — は 1 バイトも変わらない。
  Inspector の表示名は `FieldDesc::displayName` に持ち、シリアライズキー兼ハッシュ入力である
  英語の `name` には触れない。詳細は [ADR-010](docs/adr/ADR-010-editor-localization.md)
- **エディタ内 Git 連携 (Source Control)** — 変更一覧 → stage → commit → push、fetch → pull、
  ブランチの作成と切替、競合の abort / ours / theirs までを**エディタを閉じずに**通す。
  設計の中心は「`pull` や `checkout` が走っているエディタの足元でファイルを書き換える」瞬間:
  書き込み系は**全文書が保存済みかつ何も実行中でないとき**だけ押せて (阻害要因 13 種を全部並べて出す)、
  実際に変わったファイルの集合から **A (その場でホットリロード) / B (シーンを開き直す) /
  C (再起動)** を決める。`.meta` や `.terrain.edit` は本体と一体で扱い、
  Content Browser のバッジにも同じ状態が出る。実体は Rust の cdylib `MyeCollab.dll` で、
  会話は **UTF-8 の JSON 1 本 + C ABI 6 関数**だけ (`cargo test` と
  `tools\collab_verify.bat` がエディタ抜きで回帰を取る)。**sim には 1 バイトも触れない** —
  Engine / Runtime / GameLogic / Shared からの include を静的検査 (規則 12) が禁じている。
  前提は git 2.11 以上と rustup、そしてチーム規則「全員同じパスに clone」
  (サブアセット ID が絶対パス由来のため。`project.mye.json` の `canonicalRoot` が記録して食い違いを警告する)。
  **初回の認証だけはターミナルで一度 `git push` して済ませておく** — 背景 fetch は
  資格情報のダイアログを意図的に抑止している。詳細は [engine_spec.md §14](engine_spec.md) と
  [ADR-015](docs/adr/ADR-015-in-process-rust-collab.md)。v1 でやらないこと: PR / レビュー / LFS /
  sparse checkout / シーンの 3-way マージ / `git init` / 認証 UI
- **Debug/Release 一貫性** — 固定 60Hz tick、`/fp:precise`、PCG32、明示ソートキー。
  リプレイ (.rep) の tick 毎ワールドハッシュ比較で機械検証:
  `tools\replay_verify.bat` が両構成ビルド → Debug 記録 → Debug/Release 照合 → 静的規則検査。
  **被覆は 7 シーン**: 既定デモ (物理 / パーティクル / スクリプト) / 部位ショーケース
  (スキンメッシュのボーン追従 = 骨駆動 LocalTransform の構成間ビット一致) /
  ゲームフロー統合デモ (シーン遷移・ポーズ・セーブ・アクションマップ) /
  ローカル 2P デモ (プレイヤー別入力レーンの配線) /
  物理ショーケース (空力・浮力・マグヌス・ジャイロ・材料・CCD) /
  関節ショーケース (拘束ソルバ・リミット・モータ・破断・複合・凸包・ラグドール・車両) /
  音響ショーケース (波スロット表 + 敵 FSM + プレイヤー操作。**記録側だけ `--synth-input`** —
  視点角は生マウスデルタの積分なので、無入力だと恒常ゼロで検査にならない)。
  割れた tick は**どのエンティティのどのフィールドが**割れたかまで自動で出る (`--hash-diff`)
- **クラッシュしたら「再現可能なバグ報告」が自動で残る** — 例外 (スタックオーバーフロー含む) /
  `std::terminate` / 純粋仮想呼び出し / CRT 不正パラメータを捕まえ、`crash\<日時>\` に
  minidump + `crash.txt` (障害モジュール + RVA + ビルドの git ハッシュ + 起動コマンドライン) +
  **`crash.rep`** を吐く。`crash.rep` は開始スナップショットを埋め込んだリプレイなので、
  受け取った側が `Runtime.exe --replay-verify crash.rep` するだけで
  **起動シーンに依らず落ちる直前の tick までハッシュ一致で再現**する
  (Debug の Editor で出た報告を Release の Runtime で再生できることを実測)。
  ハンドラ内では一切ヒープを触らないよう、.rep のバイト列は平常時から組み上げて持っている
- **決定論を転用したネットコード (2 人 P2P)** — UDP + 遅延ロックステップ + **予測ロールバック**。
  未着の相手入力を「直近の確定値の繰り返し」で予測して先へ進み、外れたら最大 8 tick 巻き戻して
  **通常 tick と同じ `RunOneTick`** で再シムする。ネット層は sim 状態を 1 バイトも書かない —
  「いつ tick が回るか」は実時間依存でよいが「tick が何を消費するか」は確定入力だけで決まる、
  という分離がすべて。`tools\net_verify.bat` は 2 プロセスを実際に起動して
  **2 台の .rep がバイト一致**し、さらに**ローカル 2P 実行の .rep とも一致**することを確かめる
  (遅延 1 tick + ロス 30% で 21 回巻き戻しても一致を実測)。接続時は API 版 / .rep 版 /
  起動オプション / **開始ワールドハッシュ**を照合して不一致は拒否。走行中も 8 tick ごとに
  確定ハッシュを交換し、割れたら `crash\desync_<tick>_p<lane>\` に再現可能なバンドルを吐いて停止する。
  詳細は [ADR-013](docs/adr/ADR-013-predictive-rollback-netcode.md)
- **CI (GitHub Actions)** — push ごとに 8 ビルド (4 プロジェクト × Debug/Release、警告 0 を強制) +
  リプレイ照合 7 ペア + 静的規則検査 + セルフテスト両構成 + 配布パッケージのスモークが回る。
  **GPU の無い runner でも回る**のは sim が CPU 専用だから — 描画は WARP
  (ソフトウェアラスタライザ) へ自動フォールバックし、ワールドハッシュはドライバに依らず一致する
  (WARP で録った .rep が RTX 3060 でそのまま照合できることを実測)

## エディタ操作

- **Scene ビュー**: 右ドラッグ + WASDQE (Shift で加速) — エディタカメラ
- **Play / Pause / Step**: メニューバー中央。Play 中の編集は Stop で破棄 (Unity 方式)
- **Game ビュー**: シーン内カメラ視点。Play 中は矢印キーで BoxTextured (プレイヤー) が移動、
  黄色い Spawned キューブに触れると回収 (GameLogic.dll の `OnTriggerEnter`)
- **Inspector**: リフレクションから widget を自動生成。スクリプトのフィールドもここに出る
- **Particle Settings**: CPU/GPU 切替・比較モード・SIMD トグル・更新時間表示

## CLI (検証/CI 用)

```
Editor.exe --selftest                     # ECS + シリアライザ回帰テスト
Editor.exe --replay-record out.rep --replay-ticks 600
Editor.exe --replay-verify out.rep        # exit code 0/1
Editor.exe --autoplay --deferred --frames 600 --screenshot shot.png
Runtime.exe --deferred --rt-demo --rt-gi --rt-shadow --rt-refl --rt-anim-seed
                                          # レイトレのショーケース (コーネル箱)
Runtime.exe --render-demo --deferred --rt-refl --rt-restir
                                          # ReSTIR 反射 (M67) = スクショ 22 枚目。
                                          #   既定は temporal のみ。--rt-restir-spatial で
                                          #   空間再利用も on (既定 off = 目標帯の計測結果)、
                                          #   --rt-restir-no-spatial は明示 off、
                                          #   --rt-restir-visray は候補ごとに可視レイを撃つ
                                          #   (spatial を含意)。--rt-class-override N で
                                          #   全インスタンスの ReflectionClass を強制 (-1 = off)
Editor.exe --parts-demo                   # 部位 (ソケット) のボーン追従シーン
Editor.exe --acoustic-demo                # 音響ショーケース (L 字廊下 + 2 部屋 + 床材 6 枚 +
                                          #   敵 2 種 + プレイヤー) = replay 7 ペア目 +
                                          #   スクショ 18/19 枚目。波は SceneView の「音響」
                                          #   トグルでしか見えない
Editor.exe --acoustic-demo --acoustic-audio-log 300 --synth-input
                                          # 耳を使わずに音響 x オーディオの配管を検査する。
                                          #   整形した voice と波の一発再生を 1 行ずつ +
                                          #   終了時 summary。--no-audio と併用すると 0 行
Editor.exe --terrain-demo [--terrain-lod N] [--terrain-skirt N]
                                          # 地形ショーケース (M58c) = スクショ 8 枚目
Editor.exe --flow-demo                    # タイトル/ゲームのシーン遷移 + セーブ/ロード統合デモ
Editor.exe --local-demo --local-players 2 # ローカル 2P (レーン n は XInput スロット n)
Editor.exe --particle-demo                # 粒子表現のショーケース (M63a) = スクショ 16/17 枚目
                                          #   (CPU/GPU の突き合わせ)
Editor.exe --scene assets\scenes\x.scene.json
                                          # 任意のシーンを開く (相対パス可)
Editor.exe --physics-demo                 # 物理ショーケース (空力/浮力/マグヌス/材料/CCD)
                                          #   = replay 5 ペア目 + スクショ 13 枚目
Editor.exe --joint-demo                   # 関節ショーケース (関節/機構/ラグドール/車)
                                          #   = replay ペアの 6 本目 + スクショ 14 枚目
Runtime.exe --render-demo [--deferred]    # 描画ショーケース (スポット/点光源/反射床/フォグ/遠景)
                                          #   = スクショ回帰 6/7 枚目の被写体
Runtime.exe --render-demo --deferred --froxel
                                          # ボリュメトリック霧 (フロクセル)。on にすると
                                          #   距離フォグはグリッドの外側だけを持ち、
                                          #   ゴッドレイは自動 off になる (三重計上の解消)。
                                          #   Forward / Deferred どちらでも効き、不透明・半透明・
                                          #   地形・空・パーティクル (CPU/GPU 両方)・
                                          #   VFX (Sprite/Trail/TextMesh) に載る。
                                          #   UI は表示 > レンダリング > ボリュメトリックフォグ
Runtime.exe --fog-demo --froxel --particle-backend gpu
                                          # 霧のショーケース (M57追補) = スクショ 15 枚目。
                                          #   GPU パーティクルと Sprite/Trail/TextMesh の
                                          #   唯一のピクセル被覆 (それまでどちらも golden に
                                          #   1 枚も写っていなかった)。柱を 10/25/45/70m に
                                          #   並べてグリッド端 (64m) の受け持ち交代を絵に出し、
                                          #   画面上で同じ大きさの板 2 枚で霧の量だけを比べる
Runtime.exe --particle-compare            # CPU/GPU を横に並べて描く (設定は書き戻さない)
Runtime.exe --render-demo --deferred --taa
                                          # TAA (M55d、Deferred のみ)。--ssr で SSR、
                                          #   --hzb-debug N で Hi-Z ピラミッド、
                                          #   --velocity-debug で速度バッファを可視化
Runtime.exe --render-demo --deferred --froxel --froxel-dump 3
                                          # フロクセルを読み戻して CPU と照合 (調査専用)。
                                          #   --froxel-no-temporal でテンポラル再投影を外す。
                                          #   音響側の同型は --acoustic-dump N
Runtime.exe --render-demo --deferred --rt-refl --rt-debug 12
                                          # RT のデバッグ表示 (12 = reservoir の M /
                                          #   13 = 一次ヒットのクラス / 14 = 反射像側のクラス)。
                                          #   --rt-no-temporal / --rt-no-svgf でデノイザの段を
                                          #   外す A/B、--rt-freeze-seed で乱数を止める
                                          #   (撮影時は自動 freeze。画質を測るなら --rt-anim-seed)
Editor.exe --snapshot-stress 600          # スナップショットの撮影/復元を往復させ続ける
Editor.exe --timetravel-selftest [N]      # タイムトラベルのシーク結果と記録ハッシュを照合
Editor.exe --replay-record out.rep --replay-fast
                                          # 記録を早回し (描画を待たない)。replay_verify が使う
Editor.exe --img-diff a.png b.png --tol 3 # PNG 差分 (exit code で成否)
Editor.exe --font-embedded                # 内蔵フォント固定 (スクショの機種差を殺す)
Editor.exe --screenshot shot.png --shot-frame 120 --frames 200
                                          # 決定的撮影。frame 番号 == tick 番号になり、
                                          #   生マウスデルタは 0 に固定される (M68c)。
                                          #   --shot-every N で連写 (この場合は決定的にならない)
Runtime.exe --no-crash-handler            # クラッシュハンドラを外して素で落とす
Runtime.exe --net-demo --net-join HOST:PORT --net-loss 5 --net-no-halt-on-desync
                                          # パケットロス注入 / desync でも止めずに継続
Editor.exe --create-project DIR --template demo
                                          # プロジェクトを CLI で作る (--template は empty|demo、既定 empty)
Editor.exe --lang en --width 1600 --height 900
                                          # 起動言語とウィンドウサイズ
Editor.exe --package dist --package-dds --package-zip
                                          # DDS 一括クックと zip 圧縮まで含めてパッケージ
Editor.exe --warp                         # WARP (ソフトウェアラスタライザ) 固定で起動
Editor.exe --package dist                 # 配布パッケージを CLI で作成 (exit code で成否)
Runtime.exe --crash-test av --crash-at-tick 60
                                          # 意図的に落としてクラッシュバンドルを作る
Runtime.exe --net-demo --net-host 7777    # 2 人対戦デモ (ホスト)
Runtime.exe --net-demo --net-join 127.0.0.1:7777 --net-delay 3
                                          # 同 (参加側)。--net-no-rollback で素のロックステップ
Runtime.exe --net-poke-tick 60            # 片側だけ壊して desync 検出と診断チェーンを試す
Runtime.exe --rep-diff a.rep b.rep        # 2 本の .rep がどの tick で割れたか
tools\replay_verify.bat                   # 一貫性検証一式 (7 シーン被覆)
tools\shot_verify.bat [--update]          # 決定的スクショ 24 枚を tests\golden と比較
tools\crash_verify.bat                    # 5 経路で実際に落として .rep の再現性を検証
tools\net_verify.bat                      # 2 プロセスのネット対戦 + desync 検出の実地検証
tools\check_rules.ps1                     # コーディング規則の静的検査
tools\gen_project_files.ps1               # ソース一覧を vcxproj に反映
tools\collab_verify.bat [--update]        # Source Control の回帰検証。一時リポジトリへ
                                          #   NDJSON のシナリオを流し、期待出力と比較する
                                          #   (エディタも D3D も要らない。先に build_collab.bat)
pwsh -File tools\collab_fixture.ps1 <dir> # git 管理下の最小プロジェクトを作る。実機目視は
                                          #   Editor.exe --project <dir> (Source Control は
                                          #   --project 起動でしか動かない)
```

上は**作者が使う口**だけ。ほかに調整・調査専用のフラグが 24 本ある
(`--bloom-threshold` / `--exposure` / `--postfx-mode` / `--no-jobs` / `--no-cook-cache` /
`--hash-dump` / `--pick-test` / `--probe-bake*` / `--manager-shot` など)。
実在する全 113 本は `src\Editor\EditorMain.cpp` と `src\Runtime\RuntimeMain.cpp` の
引数解析が正本。

CI (`.github\workflows\ci.yml`) は**この bat をそのまま呼ぶ** — CI 専用の検証ロジックは
書かない。CI 固有の事情は環境変数 4 種だけで注入する:

| 変数 | CI での値 | 用途 |
|---|---|---|
| `MYE_EXTRA_ARGS` | `--warp --no-audio` | 全 `Editor.exe` 実行へ後置 (GPU / 音源の無い runner 用) |
| `MYE_MSBUILD_ARGS` | `/p:MyeWarnAsError=true` | 警告 0 を強制 (既定 off。ローカル開発は止めない) |
| `MYE_DOTNET_ARGS` | `/p:TreatWarningsAsErrors=true` | 同上 (C# 側。綴りが違う) |
| `MYE_SHOT_SKIP_FXAA` / `_TAA` / `_SSR` / `_FROXEL` / `_FOG` / `_PARTICLE` / `_RT` | `1` | 機種差が増幅する 10 枚をランナーでは撮らない (tol=0 のローカル限定枠) |

## 計測 (RTX 3060 / 1600x900 / Release)

レイトレ 3 レーンを全部 on にしたときの GPU 時間。GI と反射は内部 1/2 解像度、影はフル解像度。
`[rt]` ログの GpuTimer は最終フレーム 1 サンプルなので、同一条件 5 回の**最小値**を載せている。

| パス | 既定デモ (522 インスタンス) | コーネル箱 (11 インスタンス / 780 三角形) |
|---|---|---|
| BVH 構築 (CPU) | 0.20 ms | 0.02 ms |
| 拡散 GI (1spp) | 0.48 ms | 1.50 ms |
| テンポラル + SVGF | 0.35 ms | 0.55 ms |
| RT 影 (トレース + フィルタ) | 0.49 ms | 0.49 ms |
| RT 反射 (トレース + デノイズ) | 0.74 ms | 0.49 ms |
| **合計** | **約 2.1 ms** | **約 3.0 ms** |

インスタンス数が 1/50 でも閉じた箱の方が GI が 3 倍重い — **全レイがジオメトリに当たり
2 バウンス目と影レイまで必ず走る**ため。コストを決めるのは三角形数ではなくレイの平均行程。

## アーキテクチャ

```
Editor      ImGui エディタ (Hierarchy / Inspector / SceneView / Profiler ...)
GameLogic   ユーザースクリプト DLL — C ABI (src/Shared) だけを介してエンジンと通信
Engine      シーン / GameObject / ホットリロード制御 / パーティクル / リプレイ
Renderer    DX11 抽象 / IRenderPath (Forward・Deferred) / シェーダ管理
Core        ECS / リフレクション / RNG / ログ / FileWatcher / JSON
Platform    Win32 / 入力 / 時間 / DLL ロード
```

上位レイヤは下位レイヤのみに依存。生の D3D 型は Renderer 層より上に出さない。
DLL 境界 (`src/Shared/`) は C ABI + POD のみ (STL / vtable / 例外は越えない)。

## ドキュメント

- [docs/adr/](docs/adr/) — Architecture Decision Records (設計判断とトレードオフ)
- [docs/dogfooding.md](docs/dogfooding.md) — 外部プロジェクトの作者視点で踏んだ 20 件 (4 件修正済み / 16 件未解決)
- [docs/demo_script.md](docs/demo_script.md) — デモ動画の台本
- [docs/test_checklists.md](docs/test_checklists.md) — 手動テスト手順 (ホットリロード)
