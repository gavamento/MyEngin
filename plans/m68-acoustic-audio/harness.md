# harness 台帳: m68-acoustic-audio

- 依頼原文: M68: 音響伝播 × 実オーディオ (波面の 4 役目 = 遮蔽・回折ローパス・部屋の残響・鳴る波)。承認済みの計画 `plans\m68-acoustic-audio\plan.md` を正本に 3 サブ (M68a 場 + 遮蔽 + LPF / M68b 残響 + 鳴る波 + WAV 4 本 / M68c 仕上げ) で実装する。ユーザー決定 4 点 (WAV を焼いてコミット / 残響は 2 プリセット間の連続補間 / 3 サブ / harness) は確定済みで蒸し返さない。判断 1〜7 は Plan エージェントの設計レビュー (2026-09-06) を通した内容。TypeId は 50 (Cloth/SoftBody 予約は 51/52 へ)。planner が選択肢を出すなら「一発再生の追従整形」「到来方向の平均化」の 2 点だけ。sim 状態ゼロ・ABI v15 据え置き・replay 7 ペアと golden 22 枚は全サブで無風が受け入れ条件。
- 開始: 2026-09-06 / 基点コミット: 8e4272e985ecdfe655f9826816a6af6de1058474
- フェーズ: 実装 (2026-09-06 策定完了、sub-01 から)
- 元計画: `C:\Users\akita\.claude\plans\moonlit-forging-crown.md` (plan mode で 2026-09-06 に承認済み。写しを `plans/m68-acoustic-audio/plan-original.md` に置く。依頼原文の `plan.md` はこの写しを指す)
- 仕様書: `spec.md` (planner が育てる)
- 策定の往復: 0 (planner に `AskUserQuestion` が無い → 実コードで裏取りして裁定。spec §2 に S1〜S19、未決 2 点 (S16 / S17) に `[ユーザーに聞ける]` の印。2026-09-06)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | c29f7b3 | M68a: リスナー場 (Dial の 3 本目) + 遮蔽・回折の整形 (仮想発音位置 / LPF) + AcousticAudio (TypeId 50) + selftest 45 本目 + hum。依存: なし VERDICT round 1 OK (should 1 = shot_verify を 2 回追加実行して枚名を残す → reviewer 申し送り / nit 2 = 冗長な前方宣言・Debug probe 9.3ms)。M45 の既存不具合 (起動時 ApplyMixer が playOnAwake を殺す) を同時修正 |
| sub-02 | OK | 2 | 01183b1 | M68b: 部屋の残響 (2 プリセット連続補間) + 鳴る波 (PendingWaveShot) + 足音 WAV 4 本。依存: sub-01 round 1 REWORK (must 1 = openSmall 0.2→0.30 / should 1 = shotsPlayFailed / nit 1 = shot の src=) round 2 OK (nit 2 = Update の 4 仕事 200 行超 / shot の src= を文書へ → 申し送り)。golden フレークの正体 = 撮影中の生マウスデルタ (M65g 由来) → 恒久対策は sub-03 (S24 / A26) |
| sub-03 | 判定待ち | 1 | — | M68c: 仕上げ (ADR-017 / engine_spec §10.6 / README / test_checklists / CLAUDE.md / 進捗表)。依存: sub-02 |

