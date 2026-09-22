# sub-04: プロジェクトコンピュート (シーン／スタック駆動)

- 依存: sub-01, sub-03
- 状態: 未着手
- 往復: 0

## やること

fxstack の `kind: "compute"` エントリを、指定 `dispatchPoint` (`BeforePost` / `BeforeTonemap` / `AfterTonemap`) で `ShaderManager::LoadCompute` → バインド → `Dispatch` → 必ず unbind する。Properties / 名前バインドはポストと共通基盤 (sub-01) を使う。一時 UAV／バッファの寿命はエンジン側。失敗時はスキップ＋Editor/ログエラー (マゼンタはポスト専用)。

ハード上限 (仕様の例: 8) を実装し、超過は警告＋切り捨て。

サンプル: UAV を単色 fill し、後続ポストがそれを読む最小デモ (assets に置く場合はプロジェクト例として明示ステージ)。

**GameLogic/C# ABI は sub-05。** 本サブでは内部 Runner を後から ABI が薄いラッパで呼べるよう、Renderer 内に「名前＋バッファハンドル相当で Dispatch できる」入口を用意しておくと sub-05 が楽になる (必須ではないが推奨)。

## やらないこと (このサブでは)

- `EngineAPI.h` / `Interop.cs` / `MYE_API_VERSION` の変更 (→ sub-05)
- サーフェス、パーティクル CS 差し替え
- 間接 Dispatch / AppendBuffer の一般化 (必要最小で Structured/UAV テクスチャまで)
- GPU Readback → ECS

## 触る場所 (planner の見立て)

- ProjectEffectRunner (sub-02/03 で入れたもの) の compute 分岐
- `RenderSystem` / `PostProcess::Resolve` 前後の dispatchPoint 呼び出し
- リソースプール (解像度キーの一時 UAV)
- SelfTest: グループ数計算、失敗時スキップ、上限
- **触らない**: `src/Shared/EngineAPI.h`、`Interop.cs` (sub-05)

## 受け入れ条件 (このサブ)

1. fxstack からコンピュートが Dispatch され、結果を後続ポストまたはデバッグ可視化で確認できる  
   — 手動
2. 壊した CS でスキップ＋エラー、クラッシュなし  
   — 手動／ログ
3. スタック空／compute 0 で既存経路を汚さない  
   — SelfTest／レビュー
4. Material／サーフェス経路を触っていない  
   — diff
5. `Editor.exe --selftest` と `tools\check_rules.ps1` パス  

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: fill CS → ポストで表示。壊し CS でスキップ確認。

## 実装メモ (coder が追記)

## フィードバック履歴
