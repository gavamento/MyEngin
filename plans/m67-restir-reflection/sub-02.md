# sub-02: ReflectionClass の配管 (Material → RtInstance → HLSL) + デバッグ 13

- 依存: sub-01
- 状態: OK (commit: 司会が記入)
- 往復: 1

## やること

spec §4.1 の全部 (`--rt-class-override` を除く)。元計画 S1。ReSTIR 本体には触れない。

1. `RtTypes.h`: `kRtReflClassHero/Character/Vehicle/Prop/Default` (0〜4) と `kRtReflClassCount = 5`
   (「HLSL の `MYE_RT_REFL_CLASS_COUNT` と規則 9 で照合」のコメント)。`RtInstance.pad0` → `reflectionClass`
   に改名 (既定 4。`pad1` は残す。`static_assert == 80` 不変)。
2. `rt_common.hlsli`: `RtInstance.pad0` → `reflectionClass`、`#define MYE_RT_REFL_CLASS_COUNT 5`、
   `int RtHitReflectionClass(RtHit hit)`、`float3 RtReflClassColor(int cls)` (spec §4.1 の 5 色 + それ以外は黒)。
3. `GpuResources.h` `Material` 末尾に `int32_t reflectionClass = 4;` (なぜ Material 単位か、ヒット側で引く理由、
   欠損 / 範囲外 = 4 の理由をコメント)。`ParseMaterialJson` に `"reflectionClass"` (欠損・範囲外 → 4)。
4. `RtScene::Update`: `Material*` を先に引いて `inst.reflectionClass` を埋める (現状は `inst` を push した後に
   `mat` を引いている — 順序を入れ替える)。`mat == nullptr` は 4。
5. `AssetOps.cpp` `CreateMaterialAsset` の雛形に `root["reflectionClass"] = 4;`。
6. `InspectorWindow`: `MaterialEdit` に `reflectionClass`、load (欠損 = 4) / save / `ImGui::Combo` (5 クラス名) +
   ツールチップ。
7. `rt_debug.cs.hlsl`: `gDebugMode == 13` で `RtReflClassColor(RtHitReflectionClass(hit))`。CB は不変。
   `RtPasses::RenderDebug` の 4〜11 の Blit 連鎖を素通りして CS 経路に落ちることを確認 (RtDebugCB の
   `debugMode` にそのまま 13 が入る)。
8. `EditorApp.cpp` RT Debug メニューに mode 13 の項目 (11 の直後)。`LocalizationTable.inl` に
   `Menu_RtDbgReflClass` / `Mat_ReflClass` / `Insp_TipReflClass` / `ReflClass_Hero` … `ReflClass_Default` (en/ja)。
9. `DemoContent.cpp`: `rdemo_spin` = 0、`rdemo_pillar_a/b/c` = 3、`adem_player` = 0、`adem_agent_ear/eye` = 1
   (登録後に `res.materials.Get(id)->reflectionClass = ...` でも `makeMat` の引数追加でも可。**エンティティは
   足さない・順序も変えない** — 粒子 RNG ストリームが動く)。
10. `AssetOpsSelfTest.cpp` の JSON 往復に 3 ケース (欠損 → 4 / `1` → 1 / `-1` と `9` → 4)。
11. `check_rules.ps1` `$constGroups` に `kRtReflClassCount / MYE_RT_REFL_CLASS_COUNT`。

## やらないこと (このサブでは)

- reservoir / ReSTIR の一切。`--rt-class-override` (sub-06)。mode 12 / 14 (sub-04)。
- `FieldDesc` / ECS への露出 (Material は ECS ではない)。オブジェクト単位の上書き (スコープ外)。
- FBX ローダのクラス取り込み (既定 4 のまま)。

## 触る場所 (planner の見立て)

