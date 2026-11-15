# sub-24: Inspector の Field 差分、Scene View のワイヤ箱、色の設定 (M104m2)

- 依存: sub-12
- 状態: OK (commit は台帳 harness.md を参照)
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

SELF_EVAL: sub-24 (round 1)
実装:
  - src\Editor\ChangeTracking\OverlayPalette.h (新規) — `OverlayColor` / `OverlayColorId` (Added, Modified, Removed, Conflict) / `OverlayPalette` (4 色、既定値、`Reset`、`==`) / `PackOverlayRgba` (0xRRGGBBAA)。`kOverlayFaintAlpha` もここへ。`kOverlayAddedColor` / `kOverlayModifiedColor` は削除して置き換えた
  - src\Editor\ChangeTracking\SceneChangeOverlay.h — `Palette()` / `SetPalette` / `PreviewPalette` / `CommitPalette` / `LoadPrefs` / `SetPrefsSink`、`SetShow` が変化時に受け口へ知らせる、`NoteWireBoxes` / `BoxStats` (Scene View が描いた数と省いた数)、`Entities()` (表の全件)
  - src\Editor\App\EditorSettings.h/.cpp — `aiChangeShowOverlay` と `aiChangePalette` (キー 4 つ `aiChangeColorAdded` / `Modified` / `Removed` / `Conflict`、各 [r,g,b,a])。壊れた値は無視して既定のまま
  - src\Editor\App\EditorApp.cpp — 起動時に `LoadPrefs`、`SetPrefsSink` で `settings_` へ書いて `Save`。Inspector と Scene View へ overlay を渡す
  - src\Editor\Windows\Scene\HierarchyWindow.cpp — バッジの色を `Palette()` から読む
  - src\Editor\Windows\Scene\InspectorChangeMarks.h/.cpp (新規) — 見出しの右端 (プレハブの「+」の左) とフィールド行の右に小さい点を描き、ホバーでツールチップ。描画専用 (World・Undo に触れない。直前のアイテムの状態を読まない)。描いた数を数える口 `FieldMarks()` / `ComponentMarks()`
  - src\Editor\Windows\Scene\InspectorWindow.h/.cpp — `SetChangeOverlay`、`DrawComponent` の見出しの直後に `DrawComponentMark` (C# は `row.managed` でフィールド一覧つき)、`DrawComponentFields` のループ末尾 (編集・Undo・プレハブの処理のあと) に `DrawFieldMark`
  - src\Editor\ChangeTracking\ChangeFormat.h/.cpp — `FindOverlayComponent` / `FindOverlayField` / `FieldMarkTooltip` (変更前・変更後を字句のまま、`kInspectorValueMaxChars` = 120 で切る、override は見出しを変える) / `ComponentMarkTooltip`
  - src\Editor\ChangeTracking\SceneChangeWireBoxes.h/.cpp (新規) — 純粋な関数 `CollectChangeBoxCandidates` (World は読むだけ。強調が切なら空。視錐台の判定は中心点 + 1.5 倍の余裕) と `SelectChangeBoxes` (見えているものだけ、近い順、同じ距離は fileId 順、`kMaxChangeWireBoxes` = 256 で切って省いた数を返す)。生の D3D 型なし
  - src\Editor\Windows\Scene\SceneViewWindow.h/.cpp — `SetChangeOverlay`、`AppendChangeBoxes` (メッシュがあれば AABB の `AddWireBox`、無ければ `AddWireSphere`。深度テストあり)。`BuildOverlays` から呼ぶ。数は `ChangeBoxesDrawn()` / `ChangeBoxesOmitted()` と overlay の `NoteWireBoxes` へ
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp — 帯の歯車 (`DrawColorSettings`: 4 色の `ColorEdit4` と「既定に戻す」。ドラッグ中は表示だけ、マウスを離したら 1 回保存)、件数の色を palette から、上限超えの注記 (`BoxStats().omitted`)
  - src\Engine\Core\Localization\LocalizationTable.inl — 英日 11 件 (`AiChg_InspBefore` ほか)。下位の層の差分はこれだけ
  - src\Editor\ChangeTracking\SceneChangeOverlaySelfTest.cpp — 試験 5 本追加 (下の検証)
  - build\Editor.vcxproj / .filters — 新規 5 ファイル (gen_project_files.ps1)
  - docs: docs\test_checklists.md (M104m2 の手動確認)、engine_spec.md §9.2、docs\ai-change-tracking\architecture.md §7 (実装の記録)
仕様との差分:
  - [逸脱] ワイヤ箱の「可視を優先」は、見えていない候補を**描かない**形にした (優先ではなく除外)。画面外の線は積んでも見えず、スロットを使うだけのため。見えているかは**エンティティの原点**が画面の ±1.5 倍に入るか (大きい箱の原点が画面外でも枠が見える場合を拾うための余裕。原点が大きく外れた巨大な箱は出ない)
  - [未実装] Inspector のエンティティ属性 (name / tagMask の `entityFields`) の印は出していない。名前欄は幅いっぱい (`SetNextItemWidth(-1)`) で、横に置き場が無いため。コンポーネントのフィールドとコンポーネントの見出しだけ
  - [追加] 競合: `OverlayStatus` は触らず、色と描画の口を `OverlayColorId::Conflict` と `ChangeBoxCandidate::color` に置いた (競合を作る経路は sub-15 / sub-16 まで無い)。既定は赤 (0.92, 0.22, 0.22)。削除の既定はテーマのエラー色と同じ値
  - [追加] 色の編集は `ColorEdit4` (アルファ付き)。ドラッグ中は `PreviewPalette` で表示だけ変え、マウスの左ボタンを離したフレームに `CommitPalette` で保存する (1 ドラッグで何度もファイルを書かない)。設定の画面は仕様どおり帯の歯車だが、帯は追跡がつながって一覧が出る状態 (Ready 等) でしか描かれない
  - [追加] Scene View の箱は深度テストあり (選択の橙の枠は従来どおり最前面)。手前の物に隠れてよい、という判断
  - [追加] 上限 256 本を超えたときの注記を帯に出す (仕様の「その旨を帯に」)。件数は overlay の `NoteWireBoxes` 経由
  - [追加] `SceneChangeOverlay::Entities()` (表の全件) を足した。走査順は不定なので、順序に使う側が並べ直す
  - [追加] docs\ai-change-tracking\architecture.md §7 にも実装の記録を足した (仕様が求めたのは engine_spec.md §9.2 と test_checklists.md)
検証:
  - MSBuild build\Editor.vcxproj Debug|x64 / Release|x64 (/p:MyeWarnAsError=true) → どちらも成功 (警告 0)。プローブを外したあとにもう一度ビルドした
  - MYE_SELFTEST_ONLY=changetracking (Debug) → PASS。新しい試験は 74 項目前後が全部 PASS (実際の Inspector をヘッドレスの ImGui フレームで描いて印 1 + 見出し 1、World / エンティティ数 / アーキタイプ数が同じ、fileId が付かない、強調が切なら 0、空の表と同じ World、追加コンポーネントは見出しだけ、ワイヤ箱 275 候補で 256 本 + 省略 20 + 後ろ・不在は数えない、強調が切なら 0 本、走査順に依らない、palette の保存・読み直し・既定に戻す・壊れた値・ドラッグ中は保存しない、Hierarchy のバッジ、帯 + 歯車 + 省略の注記を英日で描く)
  - bin\x64\Debug\Editor.exe --selftest → **5 回中 2 回が 112 スイート 0 失敗 (75 秒・85 秒)、3 回は changetracking だけ失敗 (1 失敗)**。失敗したのは `poll cap: it spreads over many frames` と `hunks: ...` / `model: describe is stored` (TrackClient の Poll の予算が実時間の 2 ms = steady_clock で、機械が重いと 1 回の Poll で進む行が減る。実行のたびに失敗する項目が入れ替わる)。このサブは TrackClient / ChangeModel に触れていない。失敗した回はユーザーのゲーム (KingdomCome) が動いていて機械が重く、スイート全体が 75 秒 → 134〜145 秒に伸びていた。changetracking だけの実行も 4 回中 3 回失敗・1 回成功。**静かな機械での再確認は未実施 (ユーザーの環境の負荷は止められない)**。失敗項目は私の追加した試験より前に走るもの
  - bin\x64\Release\Editor.exe --selftest → 112 スイート 0 失敗 (changetracking PASS)
  - tools\check_rules.ps1 → 0 error (66 warning。sub-12 の記録は 65 で、増えた 1 件の出所は未特定。触ったファイルを grep した一覧に、新規ファイルや私の追加行の指摘は無く、規則 10・16・19 の指摘は 0)。`git diff --stat -- src\Engine` は LocalizationTable.inl だけ (+15)
  - スクショ (一時のプローブ + Release の Editor.exe --screenshot --shot-frame 30 --frames 40。プローブは外した。grep で残っていないことを確認): C:\HAL\MyEngin\cache\m104m2_shots\1_ja.png (Hierarchy・Scene View・Inspector。Scene View は Floor に緑の箱、Player / Ball / Main Camera 側に青。Inspector は Transform の見出しと position / scale に青い点、Collider の見出しに緑の点、日本語)、3_english.png (英語)、2_popup_ja.png (歯車のポップアップ = 4 色と「既定に戻す」。**帯は追跡が Ready でないと描かれないので、プローブの小窓に `DrawColorSettings` だけを出して撮った**)。撮れた・文字が切れていない・化けていない・ImGui のエラー表示が出ていない、まで確認。**見た目の合否は目視 (未確認)**。ホバーのツールチップ・帯の省略の注記・Rigidbody の点 (スクロールの外) は画像に写っていない
  - 未実行: replay_verify (このサブの検証コマンドに無く、World を書く経路を足していない)、--selftest-full、cargo test / track_verify (tools\track を触っていない)、実際の Ctrl+Z / フィールドのドラッグ編集 (既存の undo のスイートは --selftest の中で PASS)、本物の Play 中 TickRunner (2 つのシーンを同じ決定的な 60 Tick 回して形を比べる形)
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜4・6・8 を機械で確認。条件 7 は撮れたが目視は未確認、条件 5 は既存の undo のスイートの通過のみ。見えているかの判定 (原点 ±1.5 倍) と、エンティティ属性の印の未実装が差分
  正しさ: 3 — 新しい試験は全部通り、実際の Inspector を描いて World が変わらないことまで確認した。ただし Debug の selftest が負荷の下で 5 回中 3 回、このサブ以外の時間依存の項目で落ちた (静かな機械での再確認が残る)。ホバーと本物の編集操作は人手の確認が残る
  コード品質: 4 — 純粋な関数 (候補の選び方) と描画を分け、色の設定を 1 か所 (`OverlayPalette`) に集約。`SceneChangeOverlay` は設定・箱の数・表の 3 つを持つ分だけ大きくなった。`DrawFieldMark` は `SameLine` + `Dummy` で行の高さに点を合わせる単純な実装 (目視未確認)
  テスト: 4 — Inspector は実物をヘッドレスで描き、箱は純粋な部分と `AppendChangeBoxes` の両方、palette は保存の往復と壊れた値まで。本物の GPU 描画・ホバー・Undo の操作は試験に無い
不安・質問:
  - Debug の changetracking スイートが機械の負荷で落ちる件 (上の検証)。TrackClient の Poll の予算 (2 ms の実時間) を使う項目は時間に依存する。sub-24 の範囲外だが、CI で落ちうるなら別サブで直すか。静かな機械での通しの再確認を司会かユーザーにお願いしたい
  - 箱の可視判定が原点だけ (±1.5 倍) で良いか。大きいエンティティ (床など) は原点が離れると枠が出ない。AABB の 8 角で判定すると正確だが、メッシュの AABB を全候補で引く必要があり、1 フレームのコストが増える (上限 256 を先に切る今の形が崩れる)。計測はしていない
  - 帯 (と歯車) は追跡がつながっていないと出ないので、追跡が無効の人は色を変えられない。色の設定は追跡と独立でもよいか (今は仕様どおり「帯の歯車」)
  - Scene View の箱は `FindByFileId` を表の全件に毎フレーム引く (変更が数万件あると 1 フレームに数万回のハッシュ引き)。計測はしていない。数千件を超えるシーンの計測は sub-19
触ったファイル:
  - src\Editor\ChangeTracking\OverlayPalette.h (新規)、SceneChangeWireBoxes.h / .cpp (新規)、SceneChangeOverlay.h、ChangeFormat.h / .cpp、SceneChangeOverlaySelfTest.cpp
  - src\Editor\Windows\Scene\InspectorChangeMarks.h / .cpp (新規)、InspectorWindow.h / .cpp、SceneViewWindow.h / .cpp、HierarchyWindow.cpp
  - src\Editor\Windows\Project\AiChangesWindow.h / .cpp
  - src\Editor\App\EditorApp.cpp、EditorSettings.h / .cpp
  - src\Engine\Core\Localization\LocalizationTable.inl
  - build\Editor.vcxproj、build\Editor.vcxproj.filters
  - docs\test_checklists.md、engine_spec.md、docs\ai-change-tracking\architecture.md
  - plans\m104-ai-change-tracking\sub-24.md (実装メモのみ)
  - 生成物 (ステージ不要): C:\HAL\MyEngin\cache\m104m2_shots\ の 1_ja / 2_popup_ja / 3_english の .png / .log / .err (このサブで作った。消していない)
消した・止めたもの: なし。作った一時物 (消していない): cache\m104m2_shots\ の上記、スクラッチの build ログとプローブ用スクリプト (セッションのスクラッチディレクトリ)。試験が作る一時ディレクトリは試験内で消している。起動した Editor.exe は全て自分で終了 (--frames 付き)。既存の %TEMP%\mye_track_* と %TEMP%\mye_m104b\ と cache\m104k_shots / m104k2_shots / m104m_shots には触れていない
申し送り:
  - sub-13 以降: 色・強調の入切は `SceneChangeOverlay` の `Palette()` / `Show()` が唯一の読み口。競合を作るサブ (sub-15 / 16) は `OverlayColorId::Conflict` と `ChangeBoxCandidate::color` へ値を流すだけでよい (Inspector の印は今は追加 / 変更の 2 色)
  - ゴースト (sub-17) は `SelectChangeBoxes` の候補に削除エンティティを足す形にはならない (ライブに居ない)。別経路で `GhostMeshPass` へ
  - tools\track は触っていない。pipe.rs の unused_mut は残ったまま
  - Debug の changetracking の時間依存 (poll cap / hunks / model describe) は、機械が重いと落ちる。`kTrackPollBudget` が実時間の 2 ms のため

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、下位の層の差分は `LocalizationTable.inl` (15 行) だけ、`tools\track` / `tools\mcp` / `src\Engine\Renderer` に差分なし (`EditorLinePass` を触っていない)、`getenv` / `EnsureFileId` / `GuidForPath` の追加なし、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。planner の実走 (CPU 使用率 100%、ユーザーの別のプログラムが動いている状態): Release の `--selftest` 全体は 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`tools\check_rules.ps1` は 0 error / 66 warning。**Debug の `MYE_SELFTEST_ONLY=changetracking` は 5 回とも失敗** (`poll cap`、`model: describe is stored`、`hunks: …` の 1〜8 件)。
  - この失敗は sub-24 の変更によるものではない: 落ちた項目は全部 sub-09 / sub-10 の試験で、原因は `TrackClient.cpp` 178・199 行目が `Poll()` の時間の上限を `steady_clock` で直接測っていること (planner がコードで確認)。sub-24 が足した試験は、coder の報告では全部通っている。**sub-24 は OK とし、直しは新しい sub-25 (M104m3) で先に行う** (spec §2 D37)。sub-24 の受け入れ条件 8 のうち「Debug の `--selftest` が通る」は、sub-25 の後で確かめ直す。
  - `check_rules` の warning が 65 → 66 の出所: 規則 7 (ハッシュコンテナの走査。参考の警告) が `src\Editor\ChangeTracking\` に 5 件あり (`SceneChangeOverlay.cpp` 106・401・414 行目、`SceneChangeOverlaySelfTest.cpp` 350・459 行目)、このサブで 1 件増えた。どれも Editor の表示側で、sim の順序に関係しない。error ではないので合否に影響しない。
  - 逸脱・未実装の判定: 可視の判定をエンティティの原点で行う (大きいエンティティは原点が画面から外れると枠が出ない)、Inspector の `entityFields` (name / tagMask) の印が無い、は可。原点での判定の限界は sub-19 の計測のときに、AABB で判定する費用と一緒に見直す。
  - 質問への回答: (1) 時間に依存する試験は sub-25 で直す。(2) 箱の可視の判定は原点のままでよい (上のとおり sub-19 で見直す)。(3) 色の設定は帯の歯車のままでよい (追跡が無効なら強調そのものが出ないので、色を変える場面が無い)。(4) 候補作りの計測は sub-19。