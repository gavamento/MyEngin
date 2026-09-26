# review-2 — m80-destruction

- 対象: `73c8d76..HEAD` (d5c1dbf)。重点は修正コミット 7d3cec3 / 16ab02a / d5c1dbf。f17ad63 (M76) は除外
- 日付: 2026-09-26
- 検証環境: WIP 抜きの worktree `C:\HAL\mye_rv80_r1_1454` を d5c1dbf へ更新し、Debug/Release をビルドし直した。一時プローブはこの worktree にだけ入れ、実行後に revert した (差分の写し: `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\bc8aa3e8-1cc1-44bf-aefb-a18425fbda6f\scratchpad\review_probe_r2.diff`)。本体の作業ツリーには触れていない

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — 焼く → 保存 → 新しいセッション → 最初の物理 tick より前に凸包が登録され、割れる (プローブ E2 hulls=48/48 broke=1)。同じ名前の別物は別ファイル、同じ入力は同じファイル (E1)。取り消しの遅れは最大 1.7s。残りは、強度が質量に依らない絶対値なので、既定値のまま重い物 (40kg 以上) を置くと tick 0 に自壊する件 (指摘 1、minor)
  機能性: 5 — replay_verify 14 job 全 PASS。Debug/Release の --selftest は exit 0、PASS 5258 件は同数、digest も一致。shot_verify は既知の 4 枚だけ FAIL (fracture_before/after を含むほかは PASS)。受け入れ条件 21〜26 はそれぞれ SelfTest とプローブで確認した (下記)
  ビジュアルデザイン: 4 — 浮かせた壁は下寄りに穴が開き、外れた塊が床に落ち、残りは宙に固定されたまま見える (r2frac_45/120/300.png)。断面の色も出ている。デモの箱は球が当たる前に着地で割れる (指摘 2)
  コード品質: 4 — キーの表現を GUID と内容ハッシュにそろえ、判定 (FracturePieceIndicesMatchAsset) と解決 (ResolveFractureAsset) を FractureSystem と Inspector で共有した。LocalTransform の遅延書き込みは TickRunner の 1 か所に明示され、MYE_CHECK で呼び忘れを検出する。テストも各指摘に 1 対 1 で付いている
指摘:
  1. [minor] 宛先: planner — strength が質量に依らない絶対値なので、既定の 70N のままでは、40kg 以上の Destructible を床に置くだけで tick 0 に崩れる。1kg の Destructible に 5kg の箱を載せても崩れる。警告は出ず、ツールチップの英文 1 行 ("heavier objects need a higher value") だけ — 根拠: プローブ A (16 破片、1m の箱、既定 strength、床あり、600 tick 静置) の結果は次のとおり。1/5/10/20kg は割れない。40kg は tick 0 に分離 8、80kg は tick 0 に分離 13。1m 落下では 5kg 以上がすべて割れる。プローブ B: 1kg の Destructible に 5kg の箱を載せると tick 129 に分離 4。spec §2 の較正基準 (review-1 行) は 1kg だけで決めていて、spec が避けたかった「置いてあるだけで割れる」(決定 5 / Q-4 の理由) が質量次第で起きる — 期待: ユーザーに上げるかは planner が判断する。案は (a) 強度を「ルート質量あたり」にするか、既定の強度に質量を掛ける、(b) Inspector に「この質量では静置で割れる見込み (m·g/接地破片数 > 0.25·strength)」の警告を出す、(c) 現状のまま文書化する、のどれか。v1 で (c) を選ぶなら engine_spec §10.8 に 1 行書く
  2. [minor] 宛先: coder — デモの箱 (8kg、高さ 4m から落下) は、既定の strength では着地で割れる (球より先)。engine_spec §10.8 と replay の説明にある「cannon balls detach chunks from a rolling box」と、DemoContent のコメント「着地した後に球をぶつけると割れる」が実際と合わなくなった — 根拠: プローブ C (同じ質量・高さ・8 破片、球なし) で tick 50 に分離 7。r2frac_45.png では、着地した箱に球が届く前後から継ぎ目が出ている — 期待: デモの箱だけ strength を戻す (割れる前は 1 剛体として転がることを見せる、というデモの目的に合わせる) か、コメントと文書を実際の挙動に合わせる
  3. [minor] 宛先: coder — 保存名のハッシュ (ComputeFractureBakeInputHash、src/Editor/FractureBakeCommit.cpp) に、スキンの入力 (skinVertices のウェイトと skinJoints の inverseBind) が入っていない。形が同じでウェイトか骨が違うスキンメッシュを同じエンティティ名で焼くと、同じファイルに上書きされる。先に焼いたほうの子 (PartComponent の骨名、骨空間の破片の姿勢) が、次のセッションでは別の骨空間の中身と組み合わさる — 根拠: 同じ関数のハッシュ対象は sourceMesh の verts/indices、seed、pieceCount、openMeshMode、voxelResolution、kFractureBakeVersion だけ。骨空間への変換 (AssignFractureBonesAndTransform) は焼きの出力を書き換える。実行による再現はしていない (コード上の事実。起きるのは、同じ名前・同じ形・違うスケルトンという狭い条件のとき) — 期待: skinVertices と skinJoints (名前と inverseBind) もハッシュに混ぜる
