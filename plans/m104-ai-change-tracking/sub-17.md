# sub-17: 削除されたエンティティのゴーストと、Before の読み取り専用の表示 (M104r)

- 依存: sub-16
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-16 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。

## 着手前の確認の扱い (推奨で裁定済み。spec §2 D12、§6.2)

- 「Phase 6 (P2) へ進むか」→ **進む** (2026-11-15 の裁定。進捗表どおり sub-17 → sub-18 → sub-19 → sub-20)。逆の答えが来たら、sub-17〜19 を行わず、sub-20 (配布) の依存を sub-16 に付け替える。A17〜A19 が落ちる。

要求 §8.2、§8.4 の拡張段階、§16.3 の 3 番目 (Entity 削除時に Before を表示できる)。arch §7。**World にエンティティを作らない。表示のために World を書き換えない。**

## 使えるもの

- 削除されたエンティティの Before: `scene_diff` の `EntityRemoved` の行の `before_value` (エンティティの JSON の断片。字句のまま。`TrackSceneChange::beforeValue`)。`scene_overlay` (sub-12) の `entities[].status = removed`。
- ゴーストの描画: `GhostMeshPass::Add(const Mesh* mesh, const XMFLOAT4X4& world, uint32_t rgba)` (`C:\HAL\MyEngin\src\Engine\Renderer\Passes\GhostMeshPass.h` 33 行目)。前例は `C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewGhost.cpp` (分岐ゴースト、M72e / M72i。半透明 40%、メッシュが引けない物はワイヤ箱、SceneView の RT にだけ描く、読むだけ)。
- 強調の色: sub-24 の `OverlayPalette` の「削除」の色。表示の入・切: `SceneChangeOverlay::Show()`。
- Scene View の箱の候補作り: sub-24 の `SceneChangeWireBoxes` (純粋な関数。可視・上限 256・近い順)。ゴーストはライブに居ないので、この候補には入らない (別の経路。sub-24 の申し送り)。

## やること

### A. Before から描くための情報を取り出す (`src\Editor\ChangeTracking\RemovedEntityGhosts.h/.cpp`、新規。純粋な部分と、アセットを引く部分を分ける)

1. `EntityRemoved` の `before_value` (JSON の断片) から、**World を通さずに**次を読む: 名前、`fileId`、親の `fileId`、`LocalTransform` (位置・回転・拡大)、`MeshRendererComponent` のメッシュの参照 (アセットの guid / パス)。字句を保つ読み方は不要 (表示だけなので `nlohmann::json` で読んでよい)。読めない・コンポーネントが無いものは「位置だけ」「描けない」として扱う。
2. **ワールド行列**: 削除されたエンティティの親の連鎖を、(a) 同じ変更で一緒に消えた親 (同じ Revision の `EntityRemoved`) の Before の `LocalTransform`、(b) ライブのシーンに今いる親の今のワールド行列 (`FileIdComponent` を読むだけで引く。採番しない)、の順でたどって合成する。親が見つからなければ、ルートとして扱い、その旨を注記にする (「親の位置が分からないので、ローカルの位置で描いています」)。
   - **一時の `Scene` / World にエンティティを作って変換を計算する方法は採らない** (World を書かないことの確かめが難しくなり、ECS の登録・アーキタイプの生成が走る)。行列の合成は、既存の変換の式 (`LocalTransform` から行列を作る関数) を読むだけで使う。合成の結果が、同じエンティティがライブにいたときの `WorldTransform` と一致することを試験で確かめる (エンティティを消す前に記録しておき、消した後に Before から計算した行列と比べる)。
3. メッシュ: 参照から、今読み込まれているメッシュを引く (読むだけ。新しく読み込まない。読み込まれていなければワイヤ箱か印)。**アセットの今の内容で描く** (削除の時点のメッシュとは限らない)。参照先のメッシュが Before の後に変わっていれば (追跡の履歴で分かる範囲。分からなければ分からないと出す)、注記に「メッシュは今の内容で描いています」。
4. スキンメッシュ・パーティクル・ライト・UI など、メッシュでない描画のコンポーネントは、ワイヤ箱か位置の印にとどめる。

### B. Scene View への描画 (`SceneViewWindow` の既存の経路から)

