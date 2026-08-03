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
