# 三校企画 3Dアセット制作 引き継ぎ

更新日: 2026-09-09 / 対象: 別PCで作業を再開する本人・制作担当・AIエージェント。

## 1. 最初に読むこと

この文書は企画、会話での確定事項、制作記録、移行時点の実ファイル確認をまとめたものです。企画書本体は `三校企画.md`、リポジトリの作業ルールは `AGENTS.md` です。

**重要: 前回生成した `assets/models/enemy_crawler_c_v01/` は、今回の引き継ぎ作成時点では元の場所に存在しません。** 移動・削除の経緯は不明です。前回のFBX生成・検証は実施済みですが、現時点でその成果物をコピーできるとは限りません。生成元の `tools/crawler_c_v01/` は残っています。移動先・バックアップを確認し、見つからなければ下記の手順で再生成できます。この引き継ぎ作業では復元・再生成・削除を行っていません。

会話中の「これでいいよ」はC案の制作開始前の仕様承認です。生成後の初版について、外見の最終承認はまだ得ていません。その後の依頼は別PC向けの文書化です。

## 2. ユーザーとの進め方

- 最初は質問のみ、明確に「作成」と言われるまでは作らない、という方針から開始しました。C案には作成許可があります。
- 企画書・合意済み仕様にない内容は、作る前に質問してください。選択肢が必要なら複数案を提示し、ユーザーが選びます。
- 最小構成から進め、1つずつ仕上げて確認してもらいます。他のモデルへ勝手に進みません。
- エンジンの調査は許可されていますが、変更・削除は許可されていません。アセット制作とエンジン改修を混同しないでください。
- ファイル削除は `AGENTS.md` に従い、絶対パス・目的・影響を示して事前承認を得てください。
- 既存変更を取り消さないこと。再生成も成果物の上書きになるため、手修正済みデータを必ず保護してください。

## 3. 確定した制作方針

| 項目 | 確定内容 |
| --- | --- |
| 用途 | Windows用3Dゲーム、自作DirectX 11エンジン |
| 形式 | FBX |
| 見た目 | 写実的で不気味 |
| 納品範囲 | メッシュ、UV、マテリアル、リギング、アニメーション |
| 視点 | 一人称 |
| 性能目標 | 実機60fps基準。スローモーション用は不要 |
| ステージ | 地下の廃研究所。最初は中規模の部屋と通路 |
| ステージ品質 | ユーザーは完成品質を希望。現行セットの実機検証は別途必要 |
| 劣化表現 | 汚れと錆。錆・金属などをマテリアル分けで表現 |
| 備品 | 配置可。ただし物ごとに分離 |
| 扉 | 開閉可能な扉を別FBXで作る |
| 敵 | C案とA案を用意する方針。現在の制作対象はC案のみ |
| 光の敵 | 当面なし。企画書に登場しても現在の制作範囲へ戻さない |
| 光ひるみ | 光を浴びた際の中程度のひるみを含める |
| 懐中電灯 | 通常形状の案だが、制作は後回し |
| プレイヤー | 男性、30代前半、汚れた白衣風の研究員服、手袋あり |
| プレイヤー範囲 | 一人称用の腕・足を中心とする。追加装飾は不要 |

A案の具体的な造形説明はこの引き継ぎで確実に復元できていません。「A」という記号だけからデザインを推測しないでください。プレイヤーの完成済みモデルも今回確認できていません。会話に作成依頼があることと、成果物の存在は別です。

## 4. 企画の概要とアセットとの境界

企画の中心は「音が周囲の形を照らすが、敵にも居場所を知らせる」です。歩行・走行・呼吸・投擲により波が発生し、床材によって音量と波の性質が変わります。残光が薄れることで、視界の確保と危険回避を両立させる必要があります。

設置光は地形の可視化、安全地帯、消費型リスポーン地点を兼ねます。敵には巡回・警戒・探索・追跡などの状態があり、状態による波の違いをプレイヤーが読み取ります。音の伝播描画とAI用伝播計算は分離する企画です。

