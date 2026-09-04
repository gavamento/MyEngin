# sub-02: ReflectionClass の配管 (Material → RtInstance → HLSL) + デバッグ 13

- 依存: sub-01
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
