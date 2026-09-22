# sub-02: プロジェクトポスト挿入 (Resolve フック＋マゼンタ)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること

`PostProcess::Resolve` に **BeforeTonemap / AfterTonemap** のユーザーポスト実行点を追加する。サブシステム (仮称 `ProjectPostPass` / `ProjectEffectRunner`) が:

- シェーダ名から `ShaderManager::Load` (命名規約は実装で確定し SelfTest 固定)
- sub-01 の Properties パース＋値パック
- 共通 include (`ProjectPostCommon.hlsli`) 経由で SceneColor / Depth 等をバインド
- フルスクリーン描画

を行う。コンパイル失敗時はそのパス出力を **マゼンタ (1,0,1)** で塗り、ログにエラーを残す。

スタックが空のときは **追加のフルスクリーンパスを走らせない** (受け入れ条件 5)。暫定的に C++ からテスト用パス一覧を渡せる口、またはエンジン内デバッグ登録で縦切りを通す (fxstack ファイルは sub-03)。

## やらないこと (このサブでは)

- `*.fxstack.json` 本実装と Inspector (sub-03)
- コンピュート (sub-04)
- CameraPostFx の既存フィールド変更、Material／サーフェス

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/PostProcess.cpp` / `.h` — Resolve 内の 2 挿入点
- 新規 Runner / Pass クラス (Renderer 配下)
- `assets/shaders/ProjectPostCommon.hlsli` およびサンプル／エラー用ポスト HLSL
- `RenderSystem.cpp` — Runner への設定受け渡しが必要なら最小限
- SelfTest: 挿入点の順序ソート、空スタック恒等、可能ならパック結合

## 受け入れ条件 (このサブ)

1. BeforeTonemap / AfterTonemap にユーザーポストを挿入でき、Priority 昇順で安定ソートされる  
   — SelfTest (順序) ＋手動 1 本
2. 壊したシェーダでマゼンタ出力＋クラッシュなし  
   — 手動スクショ／ログ (検証手順を実装メモに残す)
3. パス 0 件時、既存 Resolve 経路と実質同一 (余分コピー無し)  
   — コードレビュー根拠＋既存 Post SelfTest
4. Material / Deferred GBuffer / `.mat.json` shader 経路を変更していない  
   — diff

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: デバッグ登録または仮リストで Tint ポスト 1 本 → BeforeTonemap 確認。壊し HLSL でマゼンタ。

## 実装メモ (coder が追記)

## フィードバック履歴
