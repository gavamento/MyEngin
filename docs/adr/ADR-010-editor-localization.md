# ADR-010: エディタの日本語化 (i18n 基盤 + 日本語を既定言語に)

## 決定

- エディタ UI の既定言語を**日本語**にし、英語も残して**実行時に切り替えられる**ようにする
- 文字列の実体は **X マクロ 1 ファイル** (`src/Engine/Core/LocalizationTable.inl`) に置き、
  `StrId` enum と英語/日本語の 2 配列を同じファイルから生成する
- ウィンドウ名とモーダル名は **`"表示名###英語ID"`** 形式にして、表示だけを訳し ID は固定する
- Inspector の**フィールド表示名は `FieldDesc::displayName`**、**コンポーネント表示名は
  エディタ側の `EditorComponentCatalog`** に置く。`name` (英語) には一切触れない
- ログは**ユーザー向けのものだけ**訳す。開発者向け診断と selftest は英語のまま
- 用語は **Unity 日本語版**に合わせる。固有名詞・略語 (Forward / Deferred / SSAO / IBL /
  SVGF / BVH / ACES / Roslyn …) は日本語モードでも英語で置く

## 理由

### なぜ「日本語化」がマイルストーンになったか

M46 の時点で、ソースには**日本語のリテラルが 21 ファイル 112 箇所**あった。トースト 9 箇所は
すべて日本語、ツールバーの tooltip も日本語、一方でメニューと Inspector は全部英語。
`ProjectManager.cpp` に至っては英語のエラーと日本語のエラーが隣り合っていた。
つまりこれは「新しく日本語対応を足す」話ではなく、**なし崩しに始まっていた混在を仕様として
決着させる**作業だった。就活ポートフォリオとして日本語で読めることにも価値がある。

### なぜ X マクロで、JSON 外部化ではないか

```cpp
MYE_STR(Win_Hierarchy, "Hierarchy###Hierarchy", "ヒエラルキー###Hierarchy")
```

この 1 行から enum と 2 本の配列が生成されるので、**訳を書き忘れるとマクロの引数が足りず
コンパイルエラーになる**。網羅性の保証が無料で付いてくる。

JSON にすればホットリロードできて「エンジンらしい」が、欠損キーが実行時フォールバックに化けて
静かに劣化する。さらに `nlohmann::json` を UI 初期化パスに持ち込むことになる。
このエンジンは「壊れない開発体験」を掲げているので、**実行時の柔軟さより、壊れたら
ビルドが通らないこと**を選んだ。

翻訳者が実在するプロジェクトなら判断は逆になりうる。ここは開発者 = 翻訳者なので、
コンパイラに検査させられる形が最も安い。

### なぜ `###` でウィンドウ名を分離するのか

ウィンドウ名は表示文字列であると同時に**ImGui のウィンドウ ID** で、しかも

- `EditorApp.cpp` の `DockBuilderDockWindow("Hierarchy", ...)` 14 箇所
- `layouts_.Init` のパネル開閉表 (= `<name>.panels.json` の JSON キー)
- 保存済みの `imgui.ini` / `layouts/*.ini` の `[Window][Hierarchy]`

の 3 者がこの文字列で結合している。素朴に訳すと既定レイアウトが崩れ、**ユーザーが保存した
レイアウトが全部孤児になる**。

ベンダリングしている ImGui 1.92.8 の `ImHashStr` (`imgui.cpp:2555-2568`) は `###` を見つけると
ハッシュをシードへ戻し `###` の 3 文字も読み飛ばす。さらに `CreateNewWindowSettings`
(`:16409-16426`) が同じ正規化を通すので、**ini に書かれるキーも `###` 以降だけ**になる。

```
"ヒエラルキー###Hierarchy" の ID  ==  "Hierarchy" の ID   (完全一致)
```

結果、DockBuilder も panels.json も既存 ini も**1 バイトも変えずに**日本語表示にできた。
実測でも、日本語 UI で起動した後の `imgui.ini` が M47 以前とバイト一致している。

