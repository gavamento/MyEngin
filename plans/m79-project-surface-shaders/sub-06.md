# sub-06: サーフェスの境界余白と両面 (boundsPadding / doubleSided)

- 依存: sub-03 (review-1 #1 の差し戻し分が OK になってから)、sub-04 (review-1 #5 の差し戻し分が OK になってから)
- 状態: 未着手
- 往復: 0
- 出所: review-1 #3 (major、planner 宛て) / #7 (minor、planner 宛て)

## やること

spec §4.1「カリングと境界」「両面」、§4.2 `.mat.json` の `boundsPadding` / `doubleSided`、§4.3 の Inspector 追記を実装する。

- `MaterialLibrary` の横テーブルに `boundsPadding` (float [m]、欠損 0、負値は 0＋WARN) と `doubleSided` (bool、欠損 false) を読む。`Material` POD は変えない。`.mat.json` の再読込で反映
- `RenderSystem` の視錐台カリング (`RenderSystem.cpp` のステージ 2、`RenderableInFrustum` 呼び出し付近) と CSM のキャスター AABB 集約 (sceneMin / sceneMax) で、サーフェスマテリアルのアイテムはメッシュ AABB をワールド空間で各軸 `boundsPadding` 広げた箱を使う。0 なら従来と完全に同じ判定 (既定シーンのビット一致)。カリングはジョブ並列の純関数なので、余白の値は並列ステージの前に解決しておく (横テーブル参照を並列内でしない)
- `doubleSided: true` のサーフェスは、Forward の不透明・透明、Deferred のサーフェス段・透明段、ShadowPass の影エントリの全部で Cull None のラスタライザ状態を使う。描いた後は各パスの既定ラスタライザへ戻す (spec §4.1「固定スロットの復元」と同じ扱い。Wireframe 表示中の状態を壊さない)
- マテリアル Inspector: サーフェス選択時に `boundsPadding` (DragFloat、0 以上) と `doubleSided` (Checkbox) を出し、保存・JSON 往復で保持。ツールチップ (日英) で「頂点変位で形がメッシュの外へ出るなら余白を付ける (付けないとカメラ外判定で消える)」
- docs (`docs/surface-shaders-deferred-limits.md` かサーフェスの作者向け docs) に、カリングの余白と両面、裏面判定 (`SV_IsFrontFace` は渡らない。法線と視線の内積で作者が判定) を追記。テンプレートのコメントにも 1 行

## やらないこと (このサブでは)

- HLSL 側でのレンダーステート宣言
- `SV_IsFrontFace` を作者へ渡すこと (シグネチャ規約を変える)
- 自動の境界計算 (変位量をシェーダから推定する等)
- スキンメッシュのカリング変更

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/GpuResources.h/.cpp` — 横テーブルと `ParseMaterialJson`
- `src/Engine/Engine/RenderSystem.cpp` — カリング候補の構築 (`cullCands`)、ステージ 2 (`:1095-1115` 付近)、キャスター AABB
- `src/Engine/Renderer/ForwardPath.cpp`、`DeferredPath.cpp`、`ShadowPass.cpp` — Cull None のラスタライザ状態と戻し
- `src/Editor/Windows/InspectorWindow.cpp/.h`、`src/Engine/Core/LocalizationTable.inl`

## 受け入れ条件 (このサブ)

1. `boundsPadding` を付けたサーフェスは、変位で元の AABB の外へ出てもカメラに写り、影も落とす。0 は従来どおり — `--selftest` (余白付き AABB のカリング判定の単体)、reviewer rv1 の liftA 相当 (余白あり/なし) の Runtime.exe スクショ
2. `doubleSided: true` のサーフェスは裏面も描かれ、影も両面で落ちる。false は従来どおり — `--selftest` (薄板を裏から read-back) またはスクショ
3. Inspector で 2 つの欄が出て、保存で `.mat.json` に書かれ、JSON 往復で保持 — `--selftest` (往復)＋Editor スクショ
4. 既定シーンの絵・決定論が不変 — Debug/Release `--selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
5. 新規文字列が日英両方 — `tools\check_rules.ps1`

## 検証コマンド

```
tools\gen_project_files.ps1   (ファイル追加時。pwsh で実行)
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <一時> --scene <一時> [--deferred] --frames <N> --screenshot <png>
```

一時シーン作成の罠 (台帳申し送り): `.meta` の GUID は自動生成を読む、メッシュは `builtin://cube`、`--frames` は撮影フレーム (既定 60) 以上、Editor の `--project` は `project.mye.json` 必須。

## 実装メモ (coder が追記)

## フィードバック履歴
