# M78 プロジェクト側ポスト／コンピュートシェーダ — 仕様書

- slug: m78-project-shaders
- 状態: 確定 (2026-09-22)
- 依頼原文: プロジェクト側でシェーダーを追加できる機能を追加したい

## 1. 目的 (なぜ作るか)

作者がエンジン再ビルドなしに、プロジェクト資産として **画面全体の見た目 (ポスト)** と **GPU シミュレーション／加工 (コンピュート)** を足し、「ばえる絵」を作れる状態にする。

達成したい状態:
- プロジェクトの `assets/shaders` に生 HLSL を置き、先頭の Properties 宣言だけで Inspector にパラメータが生える
- カメラに Effect Stack を割り当てると、既存トーンマップ前後の決まった挿入点でパスが走る
- GameLogic / C# から **不透明ハンドル＋名前バインド**でコンピュートバッファ確保・Dispatch ができる (ABI v21)
- コンパイル失敗時はマゼンタ相当／エラー表示で気づける (描画スキップで静かに消えない)
- 既存組込みポストをオフにした既定シーンの見た目・決定論は変えない (スタック空・ABI 未使用時)

## 2. 疑った点と結論

| 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|
| 「3系統すべて」は本当に今回やるか | 台帳改定: ポスト＋コンピュートのみ。design-draft §1 は旧合意 | 改定を正とする (司会補足) | **サーフェス／.mat.json shader 差し替えはスコープ外**。次マイル仮置きのみ §3 後回しへ |
| シェーダルート新設が要るか | `ShaderManager.h:36` / `EngineLoop.cpp:251-267` で `[project]\assets\shaders` 優先は実装済み | — | **ルート追加はしない**。不足は「新規名の Load・バインド・Inspector・挿入」 |
| PostProcess に差し込み口があるか | `PostProcess.cpp:674-` Resolve は固定チェーン。カスタム配列なし | — | **挿入点＋Priority のユーザーパス配列を新設** (UE Blendable に近い二軸) |
| CameraPostFx に可変リストを載せられるか | `Reflection.h` の FieldType にリスト無し。`CameraPostFxComponent` は固定フィールド末尾 append のみ (`Components.h:769`) | 3A 確定 | **`*.fxstack.json` 新アセット**をカメラが AssetRef。CameraPostFx を肥大化しない |
| コンピュートに ABI v21 が必須か | `EngineAPI.h` MYE_API_VERSION=20。Compute/Dispatch スロット無し。既存 CS は Pass 直書き (`FroxelPass.cpp` 等)。Interop は位置ミラー＋実行時版検証なし (`check_rules` 規則 11) | **1B: 今マイルで ABI も足す** (planner 初回裁定「ABI 後回し」を押し切り) | **シーン駆動 Dispatch (sub-04) ＋ ABI v21 (sub-05) の両方**。反対意見は下表直後に残す |
| 挿入点は 2 点で足りるか | Resolve は TAA…Godray→Tonemap→FXAA 固定 | **2A: BeforeTonemap + AfterTonemap のみ** | 仕様 4.1 どおり。追加挿入点は後回し |
| Properties を `.hlsl.meta` に置くか本体コメントか | 合意は「生 HLSL + 属性／コメント」。`.meta` は guid/type/version のみ | 作者形式は案 A 確定 | **HLSL 先頭の `/*@MyEngineProperties ... @*/` ブロック**を真値とする。`.meta` は触らない |
| 失敗時はマゼンタかスキップか | Forward は `!valid` で draw skip (`ForwardPath.cpp:443`)。マゼンタ無し | 案 A: エラー／マゼンタ相当 | **ポスト失敗 = マゼンタ全画面 (そのパス)**。コンピュート失敗 = Dispatch スキップ＋Editor エラー表示。エンジンは落とさない |
| 名前バインド vs register 直書き | Unity/UE とも作者にレジスタを握らせない (`reference-unity-ue.md` Q3)。DX11 では Reflection で名前→スロット可 | — | **作者は名前のみ。エンジンが Reflection／生成 CB でバインド**。エンジン予約名はプレフィックス／共通 include で分離 |
| バリアント (multi_compile) が要るか | 決定論・golden と衝突しやすい (`reference` §5-8) | — | **やらない**。別ファイルで足りる |
| dirty ツリーを掃除すべきか | 未追跡雑多あり。台帳: 明示ステージのみ | ユーザー選択 1 確定 | **無関係未追跡は触らない**。コミットはファイル名明示 |
| 受け入れ条件で確定してよいか | — | **4A: 1B 反映後の条件で実装開始してよい** | §5 を ABI 条件込みで確定 |
| SetComputeTextureFromAsset は任意か | planner 裁定「Dispatch 優先で 6 本まで削可」 | **B: v21 最小に必須** (裁定を覆す) | §4.5 で必須 7 本下限。sub-05 から削る余地を削除 |

