# ADR-002: ホットリロード対象を GameLogic.dll に限定

## 決定
エンジン本体 (Platform/Core/Renderer/Engine) は静的リンク (Engine.lib) とし、C++ コードのホットリロードは GameLogic.dll のみを対象とする。

## 理由
- エンジン全体を DLL 化する場合と比べ、リロード時の状態保存・vtable 互換維持の複雑さが桁違いに小さくなる
- DLL 境界を「C ABI の関数テーブル + POD」1 箇所に絞れるため、境界規則 (spec 8.4) を機械的に守れる

## トレードオフ
- エンジン側の変更は再起動 (リビルド) が必要