- `src\Engine\Renderer\RayTracing\RtTypes.h` (131-141)
- `assets\shaders\rt_common.hlsli` (37-46 の `RtInstance`、282 の `RtHitMaterial` の隣、定数は 9-10 行の隣)
- `src\Engine\Renderer\GpuResources.h` (182-196) / `GpuResources.cpp` (1001-1020 `ParseMaterialJson`)
- `src\Engine\Engine\RayTracing\RtScene.cpp` (163-185)
- `src\Editor\AssetOps.cpp` (327-345) / `src\Editor\AssetOpsSelfTest.cpp` (305-320)
- `src\Editor\Windows\InspectorWindow.h` (60-70 `MaterialEdit`) / `.cpp` (1448-1500 load/save、1563-1572 スライダ)
- `assets\shaders\rt_debug.cs.hlsl` (55-62)
- `src\Editor\EditorApp.cpp` (1160-1200 RT Debug メニュー)
- `src\Engine\Core\LocalizationTable.inl` (141-161 の RT 群、`Mat_*` 群)
- `src\Engine\Engine\DemoContent.cpp` (1125-1150 `rdemo_*` / 2699-2732 `adem_*`)
- `tools\check_rules.ps1` (`$constGroups`、90-110 の RT 群の隣)

## 受け入れ条件 (このサブ)

1. Debug / Release ビルド緑 (警告を増やさない)。
2. `Editor.exe --selftest` 全緑 (AssetOps の 3 ケース込み) (A3)。
3. `tools\shot_verify.bat` 21 枚全緑 (A1) — G-Buffer 非接触・`reflectionClass` は誰も読まないので当然だが機械で固定。
4. `Runtime.exe --render-demo --deferred --rt-refl --rt-debug 13 --screenshot` (SHOTBASE 条件) の画像で
   spin = 赤、柱 = 水色、床・遠景 = 灰、空 = 黒 (A3)。画像を `tests\actual\probe_rtdebug13.png` に残す
   (reviewer が見る。`tests\actual` は gitignore)。
5. `--acoustic-demo --deferred --rt-refl --rt-debug 13` (frame 3 = SHOTBASE 条件。frame 120 にすると rt_debug の
   CS が 120 フレーム走って遅い。初期配置でプレイヤーと敵は俯瞰に写る) でプレイヤー = 赤、敵 2 種 = 橙。
   画像は `tests\actual\probe_rtdebug13_acoustic.png`。
6. `check_rules.ps1` 緑 (A8 / A9)。
7. `.mat.json` 2 枚 (`assets\materials\demo_*.mat.json`) は無編集。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
cmd /c bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-debug 13 --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --screenshot tests\actual\probe_rtdebug13.png
```

## 実装メモ (coder が追記)

### round 1 (2026-09-05)

```
SELF_EVAL: sub-02 (round 1)
実装:
  - src\Engine\Renderer\RayTracing\RtTypes.h:kRtReflClass* — 5 段の定数 (Hero=0 … Default=4) と
    kRtReflClassCount=5 を M46h ブロックの直後に追加。RtInstance.pad0 → reflectionClass に改名
    (既定 kRtReflClassDefault、pad1 は残置、static_assert(sizeof==80) は不変)
  - assets\shaders\rt_common.hlsli — MYE_RT_REFL_CLASS_COUNT 5 / RtInstance.pad0 → reflectionClass /
    RtHitReflectionClass(RtHit) / RtReflClassColor(int)。色表は MYE_RT_REFL_CLASS_COUNT 長の
    配列にし、上限判定にもその定数を使う (定義するだけだと規則 9 の照合が形骸化する)
  - src\Engine\Renderer\GpuResources.h:Material — 末尾に reflectionClass (既定 kRtReflClassDefault)。
    既定値を 1 箇所から取るため RtTypes.h を include
  - src\Engine\Renderer\GpuResources.cpp:ParseMaterialJson — "reflectionClass" を is_number_integer()
    で型検査してから [0,5) を採用。欠損 / 非整数 / 範囲外は 4 (クランプしない)
  - src\Engine\Engine\RayTracing\RtScene.cpp:RtScene::Update — Material* を先に引く順へ入れ替え、
    inst.reflectionClass を充填 (mat==nullptr は 4)
  - src\Editor\AssetOps.cpp:CreateMaterialAsset — 雛形に "reflectionClass": 4
  - src\Editor\Windows\InspectorWindow.{h,cpp} — MaterialEditState.reflectionClass /
    LoadMaterialEdit (ParseMaterialJson と同じ判定) / MaterialEditToJson / Combo 5 項目 + ツールチップ
  - assets\shaders\rt_debug.cs.hlsl — gDebugMode == 13 で RtReflClassColor(RtHitReflectionClass(hit))
  - src\Editor\EditorApp.cpp — RT Debug メニューに mode 13 (11 の直後)
  - src\Engine\Core\LocalizationTable.inl — Menu_RtDbgReflClass / Mat_ReflClass /
    Insp_TipReflClass / ReflClass_Hero..Default を en/ja
  - src\Engine\Engine\DemoContent.cpp — makeMat に既定引数 reflClass を追加 (2 箇所)。
    rdemo_spin=0 / rdemo_pillar_a,b,c=3 / adem_player=0 / adem_agent_ear,eye=1。エンティティ追加なし・順序不変
  - src\Editor\AssetOpsSelfTest.cpp — JSON 往復に reflectionClass を追加 + 範囲外/非整数 4 ケース +
    両端 (0/4) + Create > Material の雛形往復
  - tools\check_rules.ps1 — $constGroups に kRtReflClassCount / MYE_RT_REFL_CLASS_COUNT
  - src\Engine\Renderer\RenderTypes.h — rtDebugMode のモード一覧コメントに 13 を追記 (コメントのみ)
