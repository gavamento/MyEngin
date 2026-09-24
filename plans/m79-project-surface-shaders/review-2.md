# review-2 — m79-project-surface-shaders

- 対象コミット範囲: 3b55f4a..a1f52ce (round 2 の修正分は a08d8d4..a1f52ce: 169ba2f / e3e1b80 / a8dd8ef / 290543d / bfe8581 / a1f52ce)
- 日付: 2026-09-24
- 一時検証物: `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\` の `rv1\` (round 1 の画像は `rv1\r1\` へ退避)・`rvw\`・`ed\`

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の混在シーンの破綻 (影の欠落・instanced の消失) は全部解消した。変位で AABB の外へ出る形は boundsPadding で写り (rv1\liftPad.png, liftPad_def.png)、余白 0 なら従来どおり消える (liftA.png)。doubleSided で内側から裏面が描かれる (ds2.png / ds2_def.png。対照の ds1.png は空だけが写る)。Water のコピーでも水面は黒く潰れない (rvw\r2_taa.png)。残りは minor 1 件 (影エントリでサンプラが未バインド)
  機能性: 4 — 受け入れ条件 13・14・15 を round 1 の再現シーンで再撮影して確認した。Debug の --selftest は exit 0 で FAIL 0 (review-1 #2 #4 の新規テスト、boundsPadding / doubleSided の読み込みテストを含む)。check_rules は 0/0。Release・replay_verify は司会の通し結果 (13/13) による。Inspector の実 GUI は未確認 (下記)
  ビジュアルデザイン: 4 — rv1\g_fix12.png (shadowA / shadowB / shadowA_def / fail / fail2 / vtexA / vtexA_def) で、forward_lit・フォールバック・instanced の影とジオメトリが描画順に依らず出る。rv1\g_lift.png、rv1\g_ds.png も意図どおり。WaterGerstner は斜め視点で環境光色が一面を覆う (絵作りの範囲なので指摘にしない)
  コード品質: 4 — 復元処理は Forward / Deferred / ShadowPass の各パスに置かれ、コメントで理由を書いている。カリングの余白は直列ステージで解決していて、並列の前提を守っている。PerMaterial は cbuffer 名で絞った。スキーマキャッシュはファイル更新時刻で無効化する。回帰テストも付いた。影エントリだけ予約サンプラを張っていない (#9)
指摘:
  9. [minor] 宛先: coder — ShadowPass の影エントリは予約サンプラ (gSampler 等) をバインドしない。VSMain でテクスチャを読むサーフェス (高さ・ノイズで波を変位) の影は、D3D 既定のサンプラ (CLAMP) で評価される。色・速度エントリは gSampler (WRAP) で評価されるので、UV が 0..1 を出るタイル状のノイズでは影の形だけが色と食い違う (spec §1「影ずれが出ない」の抜け) — 根拠: src/Engine/Renderer/ShadowPass.cpp の影エントリのブロックは BindSurfaceNamedCB / SRV だけで、Sampler の呼び出しが無い (grep "Sampler" でヒット 0)。rv1\vtexA.err と vtexA_def.err に `[d3d] DrawIndexed: The Vertex Shader unit expects a Sampler to be set at Slot 0, but none is bound` が 3 回出る (round 1 の rv1\r1\vtexA.err にも同じ 3 回) — 期待: 影エントリでも gSampler / gIblSampler 等を名前で張る (ForwardPath::DrawSurfaceItem と同じ 3 本)。round 1 で見落とした理由: error 行 (instance SRV の型不一致) だけを追い、同じログの WARN 行を読み落とした
検証した手段:
  - HEAD a1f52ce で MSBuild Debug|x64 (exit 0)。bin\x64\Debug\Editor.exe --selftest → exit 0、FAIL 行 0。tools\check_rules.ps1 → 0 error / 0 warning。Release / replay_verify は司会の通し検証 (Release の selftest は exit 0、replay_verify は 13/13) を採用し、自分では再実行していない
  - diff を全部読んだ: a08d8d4..a1f52ce の src / assets / docs / spec の差分 (ForwardPath / DeferredPath / ShadowPass / FrustumCull / RenderSystem / GpuResources / ShaderManager / ProjectShaderProperties / InspectorWindow / AssetOps / LocalizationTable / WaterGerstner)。SelfTest は抜粋
  - round 1 の再現シーンを HEAD の Runtime.exe --warp で撮り直した (Forward と --deferred): shadowA / shadowB / shadowA_def / fail / fail2 / vtexA / vtexA_def (VS SRV の型不一致エラーは消えた。残りは #9 の WARN だけ)。新しく liftPad / liftPad_def / liftPadShadow / liftNoPadShadow (boundsPadding 9) と ds1 / ds2 / ds2_def (カメラを 6m 立方体の内側に置き、doubleSided の有無で比較) を撮った。Water のコピーを deferred + TAA で撮り直した (rvw\r2_taa.png)
  - Inspector の実 GUI (sub-04 #5 の「Properties を足すと新しい欄が出る」と、sub-06 の boundsPadding / doubleSided 欄): フォーカスを奪わない方法として、PostMessage で WM_MOUSEMOVE / WM_LBUTTONDOWN を Editor のウィンドウへ送って操作を試した (ed\click.ps1、ed\shots2\)。ImGui はこのクリックを受け付けず、アセットを選択できなかった。**未確認**のまま残る。代わりに次を確認した: PropertySchemaCache の SelfTest (更新時刻で再パース、A→B→A の切替、ファイル削除と復活)、MaterialEditToJson が boundsPadding / doubleSided を書き出すこと (コード)、InspectorWindow.cpp の DrawMaterialInspector の欄 (コード)
  - 未検証: 実 GPU (すべて WARP)、doubleSided の影が両面で落ちること (画像では未確認。コードで Cull None の設定と戻しを確認しただけ)、Inspector の GUI 操作、sub-03 の空時の早期 return を固定するテスト (round 1 から引き続き無い。実地のビット一致は round 1 で確認済み)
前回指摘の消込:
  1. 解消 — rv1\shadowA.png で forward_lit 2 個の影が出る (round 1 では消えていた)。shadowB.png と同じ絵になった。Deferred (shadowA_def.png) も同じ。失敗マテリアル 2 個の fail.png / fail2.png は、両方のキューブが影を落とす。ShadowPass.cpp の restoreFixedShadowSlots (b0 / t0 / rasterizer) で直っている
  2. 解消 — rv1\vtexA.png で床と instanced のキューブ 6 個が描かれる。VS SRV の型不一致エラーは消えた (残る [d3d] は #9 のサンプラ WARN)。ForwardPath.cpp の restoreForwardLitBindings が VS t0 を戻す。SelfTest にも「VS テクスチャ付きサーフェス ↔ instanced run、順序入れ替え」の 3 件がある
  3. 解消 — spec §4.1 に boundsPadding (カリングと CSM フィット) が入り、受け入れ条件 14 が付いた。liftPad.png / liftPad_def.png では余白 9 で持ち上がった板が写る。余白 0 の liftA.png は従来どおり消える (仕様どおり)。RenderSelfTest に余白付きカリングの単体テストがある
  4. 解消 — GpuResources.cpp の collect が cbufferName == MyEnginePerMaterial で絞る。SelfTest「review-1 #4: MyEnginePerMaterial の CB サイズは自身の宣言 (32 バイト) どおり」が PASS
  5. 解消 (コードと SelfTest。GUI は未確認) — PropertySchemaCache が更新時刻で無効化する。マテリアル・fxstack の両方のキャッシュを置き換えた。AssetOpsSelfTest の 6 件が PASS
  6. 解消 — WaterGerstner に Fresnel × gAmbient の項が入った。rvw\r2_taa.png で水面は黒くない (round 1 の rvw\taa2.png は黒かった)。テンプレートにも gAmbient が入った
  7. 解消 — doubleSided を追加した (Forward / Deferred のサーフェス段と透明段 / ShadowPass が Cull None にし、描いた直後に戻す)。ds2.png / ds2_def.png で裏面が描かれる。SV_IsFrontFace を渡さないことは spec の後回しとテンプレートのコメントに明記された
  8. 別件 (planner 決定) — spec §7 に「M79 の回帰ではない、golden の撮り直しは別件」と記録された
```
