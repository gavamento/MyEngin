# sub-18: Before / After のプレビュー、左右の 3D 比較 (M104s)

- 依存: sub-17
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-17 の着手前の確認 (Phase 6 へ進むか、sub-18 を外すか) の答えに従う。

## やること

要求 §9.2、§9.3。完了条件は「本番 Scene を変更せず、過去と現在の視覚比較ができる」。

1. 一時 `Scene` + 専用の `RenderSystem` + `RenderTexture` で、Before と After を別々に描く。前例は `AnimationPreviewWindow`、`AssetPreviewCache` (arch §7)。**実行中の Scene を巻き戻して表示しない。**
2. 表示モード: Before / After / Overlay / Side by Side。カメラは左右で同期する。
3. Before の内容は Blob の JSON から一時 Scene へ読む。現在の内容は、ディスクの After か、未保存の編集を含む Editor Snapshot かを選べるようにするかを着手時に決める。
4. アセットの再現性: プレビューが参照するメッシュ・テクスチャ・プレハブは**今の**アセットで、比較時点の状態とは限らない。Before の時点から変わっているアセット (追跡の履歴で分かる範囲) があれば、その旨を出す (要求 §9.3)。
5. 一時 Scene の読み込みが、`AssetDatabase` のキャッシュ・fileId の採番・ホットリロードの監視など、本番の状態に副作用を持たないことをコードで確かめる。
6. 生の D3D 型は描画境界の外へ出さない (規則 16)。プレビュー経路の許可リストへ足す必要があれば、理由を書き `engine_spec.md` §6.3 も更新する (AGENTS.md §4.1)。
7. 重い読み込みで無応答にしない (進捗か、分割)。

## やらないこと (このサブでは)

- 過去のアセットの内容を Blob から復元して描くこと (その旨を出すまで)。
- プレビューの中での編集。

## 触る場所 (planner の見立て)

- 新規: `C:\HAL\MyEngin\src\Editor\ChangeTracking\ScenePreviewCompare.*` か `src\Editor\Windows\Project\` の窓
- 参考: `AnimationPreviewWindow`、`AssetPreviewCache` (場所は着手時に確かめる)
- `C:\HAL\MyEngin\tools\check_rules.ps1` (規則 16 の許可リストが要る場合)、`C:\HAL\MyEngin\engine_spec.md`

## 受け入れ条件 (このサブ)

1. プレビューを開く・モードを切り替える・閉じる、の前後で本番の Scene の `HashWorld` と Undo の `StateSerial` が同じ (dirty にならない) → `changetracking` スイート。
2. Play 中にプレビューを開いても sim の結果が変わらない → スイート (開いた場合と開かない場合で、同じ Tick 数の後の `HashWorld`)。
3. Before が読めない (`version` 違い、Blob 欠落) とき、その旨が出て落ちない → スイート。
4. Before の時点から変わったアセットを参照しているとき、注記が出る → スイート。
5. 4 つのモードのスクショ → `--screenshot` (Release)。見た目の合否はユーザー。
6. デバイス消失の復旧 (M88) の後にプレビューが再生成される、または安全に閉じる → 既存の復旧のテストの形に合わせる。
7. `Editor.exe --selftest-full`、`tools\check_rules.ps1` が通る。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest-full`、`tools\check_rules.ps1`、`--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