## ユーザー判断
- (2026-09-06、harness 起動前に司会が AskUserQuestion で確認済み。planner は蒸し返さない)
- U1 デモの音源 → **SynthCore で焼いた WAV + `.sound.json` + `.meta` をコミット** (起動時合成・実録音は不採用)
- U2 部屋の大きさ → 残響 → **2 プリセット間の I3DL2 連続補間** (11 プリセットの段階切替は不採用)
- U3 サブ分割 → **3 サブ** (a: 場 + 遮蔽 + LPF / b: 残響 + 鳴る波 + WAV 4 本 / c: 仕上げ)
- U4 進め方 → **/harness (3 役)**
- (2026-09-06、planner の `[ユーザーに聞ける]` 2 点を司会が AskUserQuestion で確認。どちらも裁定どおり)
- U5 到来方向の平均化 (spec S16) → **最後の 1 歩のみ** (裁定どおり。仮想位置は smoothTicks で平滑済み、T7 が厳密に書ける)
- U6 一発再生の追従整形 (spec S17) → **Play 時 1 回・状態なし** (裁定どおり。既存 PlaySoundAt と同じ扱い)
- 設計レビュー (Plan エージェント、2026-09-06) で確定した修正: TypeId は 52 でなく **50** (登録順 = TypeId、Cloth/SoftBody 予約は 51/52 へ) / 箱の外は Bypass でなく **Occluded** / 仮想発音位置中は **dopplerScale = 0** / 波→一発再生は **`PendingWaveShot` POD を TickRunner の `!ts.resim` ブロックから push し `AudioSourceSystem::Update` で drain** / リバーブ override は **`ApplyReverbParams` の 1 箇所**で選択

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- (sub-01 round 1、planner) **M45 の既存不具合を M68a で直した**: 起動時 `ApplyMixer` の保留再構築がフレーム 0 の playOnAwake voice を殺す (spec S20)。`EngineLoop` メインループ直前の `audioSystem.Update(0.0f)` 1 行。コミット本文に書くこと
- (同) tick 1 の占有未ベイク (spec S21、M65a 継承、sim 側の性質) — M65 追補候補「占有ベイクを最初の transform 更新の後に」。M68 では触らない
- (同) `shot_verify.bat` が 1 回だけ「1 shot(s) differ」(枚名未捕捉)、直後 3 回連続 22/22 → **(sub-02 round 2 で解決)** 正体は `acoustic_deferred` (maxDiff 125 / 520 px、Watcher の箱): `WatcherFpsCamera` が生マウスデルタ (`WM_INPUT`) を yaw に積分するので**撮影中に机を触ると割れる** (M65g 由来、M68 無関係)。恒久対策 = sub-03 の 1 行 (`deterministicShot` で生デルタを 0、spec S24 / A26)。それまでの reviewer は撮影中にマウスを触らないこと
- (同) Debug の probe 再構築 9.3 ms/回 (Release 0.65) — v1 許容。後続で `assign` の毎回確保をやめる余地 (spec §4.4)
- (同) nit: `AudioSourceSystem.h` の `class AcousticField;` 前方宣言は `AcousticAudio.h` 経由で実体が見えるので冗長 (害なし)
- (同) sub-02 へ: `openLarge` 0.6 → 0.8 (spec 変更履歴 #7)。廊下の `open=` 実測を報告させる → (sub-02 round 1) 報告あり、`openSmall` 0.30 を round 2 で適用 (spec S23 / #10)
- (sub-02 round 1、planner) **ADR-017 の実測値** (M68c が写す): 開放度 = 部屋 A 隅 0.468 / 中央 0.668、横廊下 西 0.496 / 中 0.357 / 東 0.287、縦廊下 0.404・0.529、部屋 B 戸口 0.607 / 中央 0.800 (`roomProbeM` 6)。既知の制限: この指標は「部屋の隅」と「廊下の端」を区別できない (局所の自由体積しか見ていない)。probe 0.61〜0.65 ms (Release、16224 セル)、shots 51 / 600 tick (合成入力)
- (同) M68c の test_checklists / ADR に「壁越しの hum は Detour (lpf 床 0.25) であって Occluded ではない — Occluded は密閉と経路上限超えだけ」を書く (A11 の予測が 0 行だった根拠)
- メモリ (`myengine-project.md`) の現在地更新はリポジトリ外なので司会が行う (M68c の coder は触らない)
- (sub-03 round 1、planner) **M68 の残り = ユーザーの耳** (`docs/test_checklists.md` の M68 節 30 項目) と調整値の焼き込み (`bendFullM` / `lpfFloor` / `occludedGain` / `detourWet` / `waveVolume` / `openSmall 0.30` / `openLarge 0.8`)。Inspector で実行中に触れる (NoHash)
- (同) nit 3 件: (1) `[shot] deterministic capture:` のログ行に「+ raw mouse delta zeroed (--shot-realtime で解除)」を足す (`EngineLoop.cpp:686`、次に触るとき) (2) ADR-017 の「決定 0」は M65 の ADR を立てるならそちらへ (3) README の CLI ブロックに `--acoustic-demo` / `--acoustic-audio-log` が無い (M65 からの漏れ。`--joint-demo` の隣に「= replay ペアの 7 本目 + スクショ 16/17 枚目」の形で)
- (同) **撮影モードの 2 行 (`EngineLoop.cpp:1163`) を守る回帰テストは無い** (`EngineLoop::Run` の中でヘッドレスから呼べない)。golden がまた時々割れたら、まずこの 2 行が消えていないかを見る。実マウス (WM_INPUT) での再現は未実施 (前面を取れない環境だった) — マシンが空いたときに `acoustic_deferred` の撮影中にマウスを動かして maxDiff=0 を 1 回確かめるとよい (任意)
- (同) reviewer へ: sub-03 のコード差分は `EngineLoop.cpp` の 2 行 + コメントだけ。A26 (c) は変更前バイナリとの 650 行バイト一致で取れている (合成入力のデルタは生きている)
- 案 4 (XPBD 布・ソフトボディ、M60'e〜) は 2026-09-06 20:00 にセッション内リマインド (harness とは無関係)