副次的な効能として、`OpenPopup` と `BeginPopupModal` の間で言語が切り替わってもモーダルの
ID が変わらないので、「切り替えた瞬間にダイアログが消える」事故が原理的に起きない。

### なぜフィールドは Core、コンポーネントは Editor なのか

`FieldDesc::name` と `ComponentDesc::name` は**表示名ではなくキー**である:

| 用途 | 場所 |
|---|---|
| シーン JSON のキー | `SceneSerializer.cpp` / `Prefab.cpp` / `ComponentClipboard.cpp` |
| ワールドハッシュ | `WorldHasher.cpp:38` (`desc.nameHash`) |
| DLL リロード時のフィールド移行キー | `World.cpp:456` の `strcmp(nf.name, of.name)` |
| .anim.json のターゲット指定 | `Animation.cpp:248-264` |

したがって訳すのは不可能で、**表示専用のスロットを別に持つ**しかない。置き場は 2 通りあり、
どちらにも既存の前例があった:

- **フィールド → `FieldDesc` に追加**。`Reflection.h` には M8 で入れた
  「Inspector 用メタデータ (ABI 非依存)…シリアライズ/ハッシュ/DLL 移行には影響しない」
  というブロックが既にあり、`dragSpeed` / `minVal` / `maxVal` / `tooltip` が同居している。
  `displayName` はここに 1 つ足すだけで済み、**定義の隣に訳が書ける** = 名前を変えたときに
  訳が取り残されない
- **コンポーネント → `EditorComponentCatalog`**。M33b で作った
  「ComponentRegistry には手を入れず、エディタ側の name→{icon,category} 表で完結させる」
  機構が既にあり、アイコンとカテゴリが載っている。25 エントリに 1 語足すだけ

非影響であることは機械的に確認できる。`WorldHasher.cpp:38-45` は `desc.nameHash` と
**フィールドのバイト列**しか読まず、`f.name` にすら触れない。よって `FieldDesc` にメンバを
足してもハッシュ入力には物理的に到達しない。受け入れ基準は「**`golden.rep` を再記録せずに
`replay_verify.bat` が通ること**」とし、実際に通った。

### なぜログは「ユーザー向けだけ」なのか

`MYE_LOG_*` は 446 箇所あるが、性格が 3 つに分かれる:

| 分類 | 数 | 扱い |
|---|--:|---|
| ユーザー向け (アセット操作の成否・次の一手の案内) | 約 100 | **日本語化** |
| 開発者向け診断 (シェーダ / GPU / BVH / ホットリロード) | 約 200 | 英語のまま |
| selftest の PASS/FAIL | 118 | 英語のまま |

判断軸は「**トーストに出したい文言か**」。`ToastCenter.cpp:83` が Warn/Error のログを
そのままトースト化するので、ユーザー向けログの日本語化はトーストの日本語化とほぼ同義になる。

一方、シェーダのコンパイルエラーや HRESULT は D3D デバッグレイヤの英語出力と並ぶ場所で、
日本語にすると英語資料との突き合わせが面倒になるだけで得がない。selftest の `TEST_CHECK` は
`#cond` (式の文字列化) が必ず ASCII なので、周りだけ日本語にすると読みにくくなる。

### なぜ語順を固定するのか

MSVC の `printf` 系は **POSIX の位置指定引数 `%1$s` に対応していない** (対応するのは
`_vsnprintf_p` という別系統)。したがって「言語によって語順を変える」ことは原理的にできない。
規則 10 の静的検査で **en/ja の変換指定子の並びが一致すること**を機械的に強制し、
語順を変えたい訳が必要なら文を分割する運用にした。

## 影響

### 静的検査 (規則 10)

`tools/check_rules.ps1` に 3 本追加した。「英語リテラルが残っていないか」を検査する案は
**採らなかった** — `ICON_FA_*` 連結 / `"##id"` / `"%s"` / `\0` 区切りコンボ / `PushID` …と
許可リストが規則本体より大きくなり、誤検知が出た瞬間に形骸化するため。代わりに:

