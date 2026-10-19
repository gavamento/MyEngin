# sub-06: Sliced / Tiled、SortingGroup2D、SpriteMask2D (ステンシル)

- 依存: sub-04
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.3、7. R3

## やること
1. `drawMode` 1 Sliced: border (px → UV と単位) で 9 分割、角は原寸 (border / ppu)、辺と中央は `size` に合わせて伸縮。`size` が角の合計より小さいときは角を縮める (Unity と同じ崩れ方で可)。border 全 0 なら Simple と同じ 1 枚。
2. `drawMode` 2 Tiled: 中央と辺を原寸で `size` の中に繰り返し、端のタイルは UV を切る (Continuous)。上限 4096 頂点 / スプライト (超えたら切って警告 1 回)。
3. `SortingGroup2DComponent` (NoHash、TypeId 末尾): `sortingLayerId / orderInLayer`。Sprite2DRenderer の収集で、各 Sprite2D の最も近い祖先の SortingGroup2D を探し (HierarchyComponent の親リンクを上る)、キーを (グループの層順, グループの order, グループの entity.index, 自分の層順, 自分の order, 自分の index) にする。グループ無しは (自分の層順, 自分の order, 0, 0, 0, index) ではなく、Unity と同じく「グループはグループのキー位置に 1 つの塊として入る」: キー = (層順, order, index) をグループなら**グループの値**、塊の内側は自分の値で 2 次ソート。
4. `SpriteMask2DComponent` (NoHash、TypeId 末尾): `sprite / alphaCutoff (0.5) / sortingLayerId / orderInLayer / rangeBack / rangeFront (int32、既定 INT_MIN / INT_MAX 相当の大きい値)`。Sprite2DPass の冒頭でステンシルを 0 でクリア (`ClearDepthStencilView(D3D11_CLEAR_STENCIL)` — 深度は触らない。読み取り専用 DSV に差し替わる前 (`RenderSystem.cpp:2097` の前) に Sprite2D パスが走ることを確認し、ステンシル書き込み可能な DSV を使う)。マスクは描画順にスプライトと一緒に並び、自分の位置で「alpha ≥ cutoff の画素にステンシル ref 1 を書く (色は書かない)」。`maskInteraction` 1 = Equal 1、2 = Equal 0 でテスト。マスクの `rangeBack / rangeFront` は「この order 範囲のスプライトだけマスクされる」= 範囲外のスプライトはステンシルテスト無しで描く。複数マスクの重なりは OR (ref 1 を書き重ねる) で可。
5. Scene View のギズモ: SortingGroup2D は無し、SpriteMask2D は矩形 (紫)。EditorComponentCatalog に 2 行。`kSimSnapshotVersion` +1 (型 2 つ追加で 1 回)。
6. 確認スクショ: Sliced (border 4 px の 16×16 を 3×1 単位)、Tiled (同じ絵を 3×2)、SortingGroup (グループ A の子 2 枚の間にグループ外の 1 枚が割り込まない)、Mask (丸マスクの VisibleInside / Outside)。

## やらないこと (このサブでは)
- Tile Mode = Adaptive、ソフトマスク、Sprite Sort Point

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp` (SortingGroup2D / SpriteMask2D)、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp` (9 分割 / タイル / キー)、`src\Engine\Renderer\Passes\Sprite2DPass.cpp` (ステンシル状態 3 種: 書き / Equal1 / Equal0 / 無し)
- `RenderSystem.cpp:2047-2100` (DSV の順序)、`SceneViewWindow.cpp` (ギズモ)
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`
- 参考: `src\Engine\Renderer\Device\RenderTexture.cpp:55-59` (D24S8)、`SwapChain.cpp:76-86`

## 受け入れ条件 (このサブ)
1. (spec 15) 4 枚のスクショ (`plans\m94-2d\screenshots\sprite2d_sliced_tiled.png`、`sprite2d_group_mask.png`) で、角の画素幅が原寸 (4 px × 画面倍率)、Tiled の繰り返し数、グループの塊、マスクの内外が期待どおり (画素座標のログ)。
2. SceneView と GameView と Runtime の 3 経路でマスクが同じに出る (SceneView のエディタスクショ 1 枚)。
3. (spec 1-5) golden 不変 (ステンシルクリアは Sprite2D が 0 枚なら実行しない)、0 警告、check_rules 0、snapshot +1 のみ。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`
- temp シーンの `Runtime.exe ... --screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