**押し切られた点 (蒸し返さない) — コンピュート ABI:**
- planner 反対意見: 既存コンピュートは Pass 直書きのみで足り、「ばえる絵」は fxstack 駆動で縦切り可能。ABI は DLL 境界・寿命・決定論・C# レーンが replay 被覆外 (`abi-bump-verification.md`) のため手戻りコストが大きい。初回裁定は「シーン駆動のみ・ABI 後回し」だった。
- ユーザー判断 1B: 今マイルで GameLogic/C# 向け Compute ABI (v21) も足す。
- 結論: 反対意見を記録したうえで **採用**。シーン駆動を先に通し (sub-04)、その上に不透明ハンドル ABI を載せる (sub-05)。

却下した案 (蒸し返さない):
- サーフェス／GBuffer 対応を本マイルに含める
- URP 風の多数 RenderPassEvent
- CameraPostFx 固定スロット N 本詰め
- ABI を本マイルから外す (初回裁定 — **ユーザー 1B で却下**)

## 3. スコープ

### やる
- プロジェクト HLSL の Properties コメント DSL パース → スキーマ (型・既定・属性)
- フルスクリーン **プロジェクトポストパス** (挿入点: BeforeTonemap / AfterTonemap、同一点内は Priority)
- **プロジェクトコンピュートパス** (fxstack から毎フレーム／指定タイミングで Dispatch。UAV／一時テクスチャはエンジンが寿命管理)
- Properties → Inspector 自動 UI (Float/Range/Color/Vector/2D テクスチャ＋主要属性)
- `*.fxstack.json` アセットとカメラからの参照
- コンパイル失敗の可視化 (マゼンタ／エラーパネル)
- 共通 include (SceneColor / Depth 等の名前付き入力)
- **Compute ABI v21** (GameLogic / C#: バッファ確保・解放・名前バインド・Dispatch。生 D3D 非露出)
- SelfTest＋既存チェック (`check_rules` 規則 11 含む / 既定シーンで組込みのみのときビット同一方針)

### やらない (明示)
- サーフェス／マテリアル用シェーダ差し替え、`.mat.json` の `shader` 編集 UI、Deferred GBuffer ユーザー規約
- パーティクル／VFX シェーダ差し替え
- シェーダキーワード／バリアント爆発
- エンジン組込みポスト (bloom/DoF/…) の置き換え UI (無効化は既存 CameraPostFx のまま)
- GPU→CPU 読み戻し (Map/Readback) で sim／WorldHash に書く経路
- DispatchIndirect / Append／Counter バッファの一般化

### 後回し
- サーフェス系統 (次マイル候補。Material 遅延 Load・マゼンタ・Properties・GBuffer)
- BeforeTranslucency / AfterFxaa / TAA 前などの追加挿入点
- Volume ブレンド (空間的な強さ補間)
- CustomEditor 相当の完全カスタム Inspector
- Compute Readback / IndirectDispatch / テクスチャ UAV の高度な共有プール API

## 4. 仕様

### 4.1 振る舞い

#### 作者ワークフロー (ポスト)
1. `<project>/assets/shaders/<Name>.post.hlsl` を作成 (命名: `*.post.hlsl` = ポスト、`*.cs.hlsl` は既存どおりコンピュート。プロジェクト新規コンピュートも `*.cs.hlsl`＋Properties ブロック)
2. 先頭に Properties ブロック、本体は `PSMain` (ポスト) / `CSMain` (コンピュート)。VS はエンジン供給のフルスクリーン三角形／クアッドを使う (作者は PS のみ書いてよい。必要なら VS も同ファイル可だが既定は共通 VS)
3. `*.fxstack.json` にパスを追加し、カメラ (またはメインカメラ想定エンティティ) に Stack の AssetRef を付ける
4. Play／SceneView で挿入点どおり実行。Inspector で Properties をいじるとライブ反映 (ホットリロード既存経路を利用)

#### 挿入と順序
既存 Resolve 順は維持:
`TAA → DoF → MB → AE → Bloom → Godray → **[BeforeTonemap ユーザー]** → Tonemap(+FXAA 配管) → **[AfterTonemap ユーザー]** → dst`

- BeforeTonemap: HDR シーンカラーを入力、HDR に書く (トーンマップ前)
- AfterTonemap: LDR (FXAA 使用時はその出力後、未使用時は tonemap 直出し) を入力、LDR に書く
- 同一 Insertion 内は `priority` 昇順 (安定ソート。同値はスタック配列順)
- スタック空／全パス無効 = **現行と同一経路** (余計なコピー無し)

#### コンピュートの起動 (シーン駆動)
- fxstack エントリ `kind: "compute"` が、指定 `dispatchPoint` (`BeforePost` / `BeforeTonemap` / `AfterTonemap`) で走る
- グループ数: Properties またはメタの `numthreads` と画面／バッファサイズからエンジンが算出 (作者は `[numthreads(x,y,z)]` を HLSL に書く。既存 `LoadCompute` エントリ `CSMain` 踏襲)
- 出力: エンジンが用意する一時 UAV テクスチャ／StructuredBuffer を名前でバインド。結果を後続ポストが SRV として参照できる
- **ワールド／ECS／ハッシュには書き込まない** (描画専用。`CameraPostFx` と同様 NoHash レーン)

#### コンピュートの起動 (ABI / GameLogic・C#)
- スクリプトは **不透明バッファ ID** と **シェーダ名 (UTF-8)／プロパティ名** だけで操作する。`ID3D11*` は Shared に出さない
- 典型フロー: `CreateComputeBuffer` → `SetComputeBuffer` / `SetComputeFloat*` → `DispatchCompute` → (不要なら) `ReleaseComputeBuffer`
- シェーダ未ロード時はエンジンが `LoadCompute` 相当を行い、失敗時は Dispatch が 0 を返してスキップ (クラッシュしない)
- **GPU 結果を ECS／WorldHash 対象へ読み戻して分岐する用法は禁止** (本マイル)。視覚効果・スクリプト内の一時利用に留める。C++ GameLogic が誤って hash 対象に書けばリプレイが壊れる — 糖衣コメントと docs で明示
- C# レーンは record/verify 中に走らない (`EngineAPI.h` Get/SetComponentField 注記と同系)。ABI 自体の正しさは規則 11 ＋ temp プローブ実走で担保 (§4.5)

#### 失敗時
| 対象 | 挙動 |
|---|---|
| ポストコンパイル失敗・`valid==false` | そのパスをマゼンタ (1,0,1) 全画面で置換。Console/Editor にエラー行 |
| コンピュート失敗 (スタック／ABI) | その Dispatch をスキップ／ABI は 0 戻り。Editor にエラー。後続は入力未更新のまま |
| ホットリロード失敗 | 既存 `ShaderManager` どおり旧プログラム維持＋WARN。成功するまで旧絵 |
| スタック参照切れ | そのエントリをスキップ＋警告。他エントリは継続 |
| 旧 GameLogic.dll (apiVersion≠21) | 既存どおりロード拒否 (`ScriptHost.cpp` 厳密一致)。再ビルド必須 |

#### エッジケース
- Properties にあるが HLSL に同名変数が無い → 警告、値は無視
- HLSL にあって Properties に無いユーザー定数 → バインド対象外 (エンジン予約・共通 include は除く)
- テクスチャ未割当 → Unity 同様 `"white"/"black"/"gray"/"bump"` 既定をエンジン組込みから供給
- 解像度変更 → 一時 RT／UAV をリサイズ。露出履歴等の組込み状態は既存どおり
- ABI: 無効バッファ ID・未知シェーダ名・解放済み ID → 失敗戻り値、落ちない
- ABI: 二重 Release／未 Release 放置 → エンジンがシャットダウン時に回収＋WARN (リークを黙殺しない)

### 4.2 データ・保存形式・互換性

#### Properties DSL (HLSL 先頭コメント)
```hlsl
/*@MyEngineProperties
[Range(0.0, 1.0)] _Intensity ("Intensity", Float) = 0.5
_Tint ("Tint", Color) = (1, 1, 1, 1)
_Mask ("Mask", 2D) = "white" {}
[Header(Advanced)]
_Direction ("Direction", Vector) = (0, 1, 0, 0)
@*/
```
対応型 (初版): `Float` / `Range(a,b)` / `Color` / `Vector` / `2D`。  
属性初版: `[Range]` (型としても可) / `[Header(name)]` / `[HDR]` / `[HideInInspector]` / `[NoScaleOffset]`。  
パース失敗 = シェーダ全体を無効扱い (コンパイル前にエラー表示)。  
**[Header] のスキーマ表現 (sub-01 で確定):** 独立行の Header は `PropertySchema` として `name.empty()`・`attr.header` にラベル・`cbOffset == -1`。Inspector (sub-03) はこの条件で区切りを描く。

#### 定数・テクスチャバインド
- ユーザースカラー／ベクトルは **シェーダ固有 cbuffer** (名前例 `MyEnginePerEffect`) にパック。**レイアウトはパース順** (HLSL 風: float は 4B 詰め、Color/Vector は 16B 境界。全体サイズは 16B 倍数)。Reflection オフセット方式は本マイルでは採用しない
- テクスチャは名前バインド。エンジン予約 (SceneColor / SceneDepth / 等) は `ProjectPostCommon.hlsli` で提供し、Properties のユーザー 2D と名前空間を分ける
- 作者に `register(tN)` / `register(bN)` を要求しない (書いてもエンジン規約外として警告してよい)
- `cbSizeBytes == 0` (プロパティ無し・スカラー無し) のとき Pack は空バッファ成功。Runner は CB バインドを省略してよい

#### `*.fxstack.json` (概略)
```json
{
  "version": 1,
  "passes": [
    {
      "kind": "post",
      "shader": "MyTint.post",
      "enabled": true,
      "insertion": "BeforeTonemap",
      "priority": 100,
      "properties": { "_Intensity": 0.7, "_Tint": [1, 0.9, 0.8, 1] }
    },
    {
      "kind": "compute",
      "shader": "MySim.cs",
      "enabled": true,
      "dispatchPoint": "BeforePost",
      "priority": 50,
      "properties": { "_Steps": 1 }
    }
  ]
}
```
- カメラ側: 新フィールドまたは薄コンポーネントで `AssetID fxStack` (FieldType::AssetRef)。既存シーンは null = 挙動不変
- シェーダ名は `ShaderManager::Load` / `LoadCompute` の既存命名に合わせる (`MyTint.post` → `MyTint.post.hlsl` 等。実装時に Load 側の拡張子規約を sub で確定し SelfTest で固定)

#### 互換
- エンジン組込みシェーダ／CameraPostFx の既存フィールド意味を変えない
- **ABI: `MYE_API_VERSION` 20 → 21**。既存スロットの並び・シグネチャは不変 (末尾 append のみ)
- cooked `Material` POD サイズを変えない (本マイルは Material 非接触)

### 4.3 UI / ビジュアル
- Inspector: fxstack 参照＋パス一覧 (有効／insertion／priority)＋選択パスの Properties 自動ウィジェット
- コンパイルエラー: 既存ログに加え、Editor 上で該当パス／シェーダ名が分かる表示 (最低限 Console。可能なら Inspector バナー)
- マゼンタは「失敗したポストパスの出力」が画面を塗りつぶすことで気づけること (スクショ検証可)
- ローカライズ: 新規 UI 文字列は既存 `MYE_JP` / ローカライズ規約に従う

### 4.4 非機能
- **決定論**: カスタムポスト／スタック駆動コンピュートはワールドハッシュ非関与。ABI コンピュートも **hash に載せない**。`--replay-verify` 対象シーンでスタック未使用かつ ABI 未使用なら従来一致
- **golden**: エンジン既定ゴールデンはスタック空想定。組込み Resolve 順・シェーダを不用意に変えない
- **層**: 生 D3D 型は Renderer 内。Shared には POD／不透明 ID／関数ポインタのみ (AGENTS.md DLL 境界)
- **性能**: パス数に比例するフルスクリーン。初版のハード上限 (例: ポスト 8＋スタックコンピュート 8)。ABI バッファ数にも上限 (実装で定数化、SelfTest)
- **ホットリロード**: 既存依存グラフ＋キャッシュを流用。Properties 変更時はスキーマ再パース＋ UI 更新

### 4.5 Compute ABI v21 (契約)

#### スロット方針 (末尾 append)
現行 v20 = **118 スロット** (`check_rules.ps1` `$apiVersionSlots[20]=118`)。v21 は末尾にコンピュート系を追加し、`$apiVersionSlots` に `21 = <新件数>` を同時登録する。

最小セット (名前は実装で最終確定してよいが役割は固定。**下限 7 本・Texture 必須**):

| 役割 | 契約の要点 |
|---|---|
| CreateComputeBuffer | `(count, stride, usageFlags) → bufferId`。エンジンヒープ。失敗は 0 |
| ReleaseComputeBuffer | 無効 ID は no-op／0。二重解放で落ちない |
| SetComputeBuffer | `(shaderUtf8, bufferNameUtf8, bufferId) → 成否`。名前バインド |
| SetComputeFloat / SetComputeFloat4 | Properties／cbuffer 名前へ値。未知名は 0 |
| SetComputeTextureFromAsset | `(shaderUtf8, textureNameUtf8, assetId または組み込み名キー) → 成否`。SRV 名前バインド。**v21 最小セットの必須スロット** (削不可) |
| DispatchCompute | `(shaderUtf8, gx, gy, gz) → 成否`。内部で LoadCompute。失敗スキップ |

- 上記は **必須**。Dispatch 縦切りのために Texture スロットを落とすことは認めない (ユーザー裁定 B。planner の「6 本まで削ってよい」は覆された)
- スロット総数は Create / Release / SetBuffer / SetFloat / SetFloat4 / SetComputeTextureFromAsset / Dispatch で **7 本**を下限とする (名前の最終綴りは実装で固定してよいが役割は欠けてはならない)
- ハンドル型: `uint64_t` (0 = 無効)。世代付きにするかは実装判断 (SelfTest で use-after-free を検出できること)
- usageFlags: 最低 Structured 相当。UAV 可をフラグで。詳細は sub-05 実装メモで固定
- SetComputeTextureFromAsset: 未解決 AssetID／未知名は 0 戻り・落ちない。組み込み (`white` 等) の扱いを SelfTest で 1 本固定
- 文字列: 呼び出しの間だけ有効 (EngineAPI 既存規則)

#### ABI bump 検証手順 (必須・正本はメモリ `abi-bump-verification.md` ＋ 規則 11)

触る一式 (同時コミット想定):
1. `src/Shared/EngineAPI.h` — `#define MYE_API_VERSION 21u`、版履歴コメント、**struct 末尾**にスロット
2. `src/Engine/Engine/Script/EngineApiTable.cpp` — `out.<Name> =` 全充填
3. `src/Scripting/Interop.cs` — struct **末尾**ミラー ＋ `Engine` 静的窓口
4. `src/Shared/ScriptAPI.h` — C++ 糖衣
5. `src/Scripting/MyeScript.cs` — C# 糖衣 (`Engine` は internal → ユーザースクリプトは MyeScript 経由)
6. `tools/check_rules.ps1` — `$apiVersionSlots` に `21 = N` を追加
7. `Project.h` の `kEngineVersion` 等、版を参照する箇所 (メモリ記載どおり存在すれば同期)
8. `PartSelfTest.cpp` (または同等) の `MYE_API_VERSION == 20u` ハードコードを **21** に更新し、新スロット非 null＋既定契約の実行時チェックを追加
9. `docs/history/api-scripting-tools.md` に v21 一行経緯

検証コマンド／手順:
1. bump 前に現行で `tools\check_rules.ps1` PASS を確認
2. スロット追加＋表更新後、再び `check_rules.ps1` PASS
3. **変異テスト**: Interop.cs の隣接 2 スロット名だけ swap → 規則 11 が検出 → 復元 (検査器自体の健全性)
4. `Editor.exe --selftest` (ABI 充填・Create/Dispatch/Release の最小経路)
5. `tools\build_managed.bat` を **Debug と Release 別々**に実行
6. C# レーンは replay 被覆外のため **temp プローブ**: `assets/scripts/<Probe>.cs` を一時作成 → 被覆シーンに付けて `Editor.exe ... --autoplay --frames 60` 等で実走 → **`.cs` と `.cs.meta` を revert** (コミットに残さない)
7. 旧 apiVersion の GameLogic が拒否されることを確認 (または既存 ScriptHost テストに委譲)

## 5. 受け入れ条件

1. プロジェクト `assets/shaders` に Properties 付き `*.post.hlsl` を置き、fxstack 経由で BeforeTonemap または AfterTonemap に挿入できる  
   — 検証: 手動シーン＋スクショ / Editor 起動手順を sub に記載
2. Properties の Float/Range/Color/Vector/2D が Inspector に自動表示され、値変更が描画に反映される  
   — 検証: 手動＋可能なら SelfTest でパース→値パック
3. 故意に壊した HLSL でポストがマゼンタ (または同等の派手な失敗色) になり、エラーがログ／Editor で見える。エンジンはクラッシュしない  
   — 検証: 手動スクショ＋ログ
4. コンピュート用 `*.cs.hlsl`＋Properties を fxstack から Dispatch でき、失敗時はスキップ＋エラー表示  
   — 検証: 単純 UAV fill → 後続ポストで可視化、または SelfTest
5. fxstack 未設定の既存プロジェクト／既定シーンで、ポスト見た目が本機能追加前と実質同一 (余計なフルスクリーンパスが走らない)  
   — 検証: `Editor.exe --selftest` の既存 Post/Merge 系＋必要ならスクショ比較方針
6. Properties パース／パックの回帰 SelfTest が `Editor.exe --selftest` に含まれる  
   — 検証: `--selftest`
7. `tools\check_rules.ps1` がパスする (新定数の C++/HLSL 一致、および **規則 11 の v21 表**)  
   — 検証: `tools\check_rules.ps1`
8. サーフェス／Material.shader 差し替え経路を追加・変更していない  
   — 検証: diff レビュー (GpuResources Material ロード・Deferred GBuffer 固定を触っていない)
9. `MYE_API_VERSION == 21`。GameLogic / C# から Create→SetBuffer／SetFloat*／**SetComputeTextureFromAsset**→Dispatch→Release の最小経路が動き、失敗しても落ちない。規則 11・SelfTest・managed 両構成・(C# は) temp プローブを §4.5 どおり実施  
   — 検証: §4.5 チェックリスト

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | Properties DSL パース＋値パック基盤 | なし | 6, 7 | `M78a: Properties DSL パースと SelfTest` |
| sub-02 | プロジェクトポスト挿入 (Resolve フック＋マゼンタ) | sub-01 | 1, 3, 5, 8 | `M78b: プロジェクトポストの挿入点と失敗マゼンタ` |
| sub-03 | fxstack アセット＋Inspector Properties UI | sub-01, sub-02 | 1, 2, 5 | `M78c: fxstack と Properties Inspector` |
| sub-04 | プロジェクトコンピュート (シーン／スタック駆動) | sub-01, sub-03 | 4, 5, 7, 8 | `M78d: プロジェクトコンピュートのスタック駆動` |
| sub-05 | Compute ABI v21 (GameLogic / C#) | sub-04 | 7, 9 | `M78e: Compute ABI v21 と検証` |

## 7. 未決事項・リスク

- リスク: `Load("*.post")` の拡張子規約が現行 `Load` (`.hlsl` 付与) とどう合成するか — sub-02 で実装確定し SelfTest 固定
- リスク: AfterTonemap と FXAA 配管の順序細部 — 「FXAA 後の LDR」を AfterTonemap 入力とする (上記 4.1)
- リスク: 一時 RT 増えすぎ — 上限と ping-pong 再利用を sub-02/04 で設計
- リスク: ABI バッファ寿命とフレーム跨ぎ — スクリプトが持ったままシーン遷移したときの回収を sub-05 で明示
- リスク: C# レーンが replay 被覆外 — temp プローブを sub-05 受け入れから外さない
- 挿入点・保存形式・マイル確定・Texture スロット必須はユーザー 2A/3A/4A／B で解消済み

## 8. 変更履歴

- 2026-09-22: 初版確定 (PLAN)。スコープをポスト＋コンピュートに改定。AskUserQuestion 不可のため §2 裁定。サーフェスは後回し。
- 2026-09-22: ユーザー回答 1B/2A/3A/4A 反映。ABI v21 をスコープに復帰 (planner 反対意見を §2 に残す)。受け入れ条件 9・§4.5 検証手順追加。sub-05 新設。sub-04 をシーン駆動に限定。
- 2026-09-22: SetComputeTextureFromAsset を v21 最小の**必須**スロットに確定 (ユーザー B)。§4.5・受け入れ 9・sub-05 を更新。planner「6 本まで削可」裁定は覆された。
- 2026-09-22: sub-01 VERDICT OK。Header を `name.empty()` スキーマ、CB をパース順パックに確定 (§4.2)。Reflection オフセットは本マイル不採用。
- 2026-09-22: sub-02 VERDICT REWORK。`--selftest` の `&&` 短絡で先行失敗時に末尾 M78 が未実行になる問題。未到達を PASS としない。集約を `ok &=` 方式へ直し全テスト実行＋総合判定とする裁定 (EditorMain の末尾 append 契約は維持)。
- 2026-09-22: sub-03 VERDICT REWORK。Inspector はスキーマ駆動＋ Tex2D (値/UI/バインド) を must。save-on-apply プレビューは受理 (保存前厳密プレビューは任意)。
