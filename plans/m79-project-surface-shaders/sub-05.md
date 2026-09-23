# sub-05: WaterWave の surfaceMaterial と MyEngineWater

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること

spec §2 の WaterWave 行、§4.2 WaterWave を実装し、**浮力と同じ波パラメータで、水面をサーフェスシェーダで描ける**ようにする (Water プロジェクトの主用途)。

- `WaterWaveComponent` (`Components.h:1650-`) に `AssetID surfaceMaterial` を**末尾追加** (Reflection: FieldType::AssetRef、`.mat.json` を参照)。欠損 / null = 従来。TypeId 不変。**WorldHash / リプレイに入らない**こと (Reflection のハッシュ除外の仕組みを確認して使う。無ければ理由と代替を実装メモに)
- RenderSystem (`RenderSystem.cpp:941-1001`) が水面データを作るとき、`surfaceMaterial` が有効なサーフェスマテリアルなら「水面をサーフェス経路で描く」印と、WaterPlane メッシュ (`resources.meshes.WaterPlane()`)・水面 World・マテリアルを渡す。WaterPass は描かない
- サーフェス経路 (Forward の不透明/透明、Deferred のサーフェス段/透明段、CSM 影) で水面を 1 アイテムとして描く。前 World は今と同じでよい (水面は動かない前提。動くなら理由を書いて prevWorld を持つ)
- 予約 CB `MyEngineWater` を、水面が有効なフレームは WaterMaterialCB 相当 (波 4 本・急峻度・基準高さ・全体スケール・色・光学) ＋今/前の水面時刻 (既存 `t = viewFrameIndex/60 * timeScale` と、その前フレーム値) ＋有効フラグ 1 で埋め、**全サーフェスシェーダ**へ名前で張る。無効フレームは 0 埋め＋有効フラグ 0
- サンプル: エンジン `assets/shaders/` に `MyEngineWater` の Gerstner を使う水面の `*.surface.hlsl` を 1 本 (既定シーンからは参照しない)。コメントに「VSMain では `gTime` ではなく `gWaterTime` (include の static、spec §4.1) を使うと WaterWave の時計と一致し、速度エントリで前/今が正しく差し替わる」旨。`gWaterTime` の static 宣言は sub-01 の include にあり、ここで今/前の値を配線する
- 設定 UI: Inspector は Reflection の AssetRef 欄として自動で出る想定。出ない場合は既存の AssetRef 欄と同じ扱いにする

## やらないこと (このサブでは)

- 浮力 (CPU Gerstner) の式・時計の変更
- 組込み `water_surface.hlsl` / WaterPass の変更 (未設定時の従来経路は 1 ビットも変えない)
- Water プロジェクトの本物のファイルの書き換え (検証はコピーか一時シーンで)

## 触る場所 (planner の見立て)

- `src/Engine/Core/Components.h` と Reflection 登録 (WaterWave の既存登録箇所)
- `src/Engine/Engine/RenderSystem.cpp` — 水面収集 (`RenderSystem.cpp:941-1001`)
- `src/Engine/Renderer/WaterPass.*`、`ForwardPath.cpp`、`DeferredPath.cpp`、`ShadowPass.cpp` — 水面アイテムの受け渡し
- `assets/shaders/MyEngineSurface.hlsli` — `MyEngineWater` (sub-01 で宣言済みなら中身の配線のみ)
- SelfTest: `surfaceMaterial` 欠損のシーン JSON 読み込みで null、保存往復、ハッシュ非関与 (同じシーンで設定有無の WorldHash が一致)

## 受け入れ条件 (このサブ)

1. `surfaceMaterial` 設定時、水面がサーフェスシェーダで描かれ、`MyEngineWater` の波パラメータで変位する — 一時シーンのスクショ
2. Deferred＋`--velocity-debug` で水面の変位が速度に出る、CSM 影が変位後の形 — スクショ 2 枚
3. 未設定時は従来の WaterPass と同一の絵 — 設定前後の既存 Water シーンのコピーで比較スクショ (またはピクセル差 0)
4. 浮力の結果と WorldHash が設定の有無で変わらない — `--selftest`、`tools\replay_verify.bat`
5. 既存シーン (WaterWave を含むものを含む) の読み込み・保存が壊れない — `--selftest`

## 検証コマンド

```
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <Water のコピー> --scene <main のコピー> --deferred --velocity-debug --screenshot <png>
```

## 実装メモ (coder が追記)

## フィードバック履歴
