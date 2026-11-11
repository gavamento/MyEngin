# sub-07: 画面サイズカリング

- 依存: sub-06
- 状態: OK (コミットは司会)
- 往復: 1

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

SELF_EVAL: sub-07 (round 1)
- 実装: DistanceCull.h `CullsByScreenSize` (純関数、ヒステリシス `kSizeCullHysteresis` 0.1) を追加、`ResolveMaxDrawDistance` のタグは `Tags::EffectiveMask` 呼び出しへ (祖先走査は RenderCulling のみ)。RenderSystem に `sizeCullHistory_[4]` (LodHistory 流用、`ResetRenderHistory` で Clear)、CullCand に `exemptSize` / `tooSmall`。ステージ 1 で門 (RenderCulling またはタグ別距離) が開いたとき exempt を解決、ステージ 2 で BoxScreenSize (lodBias なし、全姿勢の箱) 判定、ステージ 3 で履歴書き込みと `culledScreenSize` 計上 (視錐台 → 距離 → 画面サイズ)。落とした物は影候補にも入れない。
- 差分: [追加] 履歴は視錐台・距離で落ちた物にも書く (画面サイズ判定は理由の順に関わらず物の状態として一貫させるため)。`LodHistory` を型名のまま再利用 (新クラスを作らない)。
- 検証: Debug / Release ビルド成功。selftest (distancecull, RenderStats, simsnapshot) 3 suite 0 failed。`--render-bench-demo` の counts: 既定は sub-06 後と同値 (drawCalls 23 / culled 308 / candidates 3686 / culledScreenSize 0)。`--cull-min-screen-size 0.02` は culledScreenSize 1894・candidates 1781・triangles 1335104→597956。Release/Debug (--warp)、--no-jobs、Release GPU、shot-frame 61 の 6 本の counts が完全一致。replay_verify PASS (24 jobs)。check_rules 0 error。
- 自己採点: 仕様適合 4 (単体のみで、除外フラグ付きシーンの実描画は未確認) / 正しさ 4 / コード品質 4 / テスト 4 (判定関数と履歴の単体。RenderSystem 経由の画面サイズ + 除外は未テスト)
- 不安・質問: なし。`--screenshot` での画面目視・Scene View トグル経由は未実施。
- 触ったファイル: src\Engine\Engine\Rendering\DistanceCull.h / DistanceCullSelfTest.cpp / RenderSystem.h / RenderSystem.cpp

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (単体 10 件: 閾値の内外・ちょうど・帯の保持と復帰・除外 2 種・閾値 0・NaN・履歴なし・Clear 後)、2 (`--cull-min-screen-size 0.02` で culledScreenSize 1894、既定は 0 で counts が sub-06 後と一致)、3 (shot-frame 60 / 61 の counts 一致)、4 (Debug / Release / WARP / GPU / `--no-jobs` の counts 一致) を確認。sub-06 から持ち越した should (`Tags::EffectiveMask` を呼ぶ) は解消。replay_verify PASS。[追加] 3 件は採用 (spec 4.1.4 に記録)。shadowCascadeCasters が変わらないのは、0.02 未満になる遠景が CSM の最遠カスケードの外にあるためと読める (候補 3686 → 1781 は画面外リストからも除いた結果) — 矛盾なし。持ち越し: 除外フラグの RenderSystem 経由の確認は sub-08 (should)、counts の比較を目視でなく機械で行うのは sub-09 (nit)。
