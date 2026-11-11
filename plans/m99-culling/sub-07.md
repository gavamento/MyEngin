# sub-07: 画面サイズカリング

- 依存: sub-06
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.4 / 受け入れ条件 7 (と 1, 2 の画面サイズ部分)。
- 判定 (純関数): s = `BoxScreenSize(ワールド AABB, カメラ位置, proj._22, 正射影か)`、`lodBias` は掛けない。s < 閾値で落とす。前フレームにこのビューで画面サイズで落とした物は s < 閾値 × 1.1 の間は落としたまま。
- 履歴: ビュー別 (viewKey 1..3)、`LodHistory` と同じ作り (ステージ 2 は読むだけ、ステージ 3 で書く)、`ResetRenderHistory()` とサイズ変更で捨てる。
- 除外: `RenderCulling.exemptFromSizeCulling`、`alwaysVisible`。 `RenderCulling` は sub-06 と同じ解決 (自分 → 祖先の最初の 1 個) で引く。
- 掛けるビュー・影・フィット・RT の扱いは距離と同じ (sub-06)。判定順は 視錐台 → 距離 → 画面サイズ。`culledScreenSize` に数える。
- `--cull-min-screen-size F` で起動中だけ ON・閾値 F。
- スキンは URO と同じ全姿勢の箱を使う。

- **sub-06 round 2 からの持ち越し (should)**: `DistanceCull.h` の `ResolveMaxDrawDistance` はタグの継承を `Tags::OwnMask` を祖先まで OR して自前で書いている。`Tags.h` の「規則はここの 1 本きり」に合わせ、タグは `Tags::EffectiveMask(world, e)` を呼ぶ形にする (RenderCulling の祖先探索とは別の走査になってよい。門が開いているときだけのコスト)。DistanceCullSelfTest の階層テストがそのまま通ること。
- 除外フラグの門: `exemptFromSizeCulling` が意味を持つのはワールドに RenderCulling があるときだけで、そのとき sub-06 の存在の門は開いている。門に「画面サイズカリングが ON」を足す必要はない (門が閉じている = RenderCulling が無い = 除外なし)。

## やらないこと (このサブでは)
- メッシュ以外への画面サイズカリング

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Mesh\MeshLod.h` (`BoxScreenSize`、`SelectLod` のヒステリシスの書き方)
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (`lodHistory_` の隣に履歴、ステージ 2 / 3、`ResetRenderHistory` :1738)

## 受け入れ条件 (このサブ)
1. 単体テスト: 閾値の内外、ヒステリシスの帯 (閾値 ×1.0〜1.1 で前の状態を保つ)、除外の 2 種、履歴無し、リセット後
2. `--cull-min-screen-size 0.02` で既定ベンチの遠景の物が落ち `culledScreenSize` > 0、既定 (OFF) では 0 で `counts` が sub-06 後と一致
3. カメラを固定したまま連続フレームで撮っても落ちる集合が揺れない (dump を shot-frame 違いで 2 本)
4. `counts` が Debug / Release / `--warp` で一致、`--no-jobs` と一致

## 検証コマンド
- Debug / Release ビルド
- `MYE_SELFTEST_ONLY=<画面サイズのテスト>,RenderStats,distancecull,simsnapshot` で `Editor.exe --selftest`
- `tools\replay_verify.bat` (sub-06 の SimSnapshot v82 を含む状態での再確認)
- `Editor.exe --render-bench-demo --render-stats-dump --cull-min-screen-size 0.02` (Debug / Release / `--warp` / `--no-jobs`)
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
