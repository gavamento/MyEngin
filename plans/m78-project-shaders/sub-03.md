# sub-03: fxstack アセット＋Inspector Properties UI

- 依存: sub-01, sub-02
- 状態: 未着手
- 往復: 0

## やること

`*.fxstack.json` のロード／保存と、カメラからの AssetRef 接続を実装する。Inspector で:

- スタック参照
- パス一覧 (enabled / kind / insertion|dispatchPoint / priority / shader)
- 選択中ポストの Properties を sub-01 スキーマから自動ウィジェット化 (Slider / ColorEdit / Vector / テクスチャ選択)

値変更はスタック JSON の `properties` に書き戻し、実行中 Runner に反映する (保存前プレビューは既存マテリアルプレビューほど厳密でなくてよいが、Play 中は毎フレームまたはダーティ時に反映)。

AssetDatabase の分類・ブラウザ表示に `fxstack` を載せる (既存 ClassifyPath の流儀に合わせる)。

## やらないこと (このサブでは)

- コンピュート Runner 本体 (エントリ型は JSON に含めてよいが実行は sub-04)
- ABI、サーフェス
- Volume 空間ブレンド

## 触る場所 (planner の見立て)

- 新規: fxstack ローダ／型 (`Engine/Asset` または `Renderer` 近傍)
- `Components.h` — 薄コンポーネントまたは Camera 近傍の `AssetID fxStack` (末尾 append・既存シーン互換)
- `InspectorWindow.cpp` — スタック＋Properties UI
- AssetDatabase 分類、必要なら `.fxstack.json.meta` 慣例
- シリアライズ: FieldDesc の AssetRef ＋ JSON ファイル本体

## 受け入れ条件 (このサブ)

1. fxstack をカメラに割り当て、プロジェクト `*.post.hlsl` が挿入点どおり走る  
   — 手動手順 (実装メモに記載)
2. Properties 変更が Inspector からでき、画面に反映される  
   — 手動
3. fxstack 未設定シーンは従来どおり  
   — 手動／既存テスト
4. ローカライズ規約を破る生文字列を増やしすぎない (`MYE_JP` 等)  
   — レビュー

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: サンプル fxstack + Tint ポストで Inspector 操作。

## 実装メモ (coder が追記)

## フィードバック履歴