検証した手段:
  - worktree を d5c1dbf へ更新し `tools\replay_verify.bat` を実行 (Debug/Release の再ビルドを含む) → 14 job 全 PASS
  - `Editor.exe --selftest` を Debug と Release で同時に実行 → 両方 exit 0 (275s)。PASS は両方 5258 件。fracture digest は lshape 0x521C987A83D781F7 / torus 0xD9B0EEB57B686B71 で一致。新しい検査 (session / duplicate names / neighbor cap / fracture asset の境界 / member reparent / asset cache / non-uniform shrink / mid-tick pose / strength default / separation invariant / same-bone pieces / cancel / shutdown / weight cache) がすべて PASS
  - `tools\build_managed.bat Release` の後に `tools\shot_verify.bat` → FAIL は既知の 4 枚 (parts / joints / acoustic_forward / acoustic_deferred) だけ。fracture_before / fracture_after は PASS
  - `Runtime.exe --fracture-demo` を frame 20 / 45 / 60 / 120 / 300 で撮影して目視 (`scratchpad\r2frac_*.png`)
  - 一時プローブ `--m80r2` (Release): A 質量 × 静置 / 1m / 3m、A2 2m の箱、B 積み重ね、C デモの箱の着地、D 非一様スケールの縮み (2,1,3) → (1.5,0.75,2.25) → (1,0.5,1.5) → (0.5,0.25,0.75) → 破棄、E1 同じ名前 3 体 (ファイル 2、a≠b、a==c、板のハンドルは板の中身)、E2 本物の AssetDatabase で保存 → 新しいセッション → PreloadFractureAssets (resolved 3/3、hulls 48/48、割れる)、F 解像度 72 の焼きの取り消し (0.3s / 3s / 8s 後に取り消し → 遅れ 0.20s / 0.53s / 1.66s で cancelled=1)。ログは `scratchpad\r2probe.log`
  - 修正 3 コミットの diff を全部読んだ (FractureSystem / FractureBuilder / FractureLibrary / FractureAsset / FractureBake の CapNeighborsSymmetrically と取り消し / FractureBakeService / FractureBakeCommit / InspectorWindow / TickRunner / EditorApp / RuntimeMain / Components / Localization / DemoContent / ADR-021 / spec / bench.md)。TickRunner の Update から ApplyStructuralChanges・ApplyDeferredLocals の間に早期 return が無いことも確認した
  - 「仕様との差分」に無い変更は見つからなかった (#7 の配置の逸脱と、メンバー位置ずれの修正は、司会の補足とサブの記録に記載がある)
前回指摘の消込:
  1. 解消 — GUID を保存し、Editor の起動 / シーンの切り替え / Runtime / TickRunner のシーン遷移で PreloadFractureAssets を呼ぶ。プローブ E2 と SelfTest の session 系で、新しいセッションでも登録済み・一致表示・割れることを確認
  2. 解消 — 保存名を入力のハッシュにし、Commit で ReloadFromFile する。E1 で同じ名前・違う中身は別ファイル、同じ中身は同じファイル。スキンの入力がハッシュに無い件は新規の指摘 3 (round 1 は非スキンの経路しか見ていなかった)
  3. 解消 — 係数を掛ける方式に変えた。D で非一様スケールの比が保たれ、飛ばない
  4. 解消 — localCenter (破片ローカルの体積重心) で測る。SelfTest の same-bone pieces と、コードで確認 (ApplyFractureDamage と onBreak の point の両方)
  5. 解消 (仕様どおり) — 既定は 70N。1kg の 3 基準は SelfTest とプローブ A で満たす。質量への依存は新規の指摘 1 (round 1 は 1kg 相当のケースしか見ていなかった)
  6. 解消 — Inspector の上限を 72 にし、取り消しと Shutdown での打ち切りを入れた (F で遅れ最大 1.7s)。80 以上の根本原因は後回しで、ADR-021 と spec §7 に記録。Q-15 はユーザーの判断待ち
  7. 解消 — 浮かせた壁の下寄りに穴が開き、塊が落ちる (r2frac_45/120/300.png、golden を撮り直し)。Q-14 はユーザーの判断待ち
  8. 解消 — 判定を DestructiblePiecesMatchAsset (broken を考慮) にした。SelfTest の "still reports a match after breaking"
  9. 解消 — キーを fractureAsset.value にした。SelfTest の asset cache
  10. 解消 — (fid, srcPath, meshKey, ReloadCount) でキャッシュ。SelfTest の weight cache
  11. 解消 — ApplyDeferredLocals で tick 末に書く。SelfTest の mid-tick pose と separation invariant
  12. 解消 — インデックス・隣接先・自己参照・非対称を検査する。SelfTest の fracture asset 系
  13. 解消 — CapNeighborsSymmetrically にし、kFractureBakeVersion=2。SelfTest の neighbor cap
```
