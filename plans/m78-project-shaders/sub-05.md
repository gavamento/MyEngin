# sub-05: Compute ABI v21 (GameLogic / C#)

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## やること

`MYE_API_VERSION` を **21** に上げ、コンピュート用スロットを **struct 末尾に append** する。仕様 §4.5 の最小役割を **すべて必須** で実装する:

- CreateComputeBuffer / ReleaseComputeBuffer
- SetComputeBuffer
- SetComputeFloat / SetComputeFloat4
- **SetComputeTextureFromAsset** (AssetID または組み込み名キー → SRV 名前バインド。削不可)
- DispatchCompute

C++ (`ScriptAPI.h`) と C# (`Interop.cs` + `MyeScript.cs`) の糖衣を付ける。下限 **7 本** (役割欠け不可。最終関数名は実装で固定してよい)。

エンジン内部は sub-04 の Runner／バッファ管理を再利用し、**生 D3D 型を Shared に出さない**。メモリ確保・解放は常にエンジン側。

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

## フィードバック履歴