これらの波・残光・照明・聴覚AI・床材判定・復活・ゲーム操作を、今回のFBXが実装しているわけではありません。確認画像は形状を確認する通常照明のレンダーです。

企画書と会話には設置光と懐中電灯の両方が出てきます。道具を作る段階で役割を確認してください。また、企画書の光の敵は現在保留です。過去の番号付き短答で参照先が不明なものを、企画書の同番号の章への承認だと決めつけないでください。

## 5. 別PCへ持っていくもの

移行元のプロジェクトルート: `C:\HAKtokyo\My_Engin\MyEngin`

最も確実なのはプロジェクト全体をコピーすることです。Gitだけで移す場合、未追跡ファイルはclone/pullでは移動しません。今回の確認では次の3ディレクトリが未追跡でした。自動コミットはしていません。

```text
assets/models/underground_lab_v01/
tools/lab_assets_v01/
tools/crawler_c_v01/
```

この `ASSET_HANDOVER.md` も新規作成ファイルなので移行対象に加えてください。

| 必須対象 | 理由 |
| --- | --- |
| 三校企画.md | 原企画 |
| AGENTS.md | 作業ルール |
| ASSET_HANDOVER.md | 本引き継ぎ |
| assets/models/underground_lab_v01/ 全体 | 研究所FBX・テクスチャ・原本・検証記録 |
| tools/lab_assets_v01/ 全体 | 研究所生成元、C案も使用するPBR出力補助 |
| tools/crawler_c_v01/ 全体 | C案の再生成・検証元 |
| external/ufbx/ | C検証プログラムの依存ソース |
| src/・assets/・build/・MyEngine.slnなど | 実機導入とエンジンのビルドに必要なプロジェクト一式 |
| C案出力のバックアップがあれば全体 | FBXだけでなくtextures・blend・manifest・検証結果も必要 |

FBXに画像は埋め込んでいません。FBXの隣の `textures/` を省かないでください。既存の `.meta` などもプロジェクト移行時に保持してください。移行先パスは同一でなくても構いません。生成スクリプトは自身の位置からプロジェクトルートを求めますが、`tools/` と `assets/` の階層は維持してください。

## 6. 研究所セットの状態

現在存在する出力: `assets/models/underground_lab_v01/`

| ファイル | 内容 |
| --- | --- |
| Lab_Room.fbx | 内寸10m x 8m、天井高3mの部屋 |
| Lab_Corridor.fbx | 幅2.5m、中心線長15mのL字通路 |
| Lab_Bench.fbx | 実験台 |
| Lab_Sink.fbx | 流し台 |
| Lab_StorageShelf.fbx | 金属棚 |
| Lab_MetalFloorPlate.fbx | 金属床板 |
| Lab_GlassShards.fbx | ガラス片 |
| Lab_WaterPuddle.fbx | 水たまり表面 |
| Lab_Door.fbx | 扉枠・扉・開閉リグとクリップ |
| Lab_Review.blend | 編集用原本、配置と撮影環境 |
| asset_manifest.json | 配置・三角形数・材質・画像対応 |

9 FBX、合計153,156三角形、13材質、39枚の2K PNG。材質定数の粗さ・金属度を使用しています。備品の位置はmanifestの `engine_position` を参照し、回転ゼロ・スケール1が前提です。

扉は `DoorRoot` / `DoorHinge` の2ボーン。`Lab_Door_Rig|00_Door_Open` と `Lab_Door_Rig|01_Door_Close` は各1秒、60fpsで90度開閉します。先頭クリップ自動再生のローダー動作に注意し、閉じて置く場合は開くクリップの時刻0で停止させます。

過去の検証ではufbx読み込み、UV・画像参照・材質・扉ウェイトと開閉、Blender再読み込み、5方向の画像確認を通過しています。詳細はセット内のREADMEと検証ログを参照してください。ゲーム内表示、衝突、操作連動、足音材質連動は未検証です。水とガラスは半透明表面であり、屈折・水面シミュレーションはありません。

## 7. C案の承認済みデザイン