仕様との差分:
  - [追加] src\Engine\Engine\Asset\CookedCache.h:kCookVersion 1 → 2 +
    src\Engine\Engine\Asset\ModelCook.cpp の static_assert 56 → 64。理由: Material は cooked blob へ
    memcpy で書かれ、ModelCook に sizeof(Material)==56 の門番がある。フィールド追加で必ず落ちる
    ため回避不能。bump しないと旧キャッシュを読み違える
  - [追加] Material 末尾に int32_t pad0 = 0 (明示パディング)。理由: AssetID が uint64 =
    アラインメント 8 なので 60 バイトは 64 に丸められる。暗黙パディングのままだと
    「同じ入力から作った cooked ファイルのバイト列が run ごとに違う」が生まれ、
    CookedCacheSelfTest の memcmp(sizeof(Material)) も不定になる (M67 以前は 56 = 穴なしだった)
  - [追加] AssetOpsSelfTest に「Create > Material の雛形往復」1 ケース (指示は 3 ケース)。
    雛形へのキー追加 (sub 指示 5) は書き忘れても既定 4 で動く = 静かに抜けるため機械固定した
  - [追加] GpuResources.h / InspectorWindow.cpp / DemoContent.cpp / AssetOpsSelfTest.cpp に
    RtTypes.h の include。既定値・段数をリテラルで散らさないため
