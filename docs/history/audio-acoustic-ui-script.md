# 音声・音響・UI・スクリプトホストの経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## Audio/AcousticAudioSelfTest.cpp — kSimSnapshotVersion の変遷 (T1)
- (M68a) AcousticAudio は kComponentNoHash なので snapshot の版を上げていない。T1 は版を `==` の値で持ち、他の理由で版が上がるたびに値を追随させてきた (`>=` にすると「AcousticAudio のせいで版が動いていない」という主張が消えるため)。
- (M70b / M70c) InputSnapshot 72 -> 88 と Scene 節の UI 対話状態で 11 -> 13。
- Light.safeRadius の生バイト追加で 13 -> 14。
- (M71a) Scene 節の sceneName 追加で 14 -> 15。ABI v17 GetSceneName がシーン名を sim の分岐材料にしたため。
- (M18 追補) SkinnedMesh の loop / fadeTicks / クロスフェード状態の生バイト追加で 15 -> 16。
- (M75b) InputSnapshot 112 バイト + UI 対話状態のドラッグ欄で 16 -> 18 (17 は欠番)。
- (M65i) AcousticVolume.glowAlbedoMix (残光に面の色) の生バイト追加で 18 -> 19。
- AcousticVolume.glowDecayEveryTicks (残光の間引き) の生バイト追加で 19 -> 20。
- AcousticField::kMaxWaves 16 -> 32 (ACU 節の本数) で 20 -> 21。

## Acoustic/AcousticField.cpp / AgentSystem.cpp — 波の枠 16 -> 32 と敵の声の優先
- (2026-09-14、三校) kMaxWaves を 16 -> 32 に増やした。金属床を走る足音と追跡中の敵 3 体の声で 16 本が埋まり、敵の声が捨てられて「敵の位置が見えない」が起きうるため。同じ日に Emit の agentPriority を足した。敵の声は満杯でも、敵ではない発音元のうち最も古い波を追い出して立てる。kSimSnapshotVersion は 20 -> 21。

## Acoustic/AcousticDebugDraw.h / AcousticDebugDraw.cpp — デバッグ線の位置付けと音色の色
- (M65b) デバッグ線は伝播と同じサブで入れた。M65d で残光ボリュームが GPU に上がるまでは、波が壁を貫通せず角を曲がったことを人間が確かめる手段がこの線しかなかったため。
- (M65b) 音色 0..3 の 4 色は、M65e のライティングでも同じ色を使う予定で、ここを正本にした。実際の残光ボリュームは 1 セル 1 バイトで音色を持たない形になったので、音色の色が出るのはデバッグ線だけになった。
- (M65b -> M65f) 聴者の表示は、M65f でリスナーの鏡 (lastHeardPos) が入るまで十字だけだった。

## Audio/ImpactSynth.cpp / ImpactSynthSelfTest.cpp — 試聴で直したプリセット
- (2026-09-12 試聴) 「全体的に高音」と言われたので、基本周波数を 2/3 前後に下げ、brightness を少し絞り、bodyAmount (低域の胴鳴り) を足した。計画 §9 の初期値より一段暗い。
- (2026-09-12 試聴 2 回目) 三校の 14 本 (assets\audio\impact\*.impact.json) の brightness を 1.0 -> 0.7 にした (「まだ少し高い」)。ダンプ表の kDumpBrightness も同じ値。
- (試聴 3 回目「ガラスの音がおかしい」) Glass は resonanceDecay 14 で 0.3 秒鳴り続け、グラスを弾いた「ピーン」になっていた (純音 3 本だけで hi>4k のノイズが 0)。瓶や板ガラスの衝突は「カツン」なので 45 / noise 0.55 にした。
- (試聴 3 回目) GlassBreak の破片は旧値 (減衰 20-100/s、開始 0-250ms に一様) だと長い純音が 150ms 以降に積み上がり、crack より後のほうが大きい「ピロロロン」になっていた。減衰を 60-250/s にし、開始を r² で crack 側へ寄せ、「シャラ」は別枝の明るいノイズの尾で描くようにした。ImpactSynthSelfTest がこの異常の再発を固定している。
- transient の LPF は最初 1 極 (6 dB/oct) で alpha 0.9 だった。ほぼ白色ノイズで、何を踏んでも「シャッ」になっていたので 2 極 (12 dB/oct) にした。