「多関節の這い寄り型」。細長い四肢で床や壁を這い、指先と顎の振動器官で音を探ります。目は残っているもののほぼ機能しません。

- 伸ばした体格は約2m、這った高さは約60cm。
- 裸体、灰白色で汚れた皮膚、細かいしわ。
- 小さく白濁した退化眼。指先と顎の感覚器官を強調。
- 流血や露出内臓はなし。不気味さは体型と関節から出す。

前回出力先: `assets/models/enemy_crawler_c_v01/`。現在不在のため、以下は**前回生成時の記録**であり、現在のファイル存在保証ではありません。

### データ構成

`Enemy_Crawler_C.fbx`、`CrawlerC_Review.blend`、`asset_manifest.json`、`README.md`、`textures/`、`previews/`、`validation_ufbx.txt`、`validation_blender.json` を出力していました。

131,648三角形、5メッシュ、64変形ボーン、ルート込みで最大65スキンクラスター、最大4ウェイトです。以前の説明に「65変形ボーン」とありますが、正確にはこの区別です。2K画像15枚は5材質それぞれのBaseColor・Normal・Normal_DXです。粗さは画像ではなく定数です。

メッシュ名は `CrawlerC_Skin`、`CrawlerC_Sensor`、`CrawlerC_Keratin`、`CrawlerC_Crease`、`CrawlerC_CloudedEye`。リグオブジェクトは `CrawlerC_Rig`、アーマチュアデータは `CrawlerC_Skeleton`。

皮膚は球・管状形状をボクセル結合し、平滑化・ポリゴン削減・UV展開した構成です。ウェイトは近傍ボーンと隣接関節に制限して生成しています。手足には3ボーンIKと回転追従を使い、制約の結果をFBXへベイクしています。ソースのIKターゲットはゲーム用変形ボーンとは別です。

### アニメーション一覧

全クリップ60fps。フレーム0から終端までを含みます。FBX名の接頭辞は `CrawlerC_Rig|`。Blender再インポート時には接頭辞が二重になる場合があります。

| クリップ末尾 | 秒 | 終端フレーム | ループ |
| --- | ---: | ---: | --- |
| 00_Idle | 3.0 | 180 | Yes |
| 01_Patrol_Crawl | 2.4 | 144 | Yes |
| 02_Alert | 1.0 | 60 | No |
| 03_Search | 3.0 | 180 | Yes |
| 04_Chase | 0.9 | 54 | Yes |
| 05_Attack | 1.2 | 72 | No |
| 06_Hit | 1.0 | 60 | No |
| 07_LightFlinch | 1.5 | 90 | No |
| 08_Death | 2.0 | 120 | No |
| 09_LightBoundary | 2.4 | 144 | Yes |
| 10_WallCrawl | 2.0 | 120 | Yes |

全てその場再生です。ゲーム側で移動速度・位置・回転を与えます。WallCrawlはローカル接地面を壁へ合わせて使用する動作で、壁への吸着処理や床から壁への乗り移り専用動作は含みません。死亡は終端を保持します。光ひるみは前肢を頭側へ引き、体を引いて戻す中程度の反応です。

指に関節リグはありますが、現行アニメーションは主に手全体の接地・探索です。指ごとの独立した探索演技を完成済みと扱わないでください。

### 前回の検証結果と品質

ufbxによる実FBX読み込み、全11クリップの60Hzベイク、各クリップ9姿勢のスキニング、座標の有限性、ウェイト正規化、4影響以下、骨数、UV、材質を検査しました。ループ指定クリップの端点差は0。検査姿勢で最小高さは死亡時約0.0038m、最大広がりは追跡時約2.54m。`RESULT failures=0` でした。

BlenderでもFBXを読み直し、5メッシュ・1リグ・11アクション、三角形・UV・ウェイト・2K画像、8枚の非空プレビューを検証し、失敗0でした。画像は全身・正面・側面・頭部・攻撃・光ひるみ・死亡・壁這い。壁画像は指先が切れないようレンズ40mmへ修正済みです。

