# デモ動画 台本 (2〜3 分)

事前準備: `main.scene.json` を削除した状態 (デモシーンが自動構築される)、
VS で `MyEngine.sln` を開いておく。録画は Editor.exe 全画面 + VS を並べる。

## 0:00 — 起動と概要 (20 秒)
1. F5 で起動。「C++/DX11 の自作エンジン。Unity 風 API × アーキタイプ ECS」
2. Hierarchy のツリー、Inspector (リフレクション自動生成) をさっと見せる
3. Scene ビューで右ドラッグ + WASD で一周

## 0:20 — Play とスクリプト (20 秒)
1. Play を押す → スピナー回転 (Rotator)、炎 (パーティクル)、Spawned キューブ出現
2. Game タブへ切替 → 矢印キーでプレイヤー (BoxTextured) を動かし、
   黄色いキューブを回収 → Console に「pickup #n」(GameLogic.dll の OnTriggerEnter)

## 0:40 — シェーダホットリロード (25 秒)
1. VS Code で `assets/shaders/common.hlsli` を開き、ライティング式に色味を掛ける
   → 保存 → 1 秒以内に全シェーダ (Forward/Deferred 両方) が変わる
2. わざと構文エラーを保存 → Console に赤いエラー (ファイル/行付き)、
   **エンジンは旧シェーダのまま動き続ける** → 直すと復帰

## 1:05 — C++ ホットリロード (35 秒) ★目玉
1. Play したまま VS で `Rotator.cpp` の回転軸を Y → X に変更 → Ctrl+Shift+B (GameLogic のみ)
2. 1〜2 秒で挙動が変わる。**角度 (状態) は継続、Start は再実行されない**
3. フィールドを 1 個追加してリビルド → Console に「layout migrated」、
   Inspector に新フィールドがデフォルト値で出現 (既存値は保持)
4. 新コードにブレークポイントを置いて止まることも見せる (PDB コピー + /PDBALTPATH)

## 1:40 — パーティクル比較 (30 秒)
1. Particle Settings で CPU → GPU 切替 (放出は再スタート)
2. Compare mode ON → 同一シードの CPU/GPU が横並びで同じ動き、
   update ms が両方表示 (SIMD トグルで CPU 時間の変化も)
3. Inspector で Fire の rate を 10 万に → GPU が余裕なことを Profiler で見せる

## 2:10 — レンダリングパスと一貫性検証 (30 秒)
1. View > Render Path > Deferred → 見た目そのまま切替
2. ターミナルで `tools\replay_verify.bat` 実行 →
   「Debug で記録したリプレイが Debug/Release 両方でハッシュ完全一致 = VERIFY PASS」
3. まとめ: レイヤ構成 / ADR / 決定論ポリシーに触れて終了
