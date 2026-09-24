# harness 台帳: m79-project-surface-shaders

- 依頼原文: M79: プロジェクト側サーフェスシェーダー。M78 (プロジェクト側ポスト/コンピュート, plans/m78-project-shaders/) と同じ作法で、プロジェクト assets に `*.surface.hlsl` (生 HLSL, VSMain/PSMain を作者が書く) を置き、先頭 `/*@MyEngineProperties ... @*/` で Inspector にマテリアル単位のパラメータが出る。`.mat.json` の `shader` で参照 (Inspector で選択可)。エンジンは PerFrame(カメラ/光/影/霧)・PerObject を名前付き CB＋共通 include で供給し作者は register を書かない。失敗時はマゼンタ＋エラー表示。バリアントなし。初版はフォワードのみ (ディファード後回し)、ABI 追加なし想定。論点: 頂点変位を深度/影/速度(TAA)パスへどう反映するか (VSMain 使い回し vs 任意の VSShadow 等の追加エントリ)。動機: Water プロジェクト (C:\Users\akita\Documents\MyEngineProjects\Water) の main シーンで浮世絵風の動画を作るため (トゥーン/平塗りライティング、Gerstner 頂点変位の水面、作り直す大波の巻き込みアニメ)。既存参考: plans/m78-project-shaders/reference-unity-ue.md §1 サーフェス、Water の assets/shaders/water_surface.hlsl・ukiyoe_flat.hlsl (現状 cbuffer 40 行を手写し・未使用)。
- 開始: 2026-09-24 / 基点コミット: 3b55f4a
- フェーズ: レビュー

## ユーザー判断
- 作者形式は「M78 のコンピュートと同じ感じ」(生 HLSL + Properties ブロック + 名前バインド)。司会が当初出した「表面関数/ライティング関数/頂点変位関数だけ書く抽象化」案はユーザーにより却下
- 最終成果物は動画 (TAA 前提。頂点変位の速度ベクトル反映が重要)
- エンジン変更は「プロジェクト側でシェーダーを作れるようにする (Unity/UE のように)」方向なら可
- 開始前の未コミット変更は M78f `3b55f4a` としてコミット済み (ユーザー選択)
- (2026-09-24) spec §7 の [ユーザーに聞ける] 5 件はすべて planner 裁定どおりで確定: ①Forward と Deferred の両方で描く ②VSMain 再評価 (生成エントリ) ③WaterWave の surfaceMaterial を含める (sub-05) ④gTime = 描画フレーム番号/60 ⑤PerMaterial は D3DReflect の名前で詰める

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | e492a85 | 方式成立を WARP 実描画で確認。hot reload/cache・フロクセル霧は sub-02 へ、.cs.hlsl off-by-one 修正は sub-04 へ移管 |
| sub-02 | OK | 2 | 2ed28d9 | round1 REWORK: PS static 未代入・実経路未検証 → round2 で解消 (反証テスト + Runtime.exe スクショ) |
| sub-03 | OK | 2 | 4f7cbad | round1 REWORK: 前後関係未検証・SelfTest なし → round2 で解消 (Release/replay_verify PASS) |
| sub-04 | OK | 2 | 99654de | round1 REWORK: 切替で properties clear → round2 で解消。Create メニューのクリック確定は合成入力で未確認 (手動確認へ) |
| sub-05 | OK | 2 | (本コミット) | round1 REWORK: Deferred 透明段の実装漏れ・影スクショ・Inspector → round2 で解消 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- 作業ツリーに M79 無関係の未コミット変更が残る (AGENTS.md / README.md / assets/deepmodal/deepmodal.dmnet / tools/deepmodal/train.py と多数の未追跡)。**触らない・git add -A 禁止**
- Water プロジェクト側の絵作り (大波作り直し・ポストスタック・動画書き出し) は M79 完了後の別作業。M79 のスコープに含めない
- (planner 2026-09-24) spec 確定。AskUserQuestion が使えなかったので spec §7 に `[ユーザーに聞ける]` 5 件 (Deferred でも描く / VSMain 再評価 / WaterWave 差し替え / 時計 / PerMaterial オフセット)。逆を選ばれたら planner を REVIEW_RESPONSE か PLAN で呼び直す
- (planner) sub-01 は方式の成否判定を兼ねる。static 代入の再評価が成立しなければ sub-02 へ進まず planner へ差し戻すこと
- (planner) TAA は Deferred のみ (TaaPass.h:20-22)。M79 の速度・TAA 検証は必ず `--deferred` を付ける
- (司会) sub-01 coder の一時生成物 `wstrtest.obj` がリポジトリ直下に未追跡で残る。コミットしない → ユーザー承認を得て削除済み (2026-09-24)
- (sub-03 coder) 一時検証シーンの罠: `.mat.json.meta` の GUID は手で決めず、Runtime.exe を一度走らせて自動生成された値を読んでシーンに書く。Runtime 単体で登録済みのメッシュは `builtin://cube` のみ (quad/plane は未登録)。sub-05 でも同じ手順
- (sub-03) テスト作成の罠: 真上からの正射影ライトで Quad を使うと影の footprint が潰れる (Cube を使う)。変位量が大きいと read-back 画素がメッシュのスクリーン範囲からはみ出して誤検出する
- (sub-04, reviewer 向け) 切替の回帰テストは ApplyMaterialShaderSelection が名前代入のみのため実質自明。Inspector に clear を書き戻しても落ちない。Create メニュー「サーフェスシェーダ」の実クリック確定は未確認 (ユーザー/reviewer の手動確認)
- (sub-05) 自動化の罠: Editor.exe `--project` には `project.mye.json` が必須 (無いと MessageBoxW で停止)。`--screenshot` の撮影フレーム既定は 60、`--frames` がそれ未満だとエラーなしで PNG が出ない
- (reviewer 向け) sub-03 の空時早期 return を固定するテストなし / sub-04 の切替回帰テストは範囲が狭く Create メニュー実クリック未確認 / sub-05 の影スクショは影と N·L 陰影を区別できない (水面に物体の影を落とす配置で撮ると確認できる)
- (後続候補) 極端な座標 (1000,1000,1000) で CSM 描画異常の観測。ComputeCascadeVPs の頑健性調査 (未調査)
