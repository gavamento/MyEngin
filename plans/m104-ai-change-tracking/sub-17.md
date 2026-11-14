# sub-17: 削除エンティティのゴースト、Overlay (M104r)

- 依存: sub-16
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-16 の VERDICT で planner が Phase 6 を見直してから着手する。

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く (spec §2 D12)。

- 聞きたいこと: Phase 6 (sub-17〜19。優先度 P2) へ進むか。要求 §1.4 は「完全な 3D Side by Side 比較」を初期対象外と書き、要求 §15 と計画は Phase 6 に含めている。
- 推奨: Phase 5 までを使ってみてから決める。進む場合も、sub-17 (ゴースト) と sub-19 のプレハブ伝播の表示は要求 §16.3 の受け入れ項目に直接対応するので先に行い、sub-18 (左右の 3D 比較) を最後に回すか外すかを選ぶ。
- 逆を選ぶと何が変わるか: ここで止めると A17〜A19 が落ちる。要求 §18 の機能面 1〜8 は Phase 5 までで満たせるが、§16.3「Entity 削除時に Before を表示できる」は一覧と JSON の表示まで、「Prefab 伝播変更を直接編集と区別できる」は未達になる。

## やること

要求 §8.2、§8.4 の拡張段階。arch §7。

1. 削除されたエンティティのゴースト: Before のスナップショットから、メッシュとワールド行列を取り出して `GhostMeshPass::Add(mesh, world, rgba)` で描く。**World にエンティティを作らない。** 前例は `SceneViewGhost.cpp` (分岐ゴースト)。
2. Before の JSON からメッシュと行列を得る方法を着手時に決める。一時 `Scene` へ読んで取り出すなら、本番の Scene とアセットのキャッシュに副作用が無いことを確かめる。メッシュを持たないエンティティは位置の印だけ。
3. 読み取り専用のプレビュー: ゴーストをクリックすると、Before の内容 (コンポーネントとフィールド) を読み取り専用で見られる。`Selection` に入れられない (実体が無い) ので、AI Changes 窓の側に出す。
4. Scene Overlay: 追加・更新・削除・競合をまとめて重ねる表示の切り替え。未確認のアイコン (要求 §8.1)。

## やらないこと (このサブでは)

- Before / After の独立表示と左右比較 (sub-18)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\src\Editor\ChangeTracking\SceneChangeOverlay.*`、`C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewWindow.cpp`
- 参考: `C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewGhost.cpp`、`C:\HAL\MyEngin\src\Engine\Renderer\Passes\GhostMeshPass.cpp`

## 受け入れ条件 (このサブ)

1. ゴーストを出した状態と出していない状態で、本番の Scene の `HashWorld`、エンティティ数、アーキタイプ数が同じ → `changetracking` スイート。
2. 削除されたエンティティの Before の内容が読み取り専用で表示され、編集の操作が効かない → スイート (要求 §16.3 の 3 番目)。
3. Before の Blob が欠けているとき、ゴーストの代わりに「表示できない」と出る → スイート。
4. ゴーストと Overlay のスクショ → `--screenshot` (Release)。見た目の合否はユーザー。
5. `Editor.exe --selftest`、`tools\check_rules.ps1` (規則 16: 生の D3D 型を `ChangeTracking\` に持ち込まない) が通る。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`tools\check_rules.ps1`、`--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
