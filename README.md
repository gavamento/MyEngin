# MyEngine

[![CI](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml/badge.svg)](https://github.com/gavamento/MyEngin/actions/workflows/ci.yml)

C++20 / DirectX 11 製の自作ゲームエンジン。**Unity 風の使いやすさ × ECS の性能 × 壊れない開発体験** をコンセプトに、就職活動用ポートフォリオとして開発。仕様は [engine_spec.md](engine_spec.md)、設計判断の記録は [docs/adr/](docs/adr/) を参照。

## ビルドと実行

1. Visual Studio 2022 以降 (C++ デスクトップ開発ワークロード + Windows 10/11 SDK)
2. `MyEngine.sln` を開く → 構成 `Debug|x64` → F5

外部ライブラリはすべて `external/` にソースごとコミット済み (クローン → F5 で動く)。

| プロジェクト | 種類 | 内容 |
|---|---|---|
| Engine | 静的ライブラリ | Platform / Core / Renderer / Engine の 4 レイヤ |
| Editor | exe | ImGui エディタ (開発時のホストプロセス) |
| Runtime | exe | エディタ UI 無しの配布用ランタイム (エンジンは Editor と完全共有) |
| GameLogic | DLL | ユーザースクリプト。**ホットリロード対象** |

## 主要機能

- **ハイブリッド ECS** — 外部 API は `GameObject` / `GetComponent<T>()`、内部はアーキタイプ別 SoA。
  世代付き EntityID で破棄後のハンドルを検出。構造変更はコマンドバッファで tick 末一括適用
- **リフレクション基盤** — 1 つのフィールド表を Inspector 自動生成 / JSON シリアライズ /
  DLL リロード時の状態移行 / ワールドハッシュの 4 者で共用
- **ホットリロード** — シェーダ (include 依存グラフ + 失敗時は旧維持) / テクスチャ / glTF /
  シーン JSON (fileId 差分適用) / **C++ コード (GameLogic.dll)**。DLL は PDB ごとコピー +
  `/PDBALTPATH` でデバッガのブレークポイントを維持し、フィールドは名前+型一致で移行
- **パーティクル二重実装** — CPU (SoA + SSE、スカラー参照実装つき) と GPU (Compute、
  dead/alive リスト + DrawInstancedIndirect、リードバックなし)。実行時切替 + 並走比較モード。
  乱数は両者ともエンジンの決定論 RNG (GPU では乱数を生成しない)
- **レンダリングパス切替** — Forward / Deferred を実行時切替 (View > Render Path)。
  ライティング関数は common.hlsli を共用し見た目が一致。透明物とパーティクルは共通の Forward 後段
- **ハイブリッド・リアルタイムパストレーシング** — 一次光線はラスタのまま、**二次光線
  (拡散 GI / 平行光の影 / スペキュラ反射) を自前の BVH トラバーサルで置換**。
  Feature Level 11_0 縛りで DXR が使えないため `cs_5_0` のコンピュートシェーダで実装し、
  SVGF (テンポラル蓄積 → 分散推定 → A-Trous) でデノイズする。発光マテリアルはそのまま
  GI の面光源になる。詳細は [ADR-009](docs/adr/ADR-009-hybrid-path-tracing.md)
- **エディタの日本語化** — UI 言語は**日本語が既定**で、View > 言語 から実行時に英語へ切替。
  文字列は X マクロ 1 ファイルに集約し、訳の書き忘れを**コンパイルエラー**にする。
  ウィンドウ名は `"表示名###英語ID"` 形式なので、切り替えても ImGui の ID —
  つまり `imgui.ini` とドッキング配置 — は 1 バイトも変わらない。
  Inspector の表示名は `FieldDesc::displayName` に持ち、シリアライズキー兼ハッシュ入力である
  英語の `name` には触れない。詳細は [ADR-010](docs/adr/ADR-010-editor-localization.md)
- **Debug/Release 一貫性** — 固定 60Hz tick、`/fp:precise`、PCG32、明示ソートキー。
  リプレイ (.rep) の tick 毎ワールドハッシュ比較で機械検証:
  `tools\replay_verify.bat` が両構成ビルド → Debug 記録 → Debug/Release 照合 → 静的規則検査。
  **被覆は 3 シーン**: 既定デモ (物理 / パーティクル / スクリプト) / 部位ショーケース
  (スキンメッシュのボーン追従 = 骨駆動 LocalTransform の構成間ビット一致) /
  ゲームフロー統合デモ (シーン遷移・ポーズ・セーブ・アクションマップ)。
  割れた tick は**どのエンティティのどのフィールドが**割れたかまで自動で出る (`--hash-diff`)
- **CI (GitHub Actions)** — push ごとに 8 ビルド (4 プロジェクト × Debug/Release、警告 0 を強制) +
  リプレイ照合 3 ペア + 静的規則検査 + セルフテスト両構成 + 配布パッケージのスモークが回る。
  **GPU の無い runner でも回る**のは sim が CPU 専用だから — 描画は WARP
  (ソフトウェアラスタライザ) へ自動フォールバックし、ワールドハッシュはドライバに依らず一致する
  (WARP で録った .rep が RTX 3060 でそのまま照合できることを実測)

## エディタ操作

- **Scene ビュー**: 右ドラッグ + WASDQE (Shift で加速) — エディタカメラ
- **Play / Pause / Step**: メニューバー中央。Play 中の編集は Stop で破棄 (Unity 方式)
- **Game ビュー**: シーン内カメラ視点。Play 中は矢印キーで BoxTextured (プレイヤー) が移動、
  黄色い Spawned キューブに触れると回収 (GameLogic.dll の `OnTriggerEnter`)
- **Inspector**: リフレクションから widget を自動生成。スクリプトのフィールドもここに出る
- **Particle Settings**: CPU/GPU 切替・比較モード・SIMD トグル・更新時間表示

## CLI (検証/CI 用)

```
Editor.exe --selftest                     # ECS + シリアライザ回帰テスト
Editor.exe --replay-record out.rep --replay-ticks 600
Editor.exe --replay-verify out.rep        # exit code 0/1
Editor.exe --autoplay --deferred --frames 600 --screenshot shot.png
Runtime.exe --deferred --rt-demo --rt-gi --rt-shadow --rt-refl --rt-anim-seed
                                          # レイトレのショーケース (コーネル箱)
Editor.exe --parts-demo                   # 部位 (ソケット) のボーン追従シーン
Editor.exe --warp                         # WARP (ソフトウェアラスタライザ) 固定で起動
Editor.exe --package dist                 # 配布パッケージを CLI で作成 (exit code で成否)
tools\replay_verify.bat                   # 一貫性検証一式 (3 シーン被覆)
tools\check_rules.ps1                     # コーディング規則の静的検査
tools\gen_project_files.ps1               # ソース一覧を vcxproj に反映
```

CI (`.github\workflows\ci.yml`) は**この bat をそのまま呼ぶ** — CI 専用の検証ロジックは
書かない。CI 固有の事情は環境変数 3 本だけで注入する:

| 変数 | CI での値 | 用途 |
|---|---|---|
| `MYE_EXTRA_ARGS` | `--warp --no-audio` | 全 `Editor.exe` 実行へ後置 (GPU / 音源の無い runner 用) |
| `MYE_MSBUILD_ARGS` | `/p:MyeWarnAsError=true` | 警告 0 を強制 (既定 off。ローカル開発は止めない) |
| `MYE_DOTNET_ARGS` | `/p:TreatWarningsAsErrors=true` | 同上 (C# 側。綴りが違う) |

## 計測 (RTX 3060 / 1600x900 / Release)

レイトレ 3 レーンを全部 on にしたときの GPU 時間。GI と反射は内部 1/2 解像度、影はフル解像度。
`[rt]` ログの GpuTimer は最終フレーム 1 サンプルなので、同一条件 5 回の**最小値**を載せている。

| パス | 既定デモ (522 インスタンス) | コーネル箱 (11 インスタンス / 780 三角形) |
|---|---|---|
| BVH 構築 (CPU) | 0.20 ms | 0.02 ms |
| 拡散 GI (1spp) | 0.48 ms | 1.50 ms |
| テンポラル + SVGF | 0.35 ms | 0.55 ms |
| RT 影 (トレース + フィルタ) | 0.49 ms | 0.49 ms |
| RT 反射 (トレース + デノイズ) | 0.74 ms | 0.49 ms |
| **合計** | **約 2.1 ms** | **約 3.0 ms** |

インスタンス数が 1/50 でも閉じた箱の方が GI が 3 倍重い — **全レイがジオメトリに当たり
2 バウンス目と影レイまで必ず走る**ため。コストを決めるのは三角形数ではなくレイの平均行程。

## アーキテクチャ

```
Editor      ImGui エディタ (Hierarchy / Inspector / SceneView / Profiler ...)
GameLogic   ユーザースクリプト DLL — C ABI (src/Shared) だけを介してエンジンと通信
Engine      シーン / GameObject / ホットリロード制御 / パーティクル / リプレイ
Renderer    DX11 抽象 / IRenderPath (Forward・Deferred) / シェーダ管理
Core        ECS / リフレクション / RNG / ログ / FileWatcher / JSON
Platform    Win32 / 入力 / 時間 / DLL ロード
```

上位レイヤは下位レイヤのみに依存。生の D3D 型は Renderer 層より上に出さない。
DLL 境界 (`src/Shared/`) は C ABI + POD のみ (STL / vtable / 例外は越えない)。

## ドキュメント

- [docs/adr/](docs/adr/) — Architecture Decision Records (設計判断とトレードオフ)
- [docs/demo_script.md](docs/demo_script.md) — デモ動画の台本
- [docs/test_checklists.md](docs/test_checklists.md) — 手動テスト手順 (ホットリロード)