ただし、これは**確認用初版**です。丸みの強い単純な造形が残り、写実的な皮膚・解剖学的ディテールの完成品質には未達です。全フレームの自己交差、足滑り、全アニメーション遷移は検証していません。LODなし。実機描画60fpsは未測定です。60fpsベイクと60fps描画性能を混同しないでください。

## 8. 開発・制作環境

元PCはWindows / PowerShell。Blender 4.2.3 LTSを使用しました。

```text
Blender: C:\Program Files\Blender Foundation\Blender 4.2\blender.exe
GCC:     C:\mingw64\bin\gcc.exe
Engine:  Visual Studio 2022 / C++20 / DirectX 11
```

PythonスクリプトはBlender内蔵Pythonで動かします。`bpy`・`numpy` を使用します。通常のPythonで直接実行しないでください。別PCではまず同じBlender版を使うと差異を減らせます。別版への移行ではFBX出力APIと材質互換補助を再検証してください。GCC実行ファイルも別PCで再ビルド可能です。

エンジン実行は `MyEngine.sln` をVisual Studioで開き、`Debug|x64` でF5がリポジトリの案内です。エンジンを改修する場合のみ、別途許可を得て `AGENTS.md` のテスト規則に従ってください。

## 9. 再開・再生成コマンド

以下は別PCで実行するための例です。今回の文書化では実行していません。パスを実環境に合わせ、PowerShellで1つずつ実行して終了コードを確認してください。

```powershell
$Project = 'D:\Work\MyEngin'
$Blender = 'C:\Program Files\Blender Foundation\Blender 4.2\blender.exe'
$Gcc = 'C:\mingw64\bin\gcc.exe'
Set-Location -LiteralPath $Project
Test-Path '.\tools\crawler_c_v01\build_crawler.py'
Test-Path '.\tools\lab_assets_v01\build_lab.py'
Test-Path '.\external\ufbx\ufbx.c'
Test-Path '.\assets\models\enemy_crawler_c_v01'
git status --short
```

### C案を出力がない状態から作り直す

```powershell
& $Blender --background --factory-startup --python-exit-code 1 --python '.\tools\crawler_c_v01\build_crawler.py'
$LASTEXITCODE
```

既存出力フォルダーがあると保護のため停止します。次は**既存生成物を上書きする操作**なので、手修正のないことを確認し、別フォルダーへバックアップしたうえで必要な場合だけ使います。

```powershell
& $Blender --background --factory-startup --python-exit-code 1 --python '.\tools\crawler_c_v01\build_crawler.py' -- --rebuild
```

この再生成は `.blend` の手編集を取り込まず、スクリプトから作り直します。FBX・blend・画像・manifestは再生成できますが、前回手書きしたREADMEやufbxログは自動では戻りません。本引き継ぎから説明を再構成し、ログは以下で出力してください。

### C案のufbx検証

```powershell
& $Gcc -O2 -I '.\external\ufbx' '.\tools\crawler_c_v01\verify_crawler.c' '.\external\ufbx\ufbx.c' -lm -o '.\tools\crawler_c_v01\verify_crawler.exe'
$LASTEXITCODE
& '.\tools\crawler_c_v01\verify_crawler.exe' '.\assets\models\enemy_crawler_c_v01\Enemy_Crawler_C.fbx' | Tee-Object -FilePath '.\assets\models\enemy_crawler_c_v01\validation_ufbx.txt'
$LASTEXITCODE
```

期待値は終了コード0、`RESULT failures=0`、11クリップです。既存exeを再コンパイルすると上書きされます。

### C案の姿勢画像とBlender再読み込み検証

```powershell
& $Blender --background --factory-startup --python-exit-code 1 --python '.\tools\crawler_c_v01\inspect_crawler.py'
$LASTEXITCODE
```

先に生成を完了させてください。4枚の姿勢画像と `validation_blender.json` を書きます。原本blendは保存し直しません。生成スクリプトと同時実行すると書き出しと読み込みが競合するため、順番に実行してください。

### 研究所の検証