- 描くのは SceneView の RT だけ (GameView、撮影の経路、Runtime には出さない。`SceneViewGhost.cpp` と同じ)。
- 半透明のメッシュ (`GhostMeshPass::Add`)。色は `OverlayPalette` の「削除」に、不透明度を下げたもの。メッシュが引けないものはワイヤ箱 (`EditorLinePass`)。
- 可視のものを優先し、1 フレームに描く数に上限 (sub-24 と同じ形。超えたら帯に注記)。
- 強調の表示が切 (`Show()` が偽)、追跡が無効・未接続のときは 1 つも描かない。
- 生の D3D 型を `ChangeTracking\` に持ち込まない (規則 16)。`GhostMeshPass` を呼ぶのは `SceneViewWindow` / `SceneViewGhost` 側 (既に規則 16 の許可リストにある経路) にして、`ChangeTracking\` からは「何を、どの行列で、どの色で」の一覧だけを渡す。許可リストへの追加が要るなら、理由を書き `engine_spec.md` §6.3 も更新する (AGENTS.md §4.1)。

### C. 読み取り専用の Before の表示

- ゴーストをクリック (Scene View のピック) するか、AI Changes 窓の「シーンの変更」の一覧で `EntityRemoved` の行を選ぶと、Before の内容 (名前、親、コンポーネントとフィールドの値。字句のまま) を**読み取り専用**で見られる。`Selection` には入れない (実体が無い)。表示は AI Changes 窓の詳細の列か、小さい別の窓。編集の操作 (値の変更、コンポーネントの追加) は出さない。
- 「このエンティティを復元…」の導線 (sub-16 のシーンの復元の、Entity の削除の取り消し) へつなぐ。
- Scene View のピックは、既存のピックの経路を変えずに、ライブのエンティティに当たらなかったときだけゴーストを調べる (ライブの選択の挙動を変えない)。難しければ、ピックは入れず一覧からの選択だけにして理由を書く。

### D. Scene Overlay の入・切 (要求 §8.4 の拡張段階)

- 既存の強調の入・切 (sub-24 で保存される) に、ゴーストの表示の入・切を足す (帯のチェック。保存する)。未確認 (`unknown` / `inferred`) の変更のアイコン (要求 §8.1「未確認 = アイコン」) を Scene View の強調に出す: sub-24 のワイヤ箱の色分けに、帰属が未確認のものの印を足す (色ではなく小さい印。Hierarchy のバッジにも同じ印)。

### E. 文書

- arch §7、`C:\HAL\MyEngin\engine_spec.md` §9.2 (と、規則 16 の許可リストを変えたなら §6.3)、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表)。

## やらないこと (このサブでは)

- Before / After の独立の表示、左右の 3D 比較 (sub-18)。プレハブ伝播 (sub-19)。
- World・ECS への一時のエンティティの生成。メッシュの新しい読み込み。過去のアセットの内容の復元。
- Engine 層の変更 (`GhostMeshPass`、`EditorLinePass` に手を入れない。足りなければ「不安・質問」で返す)。変わってよいのは `LocalizationTable.inl` だけ。

## 受け入れ条件 (このサブ)

1. ゴーストを出した状態と出していない状態で、本番の Scene の `HashWorld`、エンティティ数、アーキタイプ数が同じ。`FileIdComponent` が付かない。メッシュの読み込みが新しく起きない → `changetracking` スイート。
2. Before から計算したワールド行列が、消す前にライブで記録した `WorldTransform` と一致する (親が同じ変更で消えた場合、親がライブにいる場合、親が見つからない場合 = 注記)。誤差は既存の変換の試験と同じ許容 → スイート (純粋な関数)。
3. 削除されたエンティティの Before の内容 (名前、コンポーネント、フィールドの値) が、読み取り専用で表示される。編集の操作が出ない・効かない。`Selection` が変わらない → スイート (ヘッドレスの ImGui フレーム) (要求 §16.3 の 3 番目)。
4. Before の行が読めない / Blob が欠けている / メッシュの参照が引けない、のそれぞれで、ゴーストの代わりにワイヤ箱か「表示できない」が出て、落ちない → スイート。
5. 描く数の上限、可視の優先、強調が切・追跡が無効のとき 0 → スイート (`ChangeTracking\` が渡す一覧の数を数える口)。
6. 未確認の変更の印が、Scene View の強調と Hierarchy のバッジに出る。確定 (`verified` / `claimed`) の変更には出ない → スイート。
7. Play 中に表示を切り替えても sim の結果が変わらない → スイート。
8. スクショ (一時のプローブ): 削除されたエンティティのゴースト (メッシュ・ワイヤ箱)、Before の読み取り専用の表示、未確認の印、英語。**見た目の合否はユーザーの目視。**
9. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1` (規則 10、16、19)、`cargo test` (track)、`tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release
- `pwsh -NoProfile -File tools\check_rules.ps1`
- スクショ: 一時のプローブ + `bin\x64\Release\Editor.exe --frames 40 --shot-frame 36 --screenshot`。プローブはコミットしない
- exe の実行は PowerShell から。試験は一時プロジェクトか偽のデータで行う。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。Windows のパスを含むコードは Write / Edit ツールで書く (Bash のヒアドキュメントはバックスラッシュを潰す)。

## 実装メモ (coder が追記)

## フィードバック履歴
