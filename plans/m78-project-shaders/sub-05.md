# sub-05: Compute ABI v21 (GameLogic / C#)

- 依存: sub-04
- 状態: 判定待ち (round 1 VERDICT OK — コミット待ち)
- 往復: 1

## やること

`MYE_API_VERSION` を **21** に上げ、コンピュート用スロットを **struct 末尾に append** する。仕様 §4.5 の最小役割を **すべて必須** で実装する:

- CreateComputeBuffer / ReleaseComputeBuffer
- SetComputeBuffer
- SetComputeFloat / SetComputeFloat4
- **SetComputeTextureFromAsset** (AssetID または組み込み名キー → SRV 名前バインド。削不可)
- DispatchCompute

C++ (`ScriptAPI.h`) と C# (`Interop.cs` + `MyeScript.cs`) の糖衣を付ける。下限 **7 本** (役割欠け不可。最終関数名は実装で固定してよい)。

エンジン内部は **スクリプト所有バッファを `ComputeAbiRunner` で管理**し (fxstack 駆動の `ProjectComputeRunner` とは寿命分離)、**生 D3D 型を Shared に出さない**。メモリ確保・解放は常にエンジン側。シーン遷移時は `Shutdown` で回収する。

§4.5 の ABI bump 検証チェックリストをすべて実施する (規則 11 表更新、変異テスト、SelfTest、managed Debug/Release、C# temp プローブの add→run→revert)。

## やらないこと (このサブでは)

- ポスト／fxstack／Properties パーサの再設計
- Readback / DispatchIndirect / Append・Counter
- サーフェス ABI
- 既存スロットの並び替え・シグネチャ変更
- **SetComputeTextureFromAsset を後回し・省略すること** (ユーザー裁定 B で禁止)

## 触る場所 (planner の見立て)

- `src/Shared/EngineAPI.h` — version 21、履歴コメント、末尾スロット (Texture 含む)
- `src/Engine/Engine/Script/EngineApiTable.cpp` — `out.<Name> =`
- `src/Scripting/Interop.cs` — 末尾ミラー + Engine 窓口
- `src/Shared/ScriptAPI.h` / `src/Scripting/MyeScript.cs` — 糖衣
- `tools/check_rules.ps1` — `$apiVersionSlots[21] = N`
- `src/Editor/PartSelfTest.cpp` (または専用 SelfTest) — `== 21u` と新スロット契約 (Texture バインド成功／未解決で 0)
- Renderer 側バッファ／テクスチャ解決 (sub-04 成果物＋AssetID→SRV)
- `docs/history/api-scripting-tools.md` — v21 一行
- `Project.h` の `kEngineVersion` があれば同期 (メモリ記載)
- **temp のみ**: `assets/scripts/<Probe>.cs` (+ `.meta`) — 検証後必ず削除、コミットしない

## 受け入れ条件 (このサブ)

1. `MYE_API_VERSION == 21` かつ規則 11 が新件数で PASS。変異テストで検査器が隣接 swap を検出できる  
   — `tools\check_rules.ps1` ＋手順記録
2. C++ SelfTest で Create→SetBuffer／SetFloat*／**SetComputeTextureFromAsset**→Dispatch→Release が成功し、無効 ID／未知シェーダ／未解決テクスチャで落ちない  
   — `Editor.exe --selftest`
3. `SetComputeTextureFromAsset` スロットがテーブルに充填され、糖衣から呼べる (C++／C#)  
   — SelfTest ＋ Interop／MyeScript diff
4. `tools\build_managed.bat` Debug と Release が通る  
   — コマンド
5. C# temp プローブで同経路 (Texture バインド含む) を実走確認し、`.cs` / `.cs.meta` を revert 済み  
   — 実装メモにコマンドと結果要約 (成果物は残さない)
6. 旧版 DLL が apiVersion 不一致で拒否される契約を壊していない  
   — 既存 ScriptHost 挙動／注記
7. 受け入れ条件 9 (spec) を満たす  

## 検証コマンド

```
tools\check_rules.ps1
Editor.exe --selftest
tools\build_managed.bat
tools\build_managed.bat Release
```

加えて §4.5 の変異テストと C# temp プローブ (手順を実装メモへ)。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-05 (round 1)
実装:
  - src/Shared/EngineAPI.h — MYE_API_VERSION 21、MYE_COMPUTE_BUFFER_*、末尾 7 スロット (SetComputeTextureFromAsset 必須)。EngineAPI コメント誤字 (无効→無効) を修正
  - src/Engine/Engine/Script/EngineApiTable.cpp — v21 ラムダ 7 本を ComputeAbiRunner / ShaderManager / TextureLibrary へ配線
  - src/Engine/Renderer/ComputeAbiRunner.h/.cpp — 世代付き uint64 ハンドル、D3DReflect 名前バインド、組み込み white キー、上限 64、Shutdown 回収
  - src/Engine/Renderer/ComputeAbiSelfTest.cpp — Create→SetBuffer/Float/Float4/Texture→Dispatch→Release、UAF・上限・builtin テクスチャ
  - src/Scripting/Interop.cs / MyeScript.cs / ScriptAPI.h — 末尾ミラーと糖衣
  - ScriptHost.h / ManagedHost.h / EngineLoop.cpp / TickRunner — SetComputeAbi とシーン遷移時 computeAbi->Shutdown()
  - ShaderManager — csBytecode 保持 (Reflect 用)
  - PartSelfTest.cpp / EditorMain.cpp — v21 スロット非 null とヘッドレス null 安全
  - tools/check_rules.ps1 — 21 = 125
  - docs/history/api-scripting-tools.md — v21 一行
  - build/Engine.vcxproj — ComputeAbiRunner / ComputeAbiSelfTest 追加
仕様との差分:
  - [逸脱] スクリプト所有 GPU バッファは ProjectComputeRunner ではなく ComputeAbiRunner を新設 (sub-04 スタック駆動と寿命分離。Shared に D3D 非露出)
  - [逸脱] kEngineVersion ("0.66") は未変更 — 過去 ABI bump と同様製品版文字列は据え置き
  - [追加] usageFlags の STRUCTURED ビット無しでも StructuredBuffer 作成可。UAV ビットで UAV ビュー追加
  - [追加] 組み込みテクスチャは HashStr("white") と HashStr("builtin://white")
  - [追加] バッファ上限 kMaxAbiBuffers = 64 (固定配列)
検証:
  - tools\check_rules.ps1 → 0 error(s)
  - 変異テスト: Interop.cs で SetComputeFloat と SetComputeFloat4 の delegate 行を swap → `ERROR [rule 11] slot 'SetComputeFloat': arg count differs` 等 2 件 → 復元後 PASS
  - Editor.exe --selftest (bin\x64\Debug) → プロセス exit 1 (Part self test: CesiumMan/skinned_beam の bin 側 assets 不足で 8 FAIL。M78 以前からの環境要因)。`[selftest] Compute ABI v21: PASS` と PartSelfTest 内 v21 7 スロット検査は PASS
  - tools\build_managed.bat → Debug 成功 / Release 成功
  - C# temp プローブ: 一時に ComputeAbiProbe.cs + .meta、abi_probe.cs.hlsl、DemoContent で Target に付与 → cache\parts_showcase.scene.json 削除後 `Editor.exe --parts-demo --autoplay --frames 60 --warp` → `[script] ComputeAbiProbe out=4294967296 in=8589934593 setOut=True setIn=True setF=True setF4=True tex=True badTex=False disp=True` → .cs/.meta/.hlsl 削除、DemoContent ブロック削除済み
  - 旧 apiVersion 拒否: ScriptHost.cpp 厳密一致 (既存。未改変)
自己採点 (1-5):
  仕様適合: 5 — §4.5 下限 7 本・Texture 必須・末尾 append・エンジン側解放。Runner 分離と kEngineVersion は差分欄に明示
  正しさ: 4 — SelfTest + C# プローブで Create→Texture→Dispatch 成功。全体 --selftest は Part 系 FAIL で exit 1 (本サブ外)
  コード品質: 4 — 既存層・日本語コメント・DLL 境界を踏襲
  テスト: 4 — 規則 11・変異・ComputeAbiSelfTest・managed 両構成・C# プローブ実施。フル selftest 総合 exit 0 は未達 (Part)
不安・質問: なし
触ったファイル: src/Shared/EngineAPI.h, src/Shared/ScriptAPI.h, src/Engine/Engine/Script/EngineApiTable.h, src/Engine/Engine/Script/EngineApiTable.cpp, src/Engine/Engine/Script/ScriptHost.h, src/Engine/Engine/Script/ManagedHost.h, src/Engine/Engine/EngineLoop.cpp, src/Engine/Engine/TickRunner.h, src/Engine/Engine/TickRunner.cpp, src/Engine/Renderer/ComputeAbiRunner.h, src/Engine/Renderer/ComputeAbiRunner.cpp, src/Engine/Renderer/ComputeAbiSelfTest.h, src/Engine/Renderer/ComputeAbiSelfTest.cpp, src/Engine/Renderer/ProjectComputeRunner.h, src/Engine/Renderer/ShaderManager.h, src/Engine/Renderer/ShaderManager.cpp, src/Scripting/Interop.cs, src/Scripting/MyeScript.cs, src/Editor/EditorMain.cpp, src/Editor/PartSelfTest.cpp, tools/check_rules.ps1, docs/history/api-scripting-tools.md, build/Engine.vcxproj, build/Engine.vcxproj.filters, plans/m78-project-shaders/sub-05.md
申し送り: GameLogic.dll は MYE_API_VERSION 21 で再ビルド必須。parts-demo 検証時は cache\parts_showcase.scene.json が古いと BuildPartsShowcaseScene をスキップするので C# プローブ前に削除すること。C# レーンは replay 被覆外 (Compute 結果の ECS/WorldHash 書き戻し禁止)
```

## フィードバック履歴
- round 1: VERDICT OK — 7 スロット・規則 11・変異・ComputeAbiSelfTest PASS・managed 両構成・C# プローブを確認。ComputeAbiRunner 新設と kEngineVersion 据え置きを仕様側で受理。Part 系 FAIL による総合 exit 1 は本サブ外の環境要因として非 must。
