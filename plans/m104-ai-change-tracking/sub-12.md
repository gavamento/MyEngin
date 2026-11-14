# sub-12: Hierarchy / Inspector / Scene View の表示 (M104m)

- 依存: sub-11
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く (計画の未決事項 5)。

- 聞きたいこと: ハイライトの色を利用者が変えられるようにする範囲。
- 推奨: `EditorSettings` に 4 色 (追加・更新・削除・競合) だけ持つ。設定の画面は AI Changes 窓の歯車。色の設定を保存する前例は無い (テーマ色はコード内の定数)。
- 逆を選ぶと何が変わるか: 「変えられない (定数)」なら設定の保存と歯車の画面が要らなくなり、要求 §8.1「表示色はユーザー設定で変更可能」を満たさない。「テーマ全体へ広げる」ならこのサブの範囲を超えるので別サブ。

## やること

arch §7、要求 §8.4 の初期段階。**World を変えない**のが存在条件。

1. `SceneChangeOverlay` (`src\Editor\ChangeTracking\`): (documentId, fileId) → 変更種別の表。開いているシーンの `documentId` と一致する分だけを引く。
2. Hierarchy のバッジ: `HierarchyWindow.cpp` の `DrawEntityNode` と `DrawFiltered` の**両方**。
3. Inspector の Field 差分: `InspectorWindow.cpp` の `DrawComponentFields` のループ、`DrawField` の後。Before の値 (字句) を見せる。C# コンポーネントはコンポーネント単位のバッジまで (arch §7)。
4. Scene View: `SceneViewWindow.cpp` の `BuildOverlays` から `EditorLinePass::AddWireBox`。緑 (追加) / 青 (更新) / 赤 (競合)。輪郭ではなくワイヤ箱 (arch §7)。可視のエンティティを優先する (要求 §14.1)。
5. 選択の同期: AI Changes 窓でシーンの変更をクリックすると、`Selection` を通して Hierarchy と Inspector が追従する。別のシーンの変更なら、選択せずにその旨を出す。
6. `fileId` は**読むだけ**の関数で引く。`Scene::EnsureFileId` を表示のために呼ばない。
7. 色の設定 (未決事項 5 の答えしだい)。文字列は英日。

## やらないこと (このサブでは)

- ゴースト、Overlay、Before / After (sub-17、18)。復元の操作 (sub-16)。
- ECS コンポーネント・イベントの追加。Engine 層の変更。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\src\Editor\ChangeTracking\SceneChangeOverlay.*`
- `C:\HAL\MyEngin\src\Editor\Windows\Scene\HierarchyWindow.cpp`、`InspectorWindow.cpp`、`SceneViewWindow.cpp`
- `C:\HAL\MyEngin\src\Editor\App\EditorSettings.h`、`LocalizationTable.inl`
- 参考: `ScmBadgeColor` / `DrawScmTileBadge`、`DrawSelectionOutline`

## 受け入れ条件 (このサブ)

1. オーバーレイに追加・更新・競合を入れた状態と空の状態で、`HashWorld` が同じ。アーキタイプの数とエンティティのコンポーネント構成も同じ (採番が起きていない) → `changetracking` スイート (要求 §16.5 の 5 番目)。
2. fileId の無いエンティティがいるシーンでオーバーレイを描いても、そのエンティティに `FileIdComponent` が付かない → スイート。
3. 開いているシーンと違う `documentId` の変更は、同じ `fileId` でもバッジが出ない → スイート。
4. バッジが通常表示と検索表示の両方に出る。Inspector に Before の値が出る。Scene View にワイヤ箱が出る → `--screenshot` (Release。偽データ)。見た目の合否はユーザー。
5. 一覧のクリックで `Selection` が対象の `fileId` になる → スイート。
6. Play 中にオーバーレイを切り替えても sim の状態が変わらない → スイート (切り替えの前後で `HashWorld`)。
7. `Editor.exe --selftest`、`tools\check_rules.ps1` が通る。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`tools\check_rules.ps1`、`--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