## UI/UIInteraction.h / UIRenderer.h — 押下判定をエンジンへ移した理由 (M70c)
- (M70b まで) 押下判定は UIRenderer の中にだけあり、ハイライト表示のためだけに計算して捨てていた。ゲーム側で動く経路は、UIElement と同じ矩形をスクリプトにもう一度手書きしてマウス座標と比べる (`UIButtonDemo`) しかなかった。そのため「絵の上でのハイライト」と「ゲームが押したと思う要素」が別々の判定になっていた。
- (M70c) 判定を UIInteractionState (Scene の sim 状態、WorldHash 対象) に移し、描画はそれを読むだけにした。

## UI/UIInteraction.h — ウィジェット用の欄を M75b でまとめて足した理由
- (M75b) changed / pressSurfX,Y / prevSurfX,Y / dragging は、M75f〜h のウィジェットで使い始める欄だった。SimSnapshot / .rep の版の bump を M75b の 1 回にまとめるため、先に全部足した (後から足すと版がもう一度動く)。M75b の時点では changed を立てる者がいなかった (今は UIWidgets.cpp が立てる)。
- (M75b) ドラッグ座標をキャンバス座標で持たなかったのは、M75c でキャンバスが複数になると倍率が要素ごとに違うため。

## UI/UILayout.h / Script/EngineApiTable.cpp — マウス座標の記録 (M70b -> M75b)
- (M70b) EngineLoop が CanvasSize(実寸) を解き、Input::CaptureSnapshot が `float(mouseX) / scale` をキャンバス座標として記録していた (mouseCanvasX)。
- (M75b) 記録をゲーム面 px + 面の寸法に変え、除算を sim 側の CanvasOfInput + SurfaceToCanvas へ移した。式を 1 回の除算のまま保ったので、既定キャンバスでは M70b の記録値と同じビットになる (UISelfTest の (i) が固定)。MouseCanvasPos もこの換算を通る。
- (M70b) MousePos のコメントに「キャンバス座標のマウスは M70c で足す」と書いていた。M70c で MouseCanvasPos (ABI v16) が入った。
- (M70b 以前) BuildSimWorldContext の aspect は sim だけ 1920x1080 固定だった。非 16:9 ではワールド追従 UI の射影が sim (ヒットテスト) と描画で横方向にずれていた。M70b からキャンバス寸法を渡す。
- (M75c) CanvasOfInput の隣に CanvasDesc を取る版を並べる予定だったが、作られていない (CanvasDesc を取るのは CanvasSize)。

## UI/UILayout.h — キャンバスのモード (M75c)
- (M75c) Shrink (max) と Match Width Or Height を足した。Expand の式は M70b から 1 ビットも変えていない (16:9 の golden 4 枚の不変はこの式に掛かっている)。

## UI/UILayout.h / UILayout.cpp / UIRenderer.cpp / UIInteraction.cpp / Script/EngineApiTable.cpp — 新機能を恒等経路で入れた
- M75a (RectTransform / 回転・スケール)、M75c (複数キャンバス)、M75e (Layout Group / ContentSizeFitter)、M75f (Selectable の遷移 / Slider) は、その機能を使わない要素がそれ以前とビット同一になるように恒等ゲートで入れた。xform の無い要素のヒットテストは ResolveVisibleRect と同じ式、Canvas の無い要素は倍率 1.0f、Group / Fitter / Slider の無い要素は RectFromTransform(rt) だけ、ウィジェットの無いシーンの頂点色は el.color の複写。golden と既存シーンの不変はこれに掛かっている。
- (M75a) RectTransform を持たない要素 (スクリプトが UIElement だけ AddComponent した等) は、旧 UIElement と同じ既定値 (左上・pivot 0・160x40) で解く。そのため M75a 以前と同じ絵になる。

## Script/EngineApiTable.cpp — PlaySound の戻り値
- v3 の PlaySound は、以前はキュー index を返していた。tick を跨ぐと別の再生と同じ値になり、停止対象が衝突した。今は tick を跨いで一意なハンドルの下位 31bit を返す。int へ潰すと 2^31 再生目以降でまた衝突しうるので、停止には v8 の StopVoice(uint64) を使う。

## Script/ScriptHost.h — Start 済み記録の型 (M64b)
- (M64b) started_ を unordered_set から std::set に変えた。unordered_set のときは反復順がハッシュ依存なので、snapshot の書き出し側で昇順に整列する約束だった。