```powershell
& $Gcc -O2 -I '.\external\ufbx' '.\tools\lab_assets_v01\verify_fbx.c' '.\external\ufbx\ufbx.c' -lm -o '.\tools\lab_assets_v01\verify_fbx.exe'
$Files = @(Get-ChildItem -LiteralPath '.\assets\models\underground_lab_v01' -Filter '*.fbx' | ForEach-Object { $_.FullName })
& '.\tools\lab_assets_v01\verify_fbx.exe' @Files
$LASTEXITCODE
& $Blender --background --factory-startup --python-exit-code 1 --python '.\tools\lab_assets_v01\verify_blender.py'
```

研究所の `finish_lab.py --export-only` は編集済みblendからFBXを再出力する補助です。成果物を書き換えるため単なる検証として実行しないでください。`build_lab.py` は出力が存在すると停止します。既存フォルダーを削除して強行せず、新版出力先またはバックアップを用意してください。

## 10. FBX互換性の注意点

調査対象は `src/Engine/Engine/FbxLoader.cpp` です。別PCのコード版が変わっていたら再確認してください。

- ufbxで左手系Y-up、メートルへ変換し、ZミラーとADJUST_TRANSFORMSを使用する設定に合わせています。
- UVは1セット。読み込み側でVを反転します。
- 最大4ボーン影響。パレット上限128に対し祖先を含めて余裕を持たせています。
- スキンアニメーションは60Hzでサンプリングされます。
- BaseColorとNormalを使い、金属度・粗さは定数です。
- `_Normal_DX.png` はエンジン用に緑成分を反転、`_Normal.png` はBlender用です。
- Blender標準FBXのPBR解釈だけでは金属度が期待どおりにならないため、`build_lab.py` の `install_pbr_export()` がプロセス内でPhysicalMaterial互換プロパティを付けます。
- この補助はBlender本体やエンジンファイルを書き換えません。C案の生成元もこれをimportするため、研究所ツールを省いて移行できません。
- 通常のBlender GUIからのFBX再出力では互換プロパティが失われる可能性があります。手修正後のC案に専用再出力手順を整える場合、原本・DXノーマル参照・材質互換・全クリップを再検証してください。
- C案のBlender座標はZ-up、前方-Y。エンジンではY-up、前方-Zです。

## 11. 次の担当者の作業順

1. 本文書と企画書、AGENTS.mdを読み、コピーしたファイルの存在とGit状態を確認する。
2. 不在のC案出力について、移動済みかバックアップがあるか確認する。新たな削除や再生成で追跡を難しくしない。
3. 必要ならユーザーに確認してC案を再生成し、ufbxとBlenderの検査を再実行する。
4. 全身、頭部、壁這い、光ひるみの画像と動作をユーザーに提示する。初版を完成品扱いしない。
5. 外見の修正点を確認し、写実的造形、皮膚、指の演技を調整する。修正後は変形と自己交差を重点検証する。
6. 実機導入は許可を得て別作業として行う。材質、法線、スケール、床接地、壁方向、ループと遷移、衝突・攻撃タイミングを確認する。
7. 解像度・GPU・敵数・影設定を記録して60fpsを測定する。必要なLODやポリゴン削減は測定結果に基づき進める。
8. C案の確認後、次にA案・プレイヤー・その他のどれを作るかユーザーに確認する。光の敵と懐中電灯は保留を維持する。

## 12. 次のAIへ渡す開始文

> このリポジトリのAGENTS.md、ASSET_HANDOVER.md、三校企画.mdを読んで作業を再開してください。自作DirectX 11向けFBXアセット制作です。現段階はC案「多関節の這い寄り型」の初版確認・調整で、写実的な完成品質には未達です。前回出力フォルダーは引き継ぎ時点で不在ですが生成スクリプトは残っています。まずファイルの実在とバックアップを確認してください。企画にないことは質問し、1モデルずつ確認します。エンジンの変更・ファイル削除・他モデルの新規制作は勝手に行わないでください。アニメーション60fpsと実機60fpsは区別してください。
