# コードレビュー見落とし記録と分析 (Oversights & Root Cause Analysis)

- 対象ファイル: エンジン全域（Core, ECS, Net, Replay, Asset, Renderer, Physics, Scripting, Editor, Audio, Shaders）
- 最終更新: 2026-09-28
- 格納場所: `C:\HAL\MyEngin\docs\rev\oversights.md`

---

## 1. 見落としていた重大な不具合 (P0 - Round 1/2)

### [P0] `World::SnapshotRead` における入力検証欠如による任意のヒープ境界外アクセス・クラッシュ

#### 対象コードパスと行番号
- [`C:/HAL/MyEngin/src/Engine/Core/Ecs/World.cpp:670-689`](file:///C:/HAL/MyEngin/src/Engine/Core/Ecs/World.cpp#L670-L689)
- [`C:/HAL/MyEngin/src/Engine/Core/Ecs/World.cpp:716-722`](file:///C:/HAL/MyEngin/src/Engine/Core/Ecs/World.cpp#L716-L722)
- [`C:/HAL/MyEngin/src/Engine/Core/Ecs/Archetype.cpp:22-24`](file:///C:/HAL/MyEngin/src/Engine/Core/Ecs/Archetype.cpp#L22-L24)
- [`C:/HAL/MyEngin/src/Engine/Core/Ecs/ComponentRegistry.h:65`](file:///C:/HAL/MyEngin/src/Engine/Core/Ecs/ComponentRegistry.h#L65)

#### 脆弱性・不整合のメカニズム
1. **未検査の `ComponentTypeId` による `ComponentRegistry` 境界外読み出し**:
   - `World::SnapshotRead`（`World.cpp:673`）はスナップショットから読み取った `U32` をそのまま `ComponentTypeId` として `types[i]` に格納する。
   - `types` の要素が `ComponentRegistry::Get().Count()` 未満であるか検証せずに `std::make_unique<Archetype>(types, sig)`（`World.cpp:688`）へ渡す。
   - `Archetype` のコンストラクタ（`Archetype.cpp:23`）は `reg.Desc(t).size` を呼び出すが、`ComponentRegistry::Desc`（`ComponentRegistry.h:65`）は `descs_[id]` を境界検査なしで引くため、破損バイナリや未登録 TypeId が渡された場合に配列外アクセス（Out-of-bounds Read / アクセス違反クラッシュ）が発生する。

2. **カラムバイト長・要素サイズの相互整合性未検証**:
   - スナップショットに記録された `sizes[i]`（要素サイズ）と、現在の実行バイナリにおける `reg.Desc(types[i]).size` が一致しているかの検算がない。
   - カラム長 `columns[i].size()` が `rowCount * sizes[i]` と厳密に一致するかの検算がない。短いカラムデータを読み込んだ場合、その後の ECS コンポーネントアクセスでヒープ破損・境界外アクセスが発生する。

3. **`rec.row` の上限チェック欠落**:
   - `World.cpp:717` では `rec.archIndex >= archetypes.size()` の上限検査はあるが、`rec.row < archetypes[rec.archIndex]->Count()` の検査が完全に欠落している。
   - 破損スナップショットにより不正な行番号が設定された場合、`World::GetComponentRaw`（`World.cpp:159`）で `rec.archetype->GetPtr(ti, rec.row)` を呼んだ際にカラムバッファ外を指す不正ポインタが返され、読み書きによるメモリ破壊を招く。

4. **`freeIndices` の整合性未検証**:
   - フリーリストのインデックス値が `records_.size()` 未満であるか、あるいは生存エンティティ（`archIndex != kNoArchetype`）と重複していないかの検査がなく、デシリアライズ後に二重アロケーションや世代の食い違いを生む。

---

## 2. なぜ初回レビューで見落としたかの原因分析 (Root Cause Analysis - Round 1/2)

1. **スコープバイアス (直近差分への過度な集中)**:
   - 直近の大規模コミット群である M80（破壊・Destructionシステム）の実装差分に意識が過度に集中し、エンジン基盤層（`src/Engine/Core/Ecs/World.cpp` の既存スナップショット復元部）の入力バリデーション監査を怠った。
2. **観点の横展開（水平展開）の欠如**:
   - `FractureAsset::Deserialize`（`FractureAsset.cpp`）では、破損バイナリに対するヘッダー・バージョン・頂点インデックス境界検査を重点的に確認したにもかかわらず、「バイナリデシリアライズにおける完全防護」という同一観点を、エンジン全体の最も重要な基盤である `World::SnapshotRead` へ横展開して監査しなかった。
3. **「既存の完成機能」に対する無意識の前提**:
   - ロールバックネットコードやスナップショット機能は既にテストをパスしている既存機能であるという無意識のバイアスが働き、境界条件・破損データ耐性の検証を省略してしまった。

---

## 3. round 3 で特定された見落としパターンと原因分析 (Root Cause Analysis - Round 3)

### 見落としパターン A: 境界横断的な「出力レーン・非シミュレーション状態」の巻き戻し漏れ
- **発見された不具合**: ロールバック再シム時、描画補間行列（`prevWorld`）や音響発音キュー、トレイル履歴が巻き戻されず、投機未来の汚染データが残存する。
- **見落としの根本原因**:
  - レビュー時に「決定論的シミュレーション状態（`World`、`CpuParticleBackend`、`AcousticField` などハッシュ対象）」の整合性のみに集中し、「描画や音響へ出力を渡す境界（出力レーン）」がロールバック時にどのような副作用を受けるかという**境界相互作用の視点**が抜けていた。

### 見落としパターン B: 「サードパーティ製パーサー通過後」のデータ契約未検証
- **発見された不具合**: glTF スキニングアクセサーの要素数未検証、インデックス値の頂点数超過、マテリアル JSON の型不一致による未捕捉例外。
- **見落としの根本原因**:
  - `cgltf_parse` や `nlohmann::json::parse` が成功した時点で「データは構文的に妥当である」と誤認し、パーサーが出力したアクセサーの長さ不整合や要素の型不一致（配列要素が string や null）といった**セマンティック層のバリデーション**を監査から漏らしていた。

### 見落としパターン C: 姉妹実装間の「片肺修正・流用漏れ」
- **発見された不具合**: `ShaderManager.cpp` において、`BuildSurfaceInputLayout` ではセマンティック名に応じた固定オフセットが実装されていたにもかかわらず、一般シェーダー用の `BuildInputLayout` は `APPEND_ALIGNED_ELEMENT` のまま放置され頂点オフセットが破壊される。
- **見落としの根本原因**:
  - サーフェスマテリアルパイプラインの修正時に専用関数だけを改修し、同ファイル内の類似関数（姉妹実装）へ水平展開・同期修正を行わなかった。

### 見落としパターン D: 部分文字列コピー時の残余パディング未初期化
- **発見された不具合**: `String64` / `String256` のデシリアライズ時、部分コピー後の残余領域がゼロクリアされず、以前のスタック・ヒープゴミが残留してハッシュ決定性が破壊される。
- **見落としの根本原因**:
  - `dst[n] = '\0';` で C 文字列としての終端を保証したことで安全と錯覚し、「POD メモリブロック全体をビット単位でハッシュ化する」というエンジンの決定論仕様（`WorldHasher`）との衝突を意識できていなかった。

---

## 4. round 4 で特定された新規見落としパターンと原因分析 (Root Cause Analysis - Round 4)

### 見落としパターン E: エディタ操作（UI状態機械）とデータライフサイクルの非同期・割り込み
- **発見された不具合**:
  1. PlayMode 中にシーンを開く/新規作成するとスナップショットが残留し、Play 停止時に新シーンが旧シーンで上書き破壊される（[P0-1](file:///C:/HAL/MyEngin/src/Editor/App/EditorApp.cpp#L1893-L1896)）。
  2. ギズモドラッグ中にエンティティ破棄や選択変更が起きると Undo トランザクションが孤立し、異種エンティティ間のスナップショット合成（キメラ化）が発生する（[P0-2](file:///C:/HAL/MyEngin/src/Editor/Windows/Scene/SceneViewWindow.cpp#L1033-L1081)）。
  3. 部分更新（`ApplyPartial`）によるエンティティ再生成時、ペイロード外の第三者エンティティが持つ `EntityRef`（EntityID）が再解決されずダングリング化する（[P1-1](file:///C:/HAL/MyEngin/src/Engine/Engine/Scene/SceneSerializer.cpp#L720-L765)）。
- **見落としの根本原因**:
  - 過去のレビューは「ヘッドレス・直列実行」を前提とする C++ 単体テスト（SelfTest）やリプレイ検証（`replay_verify.bat`）に過度に依存していた。
  - 即時モード GUI（ImGui）における「フレームを跨ぐドラッグ」「フォーカス喪失」「ショートカットキー割り込み」「部分的な Undo/Redo 再生成」といった**ユーザーの非同期・不規則なUI操作とデータライフサイクルが交差するシナリオ**を検証スコープから完全に除外していた。

### 見落としパターン F: 外部非同期 API（XAudio2）の量子（Quantum）モデルとメモリ解放の競合
- **発見された不具合**:
  - クリップ再登録・ホットリロード時に `FlushSourceBuffers` 直後に PCM バッファを解放し、XAudio2 オーディオスレッドが解放済みメモリを読み取って UAF クラッシュを引き起こす（[P0-3](file:///C:/HAL/MyEngin/src/Engine/Engine/Audio/Playback/AudioSystem.cpp#L907-L910)）。
- **見落としの根本原因**:
  - API 名の「`FlushSourceBuffers`（フラッシュ完了）」という単語から、「バッファが完全に手放された」と直感的に誤認した。XAudio2 が「約10msの処理量子単位」で非同期に動いており、現在処理中のバッファはクォンタム終了まで解放されないという**低レベル非同期オーディオランタイムの契約**を意識していなかった。

### 見落としパターン G: IEEE 754 NaN と C++ 比較演算子の「全比較 false」によるガードすり抜け
- **発見された不具合**:
  - ModalSynth において縮退オブジェクトによるゼロ除算で NaN が混入した際、`if (underRoot <= 0.0f)` などの全ガード節が false となり、NaN がそのまま PCM 生成へ流れて `std::bad_alloc` や UB キャストを引き起こす（[P0-4](file:///C:/HAL/MyEngin/src/Engine/Engine/Audio/Synth/ModalSynth.cpp#L52-L174)）。
  - 自動露出ヒストグラム（HLSL）で NaN 輝度が入り、`groupshared uint sBins[256]` に対しアドレス `4294967295` への LDS 境界外書き込み・TDR を引き起こす（[P0-7](file:///C:/HAL/MyEngin/assets/shaders/postfx_hist.cs.hlsl#L18-L37)）。
- **見落としの根本原因**:
  - ガード節を書く際に「正常な実数」のみを無意識に仮定していた。IEEE 754 において「NaN に対する大小・等値比較はすべて false を返す」という言語仕様の罠を考慮せず、`!std::isfinite()` や `isfinite()` による明示的な有限性検証を行っていなかった。

### 見落としパターン H: 静的検証ツール通過による「意味論的検証」の油断
- **発見された不具合**:
  - `Interop.cs` と `MyeEngineApi` のスロット一致静的検査（`check_rules.ps1` 規則 11）が通過していることに満足し、スロット内部でのバッファ境界検証欠如（FailFast 即死）や NaN バリデーション欠如を見落とした（[P0-2](file:///C:/HAL/MyEngin/src/Scripting/ScriptRuntime.cs#L348-L392), [P1-3](file:///C:/HAL/MyEngin/src/Engine/Engine/Script/EngineApiTable.cpp#L127-L281)）。
  - スクリプト削除時の `managedIndex` ゾンビ化（[P0-1](file:///C:/HAL/MyEngin/src/Engine/Engine/Script/ManagedHost.cpp#L191-L251)）。
- **見落としの根本原因**:
  - CI の機械的チェックが「エラー 0」を返していることで心理的安全性を得てしまい、「関数の境界で渡されるポインタ・データの中身が本当に安全か」という**セマンティック層の防御的プログラミング監査**を省略してしまっていた。

---

## 5. round 5 で特定された新規見落としパターンと原因分析 (Root Cause Analysis - Round 5)

### 見落としパターン I: OS / プラットフォーム固有仕様（POSIX互換錯覚・高DPI・RawInput）の過小評価
- **発見された不具合**:
  1. Windows における `std::filesystem::rename` が既存ファイルを上書きできずエラーになる仕様（[P1-1](file:///C:/HAL/MyEngin/src/Engine/Platform/PathUtil.cpp#L133-L138)）。
  2. Per-Monitor V2 DPI 環境下で `AdjustWindowRect` を誤用し、クライアント矩形とバックバッファが不整合を起こす（[P1-5](file:///C:/HAL/MyEngin/src/Engine/Platform/Win32Window.cpp#L52-L55)）。
  3. RDP やタブレットの RawInput（`MOUSE_MOVE_ABSOLUTE`）の 0..65535 正規化座標をピクセル差分と誤認してカメラが 34 倍暴走・カーソルロック振動（[P1-3](file:///C:/HAL/MyEngin/src/Engine/Platform/Input.cpp#L56-L70)）。
- **見落としの根本原因**:
  - レビューの焦点が ECS、レンダラー、物理などの高度なアルゴリズム層に偏り、OS ラッパー層を「自明な薄いコード」と過小評価していた。
  - C++ 標準ライブラリ（`std::filesystem`）の関数名から POSIX 的なアトミック置換を期待してしまい、Windows プラットフォーム特有の実装差分（`MoveFileExW` の挙動）を検証していなかった。

### 見落としパターン J: クラッシュハンドラ内での「ローダロック非同期安全」軽視
- **発見された不具合**:
  1. スタック枯渇回避のためにハンドラ内で `CreateThread` を呼び、ローダロック取得デッドロックで 20 秒間完全ハングする（[P0-1](file:///C:/HAL/MyEngin/src/Engine/Platform/CrashHandler.cpp#L319)）。
  2. マルチスレッドクラッシュ時に後着スレッドが即座に `TerminateProcess` を呼び、先着スレッドのダンプ出力を切断・破壊する（[P0-2](file:///C:/HAL/MyEngin/src/Engine/Platform/CrashHandler.cpp#L460)）。
- **見落としの根本原因**:
  - クラッシュハンドラは正常系テストでは動作せず、単一障害のテストケースしか通していなかった。
  - プロセスが致命的障害に陥っている状況で Windows の `CreateThread` が全 DLL の `DllMain(DLL_THREAD_ATTACH)` を同期実行し、ローダロック（`LdrpLoaderLock`）を奪合するという**OS 低レベルの非同期シグナル安全性（Async-Signal-Safety）**の制約を意識していなかった。

### 見落としパターン K: 物理幾何における「教科書的アルゴリズムの境界値」と「直感の罠」
- **発見された不具合**:
  1. Sutherland-Hodgman 多角形クリッピングの境界値（$d=0$）で同一頂点が二重登録され、接触マニフォールドが重複点で浪費される（[P1-4](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/Collider/ConvexCollision.cpp#L367-L379)）。
  2. 接触マニフォールド削減を「深度ソート降順」のみで行った結果、わずかな傾きで支持面が 1 次元の線分に縮退し、箱が直交軸まわりに転倒・不安定化する（[P2-1](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/Collider/ConvexCollision.cpp#L412-L424)）。
  3. 三角形重心とボックス中心の内積で接触法線の向きを決めていたため、ボックスが床をわずかに超えて貫通した瞬間に法線が 180 度反転し、床の内側へ吸い込まれる（[P1-1](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/Rigid/Shapes.cpp#L1435-L1444)）。
- **見落としの根本原因**:
  - 教科書通りのアルゴリズム（クリッピング、深度優先選択）を盲信し、「接触力学では最大面積の支持多角形（Support Polygon）の維持が最優先である」という物理エンジンの本質的要求と照合していなかった。
  - 正常系の浅い貫通テストケースばかりで検証し、深い貫通・境界平面上という動的極限条件での挙動トレースを怠っていた。

### 見落としパターン L: 「安全側の包含（外接球）」が引き起こす逆転の早期ガード・すり抜け
- **発見された不具合**:
  - CCD（連続衝突判定）において、安全側として外接球半径 $R$ を採用した結果、細長い剛体が障害物の手前 0.5m にある時点で「既に接触している」と誤判定され、CCD が除外ガードでスキップされて壁をすり抜ける（[P1-3](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/Rigid/PhysicsSystem.cpp#L1171-L1172)）。
- **見落としの根本原因**:
  - 「外接球で包含すれば衝突を取りこぼさないから安全側である」という直感に頼り、「包含が大きすぎると、移動前接触の除外チェックを誤発火させて処理そのものをバイパスさせる」という**ガード条件との相互作用の逆転現象**を見落としていた。

### 見落としパターン M: 手書き SoA / 特殊パイプラインにおける「汎用防護網の孤立」
- **発見された不具合**:
  - CPU パーティクルの SoA 実装で `SimSnapshot` に手書きシリアライズを行い、構造体の未初期化パディングゴミ（デシンク原因）や生存数 `alive` を超えた死骸配列をまるごと保存して肥大化させた（[P1-3](file:///C:/HAL/MyEngin/src/Engine/Engine/Replay/SimSnapshot.cpp#L195), [P1-4](file:///C:/HAL/MyEngin/src/Engine/Engine/Replay/SimSnapshot.cpp#L196-L208)）。
  - `TrailStore` を「描画専用（`kComponentNoHash`）」と分類したことで Replay 監査から除外し、巻き戻し時の `uint64_t` アンダーフローによる全点消滅・巨大ポリゴンアーティファクトを見落とした（[P0-2](file:///C:/HAL/MyEngin/src/Engine/Engine/Vfx/VfxRenderer.cpp#L97-L100)）。
- **見落としの根本原因**:
  - ECS の自動リフレクションやハッシュ検証システムがカバーしている領域の外側に、性能理由で手書きされた特殊モジュールが存在する場合、それらのデータ整合性・決定性検証がシステム全体から孤立し、レビューの死角になっていた。

### 見落としパターン N: 権威あるコード（公式バグ修正等）移植時の前提条件未検証
- **発見された不具合**:
  - Unity 公式バグ修正（case 1345471）を移植した `UILayoutGroup::ArrangeGrid` において、`cellsPerMainAxis == 1`（1列グリッド）のときに `cellsPerMainAxis - 1.0f` でゼロ除算が発生し全子要素の座標が負の彼方へ吹き飛ぶ（[P1-3](file:///C:/HAL/MyEngin/src/Engine/Engine/UI/UILayoutGroup.cpp#L446-L455)）。
- **見落としの根本原因**:
  - 「Unity の公式コードだから正しい」という権威バイアス（先入観）が働き、1列や1行という最も基本的な境界値（$n=1$）に対する分母ゼロチェックの検証を省略してしまった。

---

---

## 6. round 6 で特定された新規見落としパターンと原因分析 (Root Cause Analysis - Round 6)

### 見落としパターン O: テンポラルフィルタ（TAA）における「NaN 感染爆発（Temporal Explosions）」の盲点
- **発見された不具合**:
  - `postfx_taa.hlsl` において、3x3 近傍の色の箱（`nmin`, `nmax`）の計算に NaN ガードがなく、入力 HDR シーンに 1 画素でも NaN が混入した場合、`clamp` と `lerp` を経て次フレームの `gTaaHist` に書き込まれ、再投影サンプリングによって画面全体へ無限に拡散・黒画面/白画面化する（[P0-1](file:///C:/HAL/MyEngin/assets/shaders/postfx_taa.hlsl#L57-L78)）。
- **見落としの根本原因**:
  - TAA のカラークランピング（Neighborhood Clamping）が「ゴースト（前フレームの残像）を抑えるための安全機構」として機能していることに満足し、「IEEE 754 の min/max 比較において NaN が入ると比較結果が破壊され、クランプ自身が NaN 拡散の媒介になる」というテンポラルアルゴリズム特有の感染リスクを検証していなかった。

### 見落としパターン P: 幾何射影・シャドウスプリットにおける「対数空間（Log Space）ゼロ割れ」
- **発見された不具合**:
  - `FrustumCull.h` の `ComputeCascadeSplits` において、`nearZ <= 0` のときに `std::pow(farZ / nearZ, p)` がゼロ除算・負数累乗により NaN を返し、`cascadeSplits` が全滅してディレクショナルシャドウが全消滅する（[P0-2](file:///C:/HAL/MyEngin/src/Engine/Renderer/Pipeline/FrustumCull.h#L14-L22)）。
- **見落としの根本原因**:
  - 正常な透視投影（$nearZ = 0.1, farZ = 1000$）の範囲内でのみユニットテストが書かれており、カメラの nearClip が 0 や負値に設定された極限条件（未設定・エディタでの不正入力）での対数補間式（Practical Split）の特異点チェックが抜けていた。

### 見落としパターン Q: ポストプロセスリソース管理における「ビュー時間状態と解像度キーの結合」
- **発見された不具合**:
  - `PostProcess::Acquire` において、解像度キー（`w,h`）を元にレンダーターゲットを管理しているため、エディタウィンドウをドラッグして連続リサイズした際、毎フレーム大量の VRAM 破棄・再生成ストールが発生するだけでなく、自動露出バッファ（`exposureBuf`）の過去履歴が消失して激しい明滅（露出フリッカー）を引き起こす（[P1-2](file:///C:/HAL/MyEngin/src/Engine/Renderer/PostFx/PostProcess.cpp#L240-L274)）。
- **見落としの根本原因**:
  - 「ターゲットテクスチャのサイズ適合」という描画リソースの都合と、「自動露出の明暗適応」というビュー（カメラ）の時間的シミュレーション状態を同一のキャッシュ構造体（`Target`）に混在させ、所有権とライフサイクルの分離を怠っていた。

---

## 7. 必要な修正要件まとめ（Round 1〜6 累計）

1. **ECS / Core / シリアライズ**:
   - `World::SnapshotRead` の `row`, `freeIndices`, `sizes`, `types` 厳密検証
   - `CollectSubtree` の循環ガードおよび多重破棄ガード
   - `JsonUtil` の `String64` / `String256` デシリアライズ前の全域 `memset(0)`
   - `SceneSerializer::ApplyPartial` 完了時の全体 `EntityRef` 再解決パス
   - `SceneSerializer` の旧 `UIElement` 移行条件を `anchor` 以外の座標キーにも緩和

2. **プラットフォーム・OS**:
   - `PathUtil::WriteFileReplacing` を `ReplaceFileW` / `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` に修正
   - テンポラリファイル名にスレッドID・アトミック通し番号を追加
   - `CrashHandler` 内の `CreateThread` を即時廃止（事前作成スレッドまたは別プロセス方式へ改修）、多重クラッシュ時の `Sleep(INFINITE)` 待機
   - `Input` の `MOUSE_MOVE_ABSOLUTE` を仮想スクリーン解像度でピクセル変換し、カーソルロック時の振動相殺を導入
   - `Win32Window` の `AdjustWindowRect` を `AdjustWindowRectExForDpi` に置換、`WM_DPICHANGED` ハンドリング追加
   - `Win32Window::HandleMsg` のハンドラ走査を再入安全化

3. **物理・XPBD・幾何**:
   - `ConvexCollision` の `.mcvx` デシリアライズで `verts.size() <= kConvexMaxVerts` (64) を厳格検証
   - `PhysicsSystem` / `XpbdSolver` の粒子プール空時の `size_t` アンダーフロー即死ガード
   - `xpbd::Predict` / `Solve` の $h \le 1e-9$ ゼロ除算ガード
   - `BoxTriSat` の接触法線決定を三角形外向き面法線基準に改修（重心判定の廃止）
   - `PhysicsSystem::SolveCharacters` に複合コライダー（Compound Collider）子形状走査を追加
   - CCD の接触判定を形状固有距離関数による保守的前進へ改修
   - Sutherland-Hodgman クリッピングの境界値重複排除
   - 接触マニフォールド削減を「最深点＋最大面積多角形」による 2D 支持面維持方式へ改善

4. **パーティクル・VFX**:
   - `ParticleFlipTilePos` での `tiles == 0` ガード（`max(1u, tiles)`）
   - `TrailStore` での時間巻き戻し（`tick < pts.tick`）検出とバッファクリア
   - `SimSnapshot` の `ParticleEmitterComponent` 生ダンプを廃止し、明示的フィールド保存へ改修
   - `SimSnapshot` の SoA 保存を `alive` 個分の有効データのみに限定
   - `VfxRenderer` の `nowTick` を全エミッタ最新点ではなく現在のワールド `simTick` から取得
   - `CpuParticleBackend::Reset` での `instanceBuffer_` 解放

5. **UI・ウィジェット・フォント**:
   - `UILayout::ResolveWorldBase` での $cw \le 1e-4$ ゼロ除算・NaN 伝播ガード
   - `UIWidgets::Clamp01` での NaN 透過防止（`!(t >= 0.0f)` 検査）
   - `UILayoutGroup::ArrangeGrid` での `cellsPerMainAxis > 1` ガード
   - `UILayout::BuildSimWorldContext` での $fovY \le 0$ / $farZ == nearZ$ ゼロ除算ガード
   - `UIInteraction::FindNextFocus` の候補配列ソートによる決定性保証
   - `FontAtlas::Grow` でのパッキング成否検査とフォールバック登録
   - `EngineApiTable::SetUIRect` の親基準化防止（`hasUiAncestor` 判定）

6. **レンダラー・シェーダー・ポストプロセス**:
   - `postfx_taa.hlsl` での入力 `curSample` に対する `isfinite` 検証と NaN クランプ
   - `FrustumCull.h` の `ComputeCascadeSplits` での `nearZ <= 1e-3f` ガード
   - `postfx_hist_reduce.cs.hlsl` での `gAeSpeed` 下限クランプ（正値保証）
   - `PostProcess` の自動露出バッファを解像度キャッシュから `viewKey` 単位の永続バッファへ分離
   - `hzb_reduce.cs.hlsl` での寸法 0 時の `uint` アンダーフローガード
   - `FroxelPass` の `gridNearZ_` と `view.projNoJitter` クリップ面整合性担保
   - `postfx_hist.cs.hlsl` での NaN ガードおよび `min(bin, 255u)` クランプ
   - `rt_common.hlsli` での微小交差排除用 `tMin` ガード
   - `ShaderManager.cpp` / `ComputeAbiRunner.cpp` のリフレクション変数キーの `cbufferName.varName` 一意化
   - `ComputeAbiRunner` の毎フレーム定数バッファ生成の永続プール化
   - `forward_lit.hlsl` の `PerObject` cbuffer を 96B に同期

7. **エディタ・Undo・SCM**:
   - `EditorApp` の PlayMode 中シーン切り替え時の PlayMode 停止・スナップショット破棄ガード
   - `SceneViewWindow` のギズモドラッグ中断時の `undo.CancelRecord()` 呼出し
   - `GitTransaction` の Running モーダルへの強制キャンセル・タイムアウト実装

8. **オーディオ・音響**:
   - `AudioSystem::RegisterClip` での XAudio2 クォンタム完了待機（遅延解放）
   - `ModalSynth` および `ModalAudio` での `std::isfinite` 検証と縮退クランプ
   - `AcousticField::Rebuild` での残光ボリューム（`glow_`）書き込みの無効化（`shellSlot = -1`）

9. **スクリプト・ホットリロード**:
   - `ManagedHost::RegisterTypes` での旧スクリプト型の `managedIndex = -1` リセット
   - `ScriptRuntime.cs` の `ReadValue` におけるバッファ長境界ガード
   - `ReloadHub` の監視拡張子への `.cs` 追加
   - `EngineApiTable.cpp` の C ABI 受信境界での `std::isfinite` ガード