1. **`Tr()` を printf 系の唯一の引数にしていないこと**。訳文中の `%` が変換指定子として
   解釈されるのを防ぐ。可変引数を伴う `Text(Tr(X), a, b)` は「訳文自体が書式」という
   正当な用法なので許す (並びは 2 が保証する)
2. **テーブルの整合**: en/ja とも非空 / `###` 以降が一致 / `###` 右辺がテーブル内で一意 /
   変換指定子の並びが一致
3. 検査は `[System.IO.File]::ReadLines` で読む。`Select-String` は Windows PowerShell 5.1 で
   BOM 無しファイルを ANSI として読むため、**日本語を含む行のマッチが不発になる**

### 副次的に直った既存の不具合

- `Log.cpp` は `OutputDebugStringA` / `fputs` で出していたため、**既にあった日本語ログが
  コンソールとデバッガで文字化けしていた**。`OutputDebugStringW` と、コンソール直結時の
  `WriteConsoleW` 分岐に変更した。`SetConsoleOutputCP(CP_UTF8)` は使っていない — コードページは
  「コンソール」側の状態でプロセス終了後も残り、`AttachConsole(ATTACH_PARENT_PROCESS)` で
  親の cmd に相乗りしている以上、**呼び出し元のシェルを 65001 のまま壊してしまう**
- `Log.cpp` の切り詰めが `strncpy_s(_TRUNCATE)` = バイト境界切りで、`message[240]` の末尾で
  マルチバイト列を分断していた。`utf8::CopyTruncated` に置換
- `StatusBar.cpp` の `char info[256]` は**日本語のプロジェクト名/シーン名で実際に溢れていた**。
  `Format()` (`Core/Format.h`) で `std::string` 化
- SceneView のツールバーが `BeginChild(ImVec2(830, 30))` の固定幅だった。
  `ImGuiChildFlags_AutoResizeX` に変更 (訳文の長さに追随する)

### 運用上の制約 (v1)

- **日本語フォントが 1 つも無い環境では自動的に英語へ落ちる**。`SetupEditorFonts()` の
  戻り値を「日本語グリフが描けるか」に変え、false なら `SetLanguage(Lang::En)` にする。
  これが無いと英語版 Windows の最小構成で画面全体が豆腐になる
- **`--selftest` / `--replay-verify` / `--screenshot` 時は強制的に英語**。将来ログ文字列を
  解析する検証を足したくなったときに環境依存を持ち込まないため。`--lang <ja|en>` で上書き可
- **言語設定は `%LOCALAPPDATA%\MyEngine\editor_global.json`**。Hub (プロジェクト選択) は
  `RelaunchSelfWithProject` で自分を再起動する別プロセスで、描画時点ではプロジェクトが
  未確定なので `<project>\.mye\editor_settings.json` では間に合わない
- **切り替え後の新規ログだけが新言語**。リングバッファ (4096 件) に既にある行は旧言語のまま
  Console / StatusBar に残る (`LogEntry` の POD 性を壊さないため)
- **スクリプト由来のコンポーネント/フィールドは英名のまま**。C++ は `MyeScriptField`、
  C# は `GetFieldInfo` の vtable に表示名スロットが無く、足すと `MYE_API_VERSION` の
  bump が伴う。英名フォールバックで実害が無いので v1 では見送った
- **オブジェクト名は訳さない**。`CreateItem(..., menuLabel, objName, fn)` の第 3 引数は
  `NameComponent` の値で、シーン JSON に載りワールドハッシュの入力にもなる「データ」。
  Unity 日本語版も "Cube" を作る
- **英語時代に保存された ini のタブ幅では日本語のタブ名が切り詰められる**
  (「コンソール」→「ソール」)。ImGui のタブ縮小の正常動作で、レイアウトをリセットすれば直る
