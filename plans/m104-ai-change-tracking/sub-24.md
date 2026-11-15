# sub-24: Inspector の Field 差分、Scene View のワイヤ箱、色の設定 (M104m2)

- 依存: sub-12
- 状態: 未着手
- 往復: 0
- 順序: sub-12 → **sub-24** → sub-13 → sub-14 (番号は末尾。sub-11 の VERDICT で、sub-12 から分けた)
- 書き込みの深さ: **着手可** (2026-11-15、sub-12 の VERDICT で planner が sub-12 の結果を書き足した)
- 質問の運用: 推奨で進め、Notion に後回し。

## 着手前の確認の扱い (推奨で裁定済み。計画の未決事項 5)

- 聞きたいこと: ハイライトの色を利用者が変えられるようにする範囲。
- **裁定 (推奨どおり)**: `EditorSettings` に 4 色 (追加・更新・削除・競合) だけ持つ。設定の画面は AI Changes 窓の歯車。色の設定を保存する前例は無い (テーマ色はコード内の定数)。
- 逆の答えが来たら差し戻す範囲: 「変えられない (定数)」なら色の保存と歯車の画面を外す (要求 §8.1 の「ユーザー設定で変更可能」を満たさなくなる)。「テーマ全体へ広げる」は別サブ。

## やること

sub-12 の `SceneChangeOverlay` (fileId → 状態の表。コンポーネントとフィールドごとの Before / After を持つ) を使う。**World を変えない。**

1. **Inspector の Field 差分**: `C:\HAL\MyEngin\src\Editor\Windows\Scene\InspectorWindow.cpp` の `DrawComponentFields` (1140 行目付近) のループ、`DrawField` の後。変わったフィールドに印を付け、ツールチップか横に Before の値 (字句のまま。長ければ切る) を出す。前例はプレハブの `*` マーカーとフィールドの右クリックメニュー。コンポーネントの見出しに、追加 / 変更の印。override の変更は override と分かる印。
   - C# コンポーネントは別の経路 (`DrawManagedComponentFields`、609 行目付近) で、フィールド単位のフックが無い。コンポーネント単位の印にとどめ、変わったフィールドの名前と Before をツールチップにまとめて出す (arch §7)。
   - Inspector の編集の操作 (値の変更、Undo の記録) に影響しない。印の描画は読むだけ。
2. **Scene View のワイヤ箱**: `C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewWindow.cpp` の `BuildOverlays` (519 行目付近) から `EditorLinePass::AddWireBox`。前例は `DrawSelectionOutline` (1677 行目付近。メッシュの AABB のワイヤ箱)。追加 = 緑、更新 = 青、競合 = 赤 (競合を作る経路は sub-15 / sub-16 までは無いので、このサブでは色と描画の口だけ)。輪郭ではなくワイヤ箱 (輪郭を出すポストプロセスは無い。arch §7)。
   - メッシュを持たないエンティティは、位置に小さい印 (既存のギズモの前例に合わせる)。
   - **可視のエンティティを優先し、1 フレームに描く本数に上限を置く** (要求 §14.1)。上限を超えたら、その旨を AI Changes 窓の帯に出す。
   - 生の D3D 型を `ChangeTracking\` に持ち込まない (規則 16)。`SceneViewWindow` の既存の経路から呼ぶ。
3. **色の設定**: `EditorSettings` に 4 色 (キーは 4 つ。既定値は今の定数)。AI Changes 窓の歯車のポップアップで変えられ、「既定に戻す」がある。Hierarchy のバッジ (sub-12 の定数) もこの設定を使う。
4. 文字列は英日。`C:\HAL\MyEngin\engine_spec.md` §9.2、`C:\HAL\MyEngin\docs\test_checklists.md`。

5. sub-12 の結果 (2026-11-15):
   - 表は `SceneChangeOverlay::Find(fileId)` で引ける。`components[].fields[]` に `before` / `after` / `lastRevisionId` / `overrideField`、ほかに `entityFields` (name / tagMask など)、`hierarchy` (parent / childIndex の差) がある。値は JSON の断片 (字句のまま)。表示用の切り詰めは `ChangeFormat` の `ClipValue` (UTF-8 の文字の境界で切る)。
   - 色は `SceneChangeOverlay.h` の `kOverlayAddedColor` / `kOverlayModifiedColor` / `kOverlayFaintAlpha` (定数)。これを `EditorSettings` の 4 色に置き換え、Hierarchy のバッジ (sub-12) も同じ設定を読むようにする。
   - **強調の表示の入・切 (sub-12 では保存していない) を、色と一緒に `EditorSettings` に保存する。**
   - AI Changes 窓の帯 (強調の切り替え、件数、ここまで確認済みにする) は `AiChangesWindow` にある。歯車はその帯に置く。
   - ヘッドレスの ImGui フレームで窓を描く試験の道具 (`WithHeadlessImGuiFrame`)、偽の I/O (`ChangeTrackingFakeIo.h`)、Hierarchy の「描いた数を数える口」の前例がある。Inspector と Scene View にも同じ形の口を足す。
   - Scene View のワイヤ箱は 3D の描画なので、ヘッドレスのスイートでは「`EditorLinePass` へ渡す箱の一覧を作る関数」(可視の判定、上限、色) を純粋な形に分けて試験し、実際の描画はスクショで確かめる。
   - `tools\track` は触らない見込み (触るなら、`tests\pipe.rs` の `unused_mut` の警告 1 件を直す)。

## やらないこと (このサブでは)

- ゴースト、Overlay の切り替え以上のもの、Before / After (sub-17、18)。復元の操作 (sub-16)。
- Engine 層の変更 (`EditorLinePass` に手を入れない。足りなければ「不安・質問」で返す)。

## 受け入れ条件 (このサブ)

1. Inspector と Scene View を、オーバーレイに追加・変更・競合を入れた状態と空の状態で描いて、`HashWorld`、エンティティ数、アーキタイプ数が同じ。`FileIdComponent` が付かない → `changetracking` スイート (ヘッドレスの ImGui フレーム) (要求 §16.5 の 5 番目)。
2. 変わったフィールドに印が出て、Before の値が字句のまま出る。変わっていないフィールドには出ない。C# コンポーネントはコンポーネント単位の印 → スイート (描いた印の数と文面)。
3. ワイヤ箱の本数が上限以下。可視のものが優先される。強調が切なら 0 本 → スイート (`EditorLinePass` へ渡した本数を数える口)。
4. 色の設定が保存され、読み直しても同じ。「既定に戻す」。バッジ・印・ワイヤ箱が同じ設定を使う → スイート。
5. Inspector でフィールドを編集する操作と Undo が、印の有無で変わらない (既存の undo のスイートが通る) → `Editor.exe --selftest`。
6. Play 中に強調を切り替えても sim の結果が変わらない → スイート。
7. スクショ (一時のプローブ): Inspector の Field 差分、Scene View のワイヤ箱 (緑と青)、歯車の色の設定、英語。**見た目の合否はユーザーの目視。**
8. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1` (規則 10、16、19) が通る。下位の層の差分が `LocalizationTable.inl` だけ。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`pwsh -NoProfile -File tools\check_rules.ps1`、`--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