検証:
  - MSBuild Debug x64 → EXIT 0、warning 0
  - MSBuild Release x64 → EXIT 0、warning 0
  - cmd /c bin\x64\Debug\Editor.exe --selftest → EXIT 0、FAIL 0 件。新規 3 行が PASS
    (out-of-range / non-integer → 4、両端 0 と 4、Create > Material の雛形往復)。
    CookedCache self test も ALL PASS (Material 56 → 64 の往復 memcmp 込み)
  - pwsh -File tools\check_rules.ps1 → 0 error / 0 warning (規則 9 の新グループを含む。
    定義が見つからなければ ERROR になる実装なので、緑 = 両サイトが実際に照合された)
  - tools\shot_verify.bat → [PASS] 21 shots (3.4 min)。**demo_render_rtrefl / demo_render_rtgi が
    tol=0 で緑** = ReflectionClass の配管は RT 反射 / GI の絵を 1 画素も動かしていない。
    最終ツリーを含め計 3 回実行し、3 回とも緑
  - tools\replay_verify.bat → [PASS] 全 10 ジョブ 79.8 s (Debug/Release 7 シーン + snapshot 往復 +
    タイムトラベル + 規則検査)。sub-02 の指示外だが、kCookVersion bump が sim へ波及しないことを
    機械で確かめるために回した (A10 前倒し)
  - cmd /c bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-debug 13 (SHOTBASE 条件)
    → tests\actual\probe_rtdebug13.png。画素実測 (ACES + sRGB 後):
    spin (430,290) = 231,147,147 / 柱 (110,270) = 147,224,231 / 床 (480,450) = 205,205,205 /
    遠景 (470,130) = 205,205,205 / 空 (50,20) = 0,0,0。
    逆算すると線形値は (1.0,0.2,0.2) Hero / (0.2,0.8,1.0) Prop / (0.5,0.5,0.5) Default / 黒 =
    §4.1 の色表と完全一致
  - cmd /c bin\x64\Release\Runtime.exe --acoustic-demo --deferred --rt-refl --rt-debug 13 (frame 3)
    → tests\actual\probe_rtdebug13_acoustic.png。敵 2 体 (426,187) / (601,160) = 231,213,99 =
    線形 (1.0,0.6,0.1) Character、プレイヤー (240,458) = 231,147,147 = (1.0,0.2,0.2) Hero、
    床・壁 = 205,205,205 = Default、空 = 0,0,0。**橙は小さく写ると黄に見えるので画素で確定させた**
    (0.9,0.9,0.2 = Vehicle なら G も 231 になるはずで、実測 213 は Character でしか出ない)
  - .mat.json 2 枚は無編集 (git status に現れない)
  - 未実行: なし (sub-02 の検証コマンド 6 本 + 追加で replay_verify)
自己採点 (1-5):
  仕様適合: 4 — sub-02 の 11 項目すべてを実装し、受け入れ条件 1〜7 を機械 / 画素で確認した。
    5 にしないのは [追加] 4 件 (kCookVersion bump / Material の明示 pad / selftest 1 ケース /
    include 4 箇所) が仕様に無い判断を含むため
  正しさ: 4 — golden 21 枚 tol=0 込み緑・replay 全緑・selftest 全緑・デバッグ 13 の色を
    2 デモで画素実測。ただし Inspector の Combo は実機で操作していない (下記)
  コード品質: 4 — 既存のコメント密度と口調に合わせ、なぜそうなっているか (クランプしない理由 /
    型検査する理由 / 明示 pad の理由 / RtScene の順序入れ替え) を日本語で残した。
    RtTypes.h の include を 4 ファイルに足したぶん結合は増えている
  テスト: 4 — JSON 側は境界 (0/4)・範囲外 (-1/9)・非整数 (文字列/小数)・欠損・雛形往復を機械化。
    Inspector の LoadMaterialEdit / MaterialEditToJson は private メンバなのでヘッドレスから
    呼べず、往復はコード読みでしか担保していない (A3 の一部が未機械化)
不安・質問:
  1. Material が 56 → 64 バイトになり kCookVersion を 1 → 2 へ上げた。cooked キャッシュは
     gitignore なので影響は「初回起動で 1 回焼き直す」だけのはずで、CookedCacheSelfTest と
     replay_verify で確認済み。**この bump を sub-07 の engine_spec / ADR に書くべきか**を
     planner に判断してほしい (ABI ではないが、配布ビルドの封印キャッシュ M51j は作り直しが要る)
  2. A3 の「Inspector 保存 → 読み直しで往復」は InspectorWindow の private メンバのため
     ヘッドレスで呼べず、**コード読みでしか担保できていない**。機械化するなら
     (a) 2 関数を静的ヘルパへ切り出す (b) reviewer の実機操作で代替、のどちらかになる。
     sub-06 でチューニング UI を足すときに (a) をまとめてやる価値があるか判断してほしい
  3. `--rt-debug 13` は `--rt-refl` を必要としない (rt_debug の CS は反射バッファを見ない)。
     sub-02 の受け入れ条件は `--rt-refl` 付きで書かれているのでそのまま従ったが、
     sub-04 で 12 / 14 を足すときは「12 / 14 は --rt-refl が前提、13 は不要」と分かれる
