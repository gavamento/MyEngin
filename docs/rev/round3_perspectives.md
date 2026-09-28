# レビュー round 3 観点記録 (エンジン全域深層監査)

- 対象: Core / ECS、Net / Rollback / Replay、Asset / Loader、Renderer / D3D11、Physics / Scripting 全域
- 日時: 2026-09-28
- 作成先: `C:\HAL\MyEngin\docs\rev\round3_perspectives.md`

---

## round 3 で意識した観点

round 1（破壊システム基盤）および round 2（破壊力学・幾何・外部連携）に続き、本 round 3 では**「エンジン全域にわたる深層不変量の検証」**をテーマとし、5つの専門観点を設定して並行・網羅的に監査を実施した。

### 観点9: ネットワーク・ロールバックネットコード・スナップショット同期 (Network Rollback, Snapshot Sync & Output Lanes)
- **ロールバック巻き戻し時の非シミュレーション状態（出力レーン）の不変量**:
  - 投機実行（Prediction）で進んだ状態から巻き戻す際、ECS/World 外の状態（描画補間用行列 `prevWorld`、オーディオ発音キュー `PushWaveShot`、トレイル点列 `TrailStore`）が過去の投機未来に汚染されないか。
- **チェックポイント確定とデシンク検出の整合性**:
  - パケットロスやバースト受信時に、スライディングウィンドウを用いずに単一の確定 tick/hash だけを共有することで生じるデシンク検査のスキップ・不一致リスク。
- **通信切断・DoS耐性**:
  - 正常終了メッセージ（`NetMsg::Bye`）の処理漏れによるタイムアウトハング、UDP バッファ超過（`WSAEMSGSIZE`）時の受信ループ中断。

### 観点10: アセットローダー・フォーマット解析・悪意/破損データ耐性 (Asset Loaders, Parsers & Resilience against Malformed Files)
- **フォーマットアクセサーの境界検査と GPU 送信前のバリデーション**:
  - glTF/GLB のスキニングアクセサー（`jointAcc`, `weightAcc`）やインデックスバッファの頂点数超過（OOB）検証。
  - 埋め込みテクスチャの `buffer_view` オフセット/サイズ境界検証。
- **JSON・バイナリデシリアライズ時の例外安全性とアロケーション上限**:
  - マテリアル JSON 解析時の未処理型例外（`nlohmann::json::type_error`）によるプロセス即死の防止。
  - クックバイナリのカウント偽装による巨大アロケーション（`std::bad_alloc`）対策。
- **決定論的データ表現の不変量**:
  - `String64` / `String256` の部分コピーによる残余バイトの未初期化ゴミメモリ残留と、WorldHasher によるハッシュ値の実行時ブレ。

### 観点11: Core / ECS アーキテクチャ・構造変更・データレイアウト (ECS Architecture & Storage Invariants)
- **スナップショット復元（SnapshotRead）の厳密な健全性検証**:
  - レコードの `row` 番号がアーキタイプ行数未満であるか、`freeIndices` がレコード範囲内かつ生存エンティティと重複していないかの完全検証。
- **階層構造（Hierarchy）の再帰・参照整合性**:
  - `CollectSubtree` の循環参照によるスタックオーバーフロー防止、DAG 多重親による二重破棄/NULL デリファレンス防止。
  - 親死亡時のアンリンクによるダングリング兄弟ポインタの防止。
- **型レジストリ（ComponentRegistry）の一意性と衝突耐性**:
  - FNV-1a 64bit ハッシュ一致のみに依存せず、型名文字列の完全一致比較による型取り違え（Type Confusion）防止。

### 12. レンダリングパイプライン・DirectX 11 API規約・リソースライフサイクル (D3D11 Pipeline & COM Invariants)
- **入力レイアウト（InputLayout）と頂点構造体のアライメント**:
  - `APPEND_ALIGNED_ELEMENT` によるセマンティック欠損時のオフセットずれと頂点データメモリ不正解釈の防止。
- **D3D11 リソース（SRV / DSV / RTV / CB）のクリーンアップ順序とスロット漏れ**:
  - スワップチェーンリサイズ時の深度テクスチャビュー解放漏れによるメモリリーク。
  - ForwardPass 終了時の PS シェーダーリソース解放数不足（スロット 9 の残留）による後続パスのハザード。
  - 定数バッファ生成時の 16 バイトアライメント違反と HRESULT 未検証。
- **パス間依存関係とタイミング**:
  - レイトレーシングパスとサーフェス速度書き込みの順序逆転によるテンポラル蓄積の破綻。

### 13. 物理力学・スクリプトABI・決定論的演算 (Physics, Script ABI & Deterministic Math)
- **コライダー非保有剛体への外力適用**:
  - `ApplyForceAtWorldPoint` においてコライダーを持たない剛体に対する回転モーメント計算時の基底欠損。
- **スクリプト境界のゼロ除算・NaN伝播**:
  - `AddForce` / `AddImpulse` における質量ゼロ・非正値時のゼロ除算耐性。

---

## 追補: 見落とし分析とフィードバック
本 round 3 において蓄積された「なぜ既存のテストや以前のレビューで見落とされたのか」の分析は、[oversights.md](file:///C:/HAL/MyEngin/docs/rev/oversights.md) に集約・追記した。
