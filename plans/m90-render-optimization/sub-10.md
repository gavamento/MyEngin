# sub-10: review-1 の指摘を直す (統計の読み戻しの待ち、カット直後の LOD の画素差、小さい残り)

- 依存: sub-08 (全サブのコミット後)
- 状態: OK (コミット待ち)
- 往復: 1
- 出所: `plans/m90-render-optimization/review-1.md` の #1 #2 #4 #5 #6 #7 (coder 宛て)。#3 (planner 宛て) は下の「既定 ON の判断」に従う。

## やること
1. (review-1 #1) オクルージョンの統計の読み戻し (`OcclusionCullPass.cpp:442-460` の `ReadStats`、`Map(..., D3D11_MAP_READ, 0, ...)`) が GPU を待っており、ON の `cpuMs.gbufferSubmit` が解像度に比例する。spec §4.1.1「読み戻しを待たない」に反する。
   - 対話の描画 (エディタ、Runtime の通常のプレイ) では `D3D11_MAP_FLAG_DO_NOT_WAIT` で読み、まだ終わっていなければ前の値を残す (`DXGI_ERROR_WAS_STILL_DRAWING` はエラーとして扱わない)。
   - **決定的な撮影 (`--render-stats-dump` / `--screenshot` の撮影モード) では今までどおり待つ。** 理由: dump の counts (occluded / phase1 / phase2) は受け入れ 2 で「Debug / Release / WARP で一致」をゲートにしている。待たないと、値が GPU の進み具合で揺れる。どちらで読むかは RenderSystem の設定 (例: 決定的撮影の既存フラグ) から渡す。経路ごとに別の実装にはしない。
   - `ReadStats` の呼び出しを、`gbufferSubmit` / Forward 不透明の CPU 計測区間の外へ出す (撮影モードで待つ分が提出コストに混ざらないように)。
   - 3 解像度 (320x180 / 1920x1080 / 3840x2160) の実 GPU で ON / OFF の `cpuMs.gbufferSubmit` を測り直し、ON が解像度に比例しなくなったことを示す。ADR-029 の計測節と判断表 (ADR-029:261-266 付近) を書き直す。
2. (review-1 #2) カメラカットのフレームで ON と OFF が 11 画素違う (Forward maxDiff 47 / Deferred 49、最悪の画素 (365,326)、`--render-bench-cut-frame 30 --shot-frame 30`)。reviewer の観測: `--lod-force 0` では差 0、`--lod-force 1` では同じ 11 画素が違い、**ON の画素は LOD0 で描いた絵と一致する**。この観測からは z-fight より「ON の経路のどこかで、カットのフレームだけ LOD0 の範囲で描いている (indirect 引数の indexCount / startIndex か、フェーズ 2 の run の段、影のキャスターの段)」が疑わしい。原因を断定してから直す。
   - 取り違えなら直す。selftest に「LOD 付きのメッシュをカット直後のフレームで ON / OFF 全画素一致」を足す (`OcclusionSelfTest` の `TestPathAB<Path>` を LOD 付きの項目で回すのが最小)。
   - 本当に z-fight (同じ深度の交線で描画順だけが変わる) と断定できたなら、その証拠 (深度の値、描く順) を示し、ADR-029:295 の「画素差は 0」を正確な記述に直す。selftest は同じく足す。
3. (review-1 #4) `Menu_Occlusion` (`LocalizationTable.inl:88`) の「(Deferred)」を両言語から外す。
4. (review-1 #5) `ResolveOcclusionCulling(assetsRoot, cliFlag)` を作り、`EngineLoop.cpp:387` と `EngineCliSelfTest.cpp:264-278` の両方から呼ぶ (式を二重に書かない)。
5. (review-1 #6) `TagNames.h:4` の冒頭説明に描画設定 (`rendering.occlusionCulling`) を足す。
6. (review-1 #7) `docs\test_checklists.md:624` の `--forward` を「`--deferred` あり / なし」に直す。

## 既定 ON の判断 (review-1 #3、planner の基準)
ユーザーは sub-06 の数字 (既定 render_bench で CPU +0.15 ms、unique で 1.7 倍) を見て、既定 ON + 設定で切り替え、を選んだ。#1 の待ちはその前提に入っていなかった。#1 を直したあとに、次の基準で planner が VERDICT で判断する:
- 既定の render_bench、Deferred、1920x1080、Release、実 GPU、各 10 回の中央値で、ON の `cpuMs.gbufferSubmit` の増分 (ON − OFF) が **0.5 ms 以下** (sub-06 でユーザーに示した範囲 0.15〜0.6 ms) なら、ユーザーの判断の前提が保たれているので既定 ON を続ける (聞かない)。
- 0.5 ms を超えるなら、前提が崩れたのでユーザーに確認する (`[ユーザーに聞ける]`)。
- GPU ms は二峰に揺れるので判断には使わない (参考値として ADR に載せる)。WARP の GPU ms は傾向の参考にする。

## やらないこと
- 読み戻しで描画を決めること (spec §4.1.4)。draw 数に応じた自動 OFF (spec §8、後回し)。

## 受け入れ条件
1. 対話モードでは統計の Map が待たない (`DO_NOT_WAIT`)。3 解像度で ON の `cpuMs.gbufferSubmit` が解像度に比例しない (実測の表)。撮影モードの dump の counts は、Release / Debug / Release --warp / Debug --warp の 4 構成で一致する (今までどおり)。
2. カメラカットのフレーム (`--render-bench-cut-frame 30 --shot-frame 30`、Forward / Deferred、WARP 960x540) で、ON と `--no-occlusion` の `--img-diff --tol 0` が diffPixels=0。z-fight と断定した場合だけ、証拠付きで差を許す。selftest (LOD 付き、カット直後) が両構成で PASS。
3. #4〜#7 が直っている。
4. ビルド Debug / Release、`--selftest` 両構成 exit 0、`tools\check_rules.ps1` 0 error、`tools\shot_verify.bat` 30 枚 PASS、`tools\replay_verify.bat` PASS。
5. ADR-029 の計測節・判断表・画素差の記述が新しい実測と一致する。SELF_EVAL に上の「既定 ON の判断」の数字 (1920x1080、中央値、ON − OFF) を書く。

## 検証コマンド
- 上の受け入れ条件どおり。画素の A/B は WARP で取る (実 GPU は M90 前から 51 画素揺れる、spec §7)。

## 実装メモ (coder が追記)

### round 0 (SELF_EVAL)
- #1: `OcclusionCuller::PollStats(device, viewKey, wait)` を新設し、`TestPhase2` から `ReadStats` の呼び出しを外した。経路 (Deferred / Forward) は GPU/CPU 計測区間の後で `PollStats` → `PublishStats` を呼ぶ。`wait=false` は `D3D11_MAP_FLAG_DO_NOT_WAIT` で、失敗 (WAS_STILL_DRAWING) なら前の値を残す。切り替えは `RenderView::occlusionStatsWait` ← `RenderSystem::occlusionStatsWait` (既定 true)、`EngineLoop` が `config.IsCaptureRun()` を入れる (対話は false)。
- 実測 (Deferred、既定 bench、Release、実 GPU、shot 300、中央値 ms、ON−OFF): 320x180 0.253−0.177=0.076 (5 回) / 1920x1080 0.259−0.196=**0.064 (10 回)** / 3840x2160 0.248−0.200=0.048 (5 回)。修正前の 1.4〜3.5 ms は解像度に比例しなくなった。unique bench 1080p は 0.55 → 1.01 ms。GPU frame ms (参考) 1080p 既定 bench 1.50 (ON) / 1.51 (OFF)。
- #2: 原因は z-fight (描く順序) と断定。`--lod-force 1` でも同じ 11 画素が違うので LOD の段の取り違えではない。一時コードで可視ビット履歴を捨てると ON と OFF が全画素一致 (diffPixels=0、Forward)。カットのフレームの可視ビットが古いカメラのものでフェーズ 1 の描画順が変わるのが原因。コードは直さず ADR-029 3-2 / 3-6 / 検証節に書いた。Deferred も 11 画素 (maxDiff 49) で同じ。`OcclusionSelfTest` の `TestPathAB` に LOD 付きメッシュ (LOD1 の run、段数超過、LOD0 の run) を足した (Deferred / Forward 両方、履歴なし・定常・カット・通番の飛び・失敗注入で ON/OFF 全画素一致)。`TestStats` に待たない読み戻しの経路を足した。
- #4: `Menu_Occlusion` の「(Deferred)」を両言語から外した。#5: `ResolveOcclusionCulling(assetsRoot, cliFlag)` を TagNames.{h,cpp} に作り、EngineLoop と EngineCliSelfTest から呼ぶ。#6: TagNames.h 冒頭の説明を直した。#7: test_checklists.md の `--forward` を「`--deferred` あり / なし」に直した。
- 検証: Debug / Release ビルド OK。`Editor.exe --selftest` Release exit 0 / Debug exit 0 (Occlusion PASS)。`tools\check_rules.ps1` 0 error (50 warning は既存)。`tools\shot_verify.bat` PASS (30 枚)。`tools\replay_verify.bat` PASS。dump の counts は Release / Release+WARP / Debug / Debug+WARP × Deferred / Forward の 8 本で完全一致 (occluded 2793)。
- 既定 ON の判断材料 (review-1 #3): 1920x1080 / Release / 実 GPU / Deferred / 既定 bench / 各 10 回の中央値で `cpuMs.gbufferSubmit` は ON 0.259 ms、OFF 0.196 ms、増分 0.064 ms (基準 0.5 ms 以下)。

## フィードバック履歴
- round 0: VERDICT OK (planner、2026-10-10)
  - #1: 統計の読み戻しを `PollStats` の 1 か所に集め、計測区間の外へ出した。待つかどうかは `RenderView::occlusionStatsWait` (撮影 = 待つ / 対話 = DO_NOT_WAIT) で決める。3 解像度での ON の増分は +0.076 / +0.064 / +0.048 ms で、解像度に比例しなくなった。counts は 8 構成で一致した。
  - #2: z-fight と断定し、証拠付きで許容する (spec 受け入れ 8 の「z-fight 由来の差は原因を示して許容」に該当)。証拠: 可視ビットの履歴を捨てると ON と OFF が全画素一致する (描画順だけが原因)。`--lod-force 1` に固定しても差が出るので、段の取り違えではない。LOD 付きの ON/OFF 一致を selftest に追加した。カメラカットを描画側へ知らせる口は足さない (エンジンはカットを知らない。足すなら API の追加になり、M90 の範囲外)。
  - #4〜#7 は解消した。
  - review-1 #3 の判断: 1080p / 10 回の中央値で増分は +0.064 ms。基準の 0.5 ms 以下なので、ユーザーの判断の前提は保たれている。既定 ON を続け、ユーザーには聞かない。
  - nit (申し送り): `TagSelfTest.cpp:304` の C4456 (M90h 由来)。対話モードの cpuMs を直接測る手段が無い。LOD 付きテストの変異テストは未実施。