触ったファイル:
  - assets\shaders\rt_common.hlsli
  - assets\shaders\rt_debug.cs.hlsl
  - src\Editor\AssetOps.cpp
  - src\Editor\AssetOpsSelfTest.cpp
  - src\Editor\EditorApp.cpp
  - src\Editor\Windows\InspectorWindow.cpp
  - src\Editor\Windows\InspectorWindow.h
  - src\Engine\Core\LocalizationTable.inl
  - src\Engine\Engine\Asset\CookedCache.h
  - src\Engine\Engine\Asset\ModelCook.cpp
  - src\Engine\Engine\DemoContent.cpp
  - src\Engine\Engine\RayTracing\RtScene.cpp
  - src\Engine\Renderer\GpuResources.cpp
  - src\Engine\Renderer\GpuResources.h
  - src\Engine\Renderer\RayTracing\RtTypes.h
  - src\Engine\Renderer\RenderTypes.h
  - tools\check_rules.ps1
  - plans\m67-restir-reflection\sub-02.md (この実装メモ)
申し送り:
  - (sub-04 へ) デバッグ 13 は rt_debug.cs.hlsl の CS 経路。RtPasses::RenderDebug の 4〜11 は
    Blit で早期 return するので、12 / 14 を足すときは **13 より前に** if を置くこと
    (13 は「どの早期 return にも当たらない」ことで CS へ落ちている)
  - (sub-04 へ) 反射像側の色分け (14) は同じ RtReflClassColor を使えば 13 と色が揃う。
    スカイ (cls が範囲外) は黒に落ちる実装になっている
  - (sub-07 へ) engine_spec / ADR に「Material 56 → 64 バイト、kCookVersion 1 → 2」を書くかは
    上の質問 1 の裁定待ち
  - (sub-03 以降へ) `RtInstance` の残りの空きは `pad1` 1 本だけ (sizeof==80 の枠が満杯)。
    クラス以外の per-instance 値が要るなら 80 → 96 になる = golden の再撮影が要る
  - (reviewer へ) 画像は tests\actual\probe_rtdebug13.png (--render-demo) と
    tests\actual\probe_rtdebug13_acoustic.png (--acoustic-demo)。どちらも gitignore 配下
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-05)。受け入れ条件 1〜7 を SELF_EVAL の検証欄で確認 (Debug/Release 警告 0 /
  selftest 新規 3 行 PASS / check_rules 0-0 / shot_verify 21 枚 ×3 回緑、RT 2 枚 tol=0 / debug 13 の色を 2 デモで画素実測 /
  `.mat.json` 無編集 / replay_verify 前倒し緑)。planner 側で `ModelCook.cpp:15-19` (memcpy + static_assert 64) と
  `CookedCache.h:19-23` (kCookVersion=2) と `git log -S kCookVersion` (M51b 導入 = M46i の emissive は cook 以前) を読んで裏取り。
  [追加] `kCookVersion` 1 → 2 + `static_assert` 56 → 64 + 明示パディング → **仕様側の見落とし** (spec §2 S15 に記録)。承認。
  [追加] 雛形往復テスト 1 件 / `RtTypes.h` の include 4 箇所 → 承認。
  [should] `CookedCache.h:20` のコメント「56 → 60 バイト」は静的検査の 64 と食い違う (60 + 明示パディング 4 = 64) →
  sub-07 の衛生項目へ (このサブは差し戻さない)。
  不安 1 → engine_spec §10.2 に 1 文 + CLAUDE.md チェックリスト (sub-07)。ADR には書かない (設計判断ではなく機械的帰結)。
  不安 2 → (b) reviewer の実機操作で担保。(a) の静的ヘルパ切り出しは sub-06 に**含めない** (チューニング UI は RT Debug
  メニューで Inspector に触らない。テスト容易化のための構造変更は M67 の外)。spec A3 の検証手段を実態に合わせた。
  不安 3 → spec §4.4 と sub-04 に「13 は `--rt-refl` 不要、12/14 は前提」と早期 return の順序を明記。
