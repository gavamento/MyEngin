# sub-03: Deferred フォワード段・速度・CSM 影

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること

spec §4.1「パスへの組み込み」の Deferred 行と CSM 行を実装し、**頂点変位が深度・速度 (TAA)・影に反映される**ことを実画像で示す。動機 (TAA 付き動画) の本丸。

- DeferredPath:
  - GBuffer ループ (`DeferredPath.cpp:877-970`) とインスタンス run 構築 (`DeferredPath.cpp:844-876` 付近) から**不透明サーフェスのアイテムを除外**
  - **SSR の後・水面の前** (`DeferredPath.cpp:730-735` の 2.6 と 2.7 の間) に「サーフェス段」を追加: RT = HDR シーン (`view.rtv`) ＋ `gbVelocity_`、DSV = 既存 (テスト＋書き込み)、サーフェスプログラムの**速度エントリ**で描く。前履歴なし (`view.prevViewProjValid == 0`) は velocity 0 (既存 GBuffer と同じ)。前 World は `RenderItem::prevWorld`
  - サーフェスのアイテムが 0 件ならこの段は RT / ステート / SRV を一切触らない
  - 透明サーフェスは既存の透明段 (`DeferredPath.cpp:1350-`) で色エントリ (sub-02 のバインドを流用)
  - 失敗時の `surface_error` もこの段で描き、剛体の速度 (prevWorld / prevViewProj) を書く
- ShadowPass (CSM): サーフェスの不透明アイテムは**影エントリ** (ライト VP を `gViewProj` に入れて `VSMain`、PS なし) で描く。インスタンス run から除外。深度バイアス等のステートは既存どおり。失敗時は従来の `shadow_depth` (変位なし) でよい
- ShadowAtlas (スポット/ポイント) は変更しない (従来シェーダ = 変位なしで描かれる。spec §3 後回し)
- `docs/` に「Deferred のサーフェス画素には SSAO / SSR / デカール / RT 受光が掛からない」「アトラスの影は変位なし」を追記 (既存の M78 docs の近くに。新規ファイルでもよい)
- 速度エントリの GPU 時間 (変位ありの平面 1 枚) を実装メモに記録 (spec §4.4、最適化はしない)

## やらないこと (このサブでは)

- Forward パスへの velocity / TAA 追加
- GBuffer 作者規約、SSAO / SSR / デカールをサーフェスに効かせること
- シャドウアトラスの変位込み影、アルファクリップ影
- WaterWave (sub-05)

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/DeferredPath.cpp/.h` — `Render` の段の並び (`DeferredPath.cpp:716-737`)、`RenderGeometry`、新しい段の関数。MRT のブレンド状態 (`IndependentBlendEnable=FALSE` の前提、`DeferredPath.cpp:760-761`) と R16G16F の組み合わせを確認
- `src/Engine/Renderer/ShadowPass.cpp` — `Render` (`ShadowPass.cpp:117-`) のループとインスタンス run
- `src/Engine/Renderer/TaaPass.*` — 変更は不要の見込み (velocitySRV を読むだけ)。触るなら理由を書く
- 検証用の一時シーン: 変位 (`gTime` 駆動のサイン波) ありの平面と、それを受ける床、静止カメラ、太陽 1 本

## 受け入れ条件 (このサブ)

1. Deferred で不透明サーフェスが描かれ、深度を書く (後段の水面・透明・パーティクルが正しく前後する) — スクショ
2. `gTime` 駆動の変位メッシュが、カメラ静止でも `--velocity-debug` に動きとして出る。変位なし版では出ない — `Runtime.exe --deferred --velocity-debug --screenshot` の 2 枚
3. `--taa` 有効時、変位メッシュの縁に前フレーム形の残像が出ない — `--deferred --taa --screenshot` (決定的撮影で数十フレーム進めた時点) と、比較用に「速度を書かない」一時改変版の 1 枚 (比較版はコミットしない)
4. CSM の影が変位後の形で落ちる — 変位あり/なしの 2 枚
5. サーフェスの無いシーンは Deferred の絵が不変 (フォワード段が何も張らない) — `--selftest` (既存 golden)、`tools\replay_verify.bat`

## 検証コマンド

```
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <一時> --scene <一時> --deferred --velocity-debug --screenshot <png>
Runtime.exe --project <一時> --scene <一時> --deferred --taa --screenshot <png>
```

`replay_verify` が割れたらメモリ `replay-verify-triage.md` の手順で切り分けてから報告すること。画質系スクショはメモリ `rt-screenshot-freeze-seed-trap.md` の freeze-seed に注意。

## 実装メモ (coder が追記)

## フィードバック履歴
