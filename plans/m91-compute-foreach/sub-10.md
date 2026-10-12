# sub-10: 外部プロジェクトの C++ カーネル表 (エディタのスクリプトビルドへの組み込み)

- 依存: sub-05
- 状態: OK (commit ff60f77)
- 往復: 0
- 新設: 2026-10-12 (U12 のユーザー回答「M91 でやる」。sub-05 の次に行う = M91f)

## やること
spec 3 (やる)、受け入れ条件 17。

外部プロジェクト (HAL Collector / 三校など) の C++ スクリプトは、エディタが `<project>\cache\GameLogic.vcxproj` と `GameLogicMain.cpp`、`cache\Generated\` を生成し、MSBuild でビルドする (`src/Editor/Asset/AssetOps.cpp` の `PrepareProjectScriptsBat`。エンジンの gen_project_files.ps1 には依存しない設計)。この経路にカーネル表を足す。

1. **C++ の生成器**: sub-05 の PowerShell の生成器と同じ内容の `Kernels.gen.{h,cpp}` を出す C++ 実装を、Engine 側 (例: `src/Engine/Engine/Compute/KernelCppCodegen.*`) に置く。
   - 引数の HLSL 変数名や struct は、sub-02 の構文解析器から取る (正規表現で拾わない)
   - ハッシュは `HashKernelSource` を使う
   - 検査に落ちたカーネルは表に入れず、ログに出す (C# の生成と同じ扱い)
   - 書き込みは `schema::WriteIfChanged` で行う
2. **組み込み**: `PrepareProjectScriptsBat` で `<project>\assets\shaders\kernels\*.kernel.hlsl` を走査し、生成物を `cache\Generated\Kernels.gen.{h,cpp}` に出す。`WriteGeneratedVcxproj` に `Kernels.gen.cpp` を足す。カーネルが 0 本でも空の表を出して、ビルドが通るようにする。
   - 生成する `GameLogicMain.cpp` の `mod.kernels` / `kernelCount` は sub-05 で入っている (AssetOps.cpp:1659)
   - カーネルの include パスは絶対パスか、cache からの相対パスにする。どちらにしたかと理由を実装メモに書く
3. **実行時のカーネルの置き場**: 外部プロジェクトで起動したとき、ScriptHost の kernelDir (sub-05 では `<assetsRoot>\shaders\kernels`) がプロジェクトの assets を指すことを確かめる。シェーダルートの優先順位 (プロジェクト → エンジン) と矛盾しないようにする。エンジン側の selftest 用カーネル (SelfTest*) がプロジェクトの表に紛れ込まないこと。
4. **2 つの生成器の一致**: エンジンの `assets\shaders\kernels` を C++ の生成器に通し、カーネル表 (名前・ハッシュ・引数名・RW/RO・stride・params の大きさ) が PowerShell 版 (コミット済みの `src/GameLogic/Generated/Kernels.gen.*`) と一致することを selftest で確かめる。include パスのような、環境で変わる行は比較から外してよい。どこを外したかは実装メモに書く。
5. **実走**: 一時プロジェクト (scratchpad に作る。ルールに従い、作業後に消す) にカーネル 1 本と、宣言付きの C++ スクリプト 1 本を置く。エディタの [Rebuild Scripts] と同じ経路 (bat) でビルドし、Play か headless で State の ForEach が成功することを確かめる。手順と結果を実装メモに書く。カーネルを 1 文字変えるとハッシュ不一致で失敗し、再ビルドで戻ることも確かめる。

## やらないこと (このサブでは)
- C# 側 (外部プロジェクトの C# の CPU 版は、sub-06 の実行時変換で `<assetsRoot>` に出る)
- gen_project_files.ps1 の置き換え (エンジンのリポジトリ用として残す)

## 触る場所 (planner の見立て)
- `src/Editor/Asset/AssetOps.cpp` (`PrepareProjectScriptsBat` は 1767 行付近、`WriteGeneratedVcxproj` は 1687 行付近)、`src/Editor/Asset/AssetOpsSelfTest.cpp`
- 新規: `src/Engine/Engine/Compute/KernelCppCodegen.*` (sub-02 の KernelParser と KernelHash を使う)
- ScriptHost の kernelDir の設定箇所 (sub-05)
- `tools\gen_project_files.ps1` の再実行 (ファイル追加のため)

## 受け入れ条件 (このサブ)
1. spec 5-17: 2 つの生成器の一致 (selftest)。一時プロジェクトでの実ビルドと実走、版ずれでの失敗と復帰 (手順と結果の記録)
2. カーネルの無い外部プロジェクトでも [Rebuild Scripts] が今までどおり通る (selftest で生成物を確かめるか、一時プロジェクトで確かめる)
3. `tools\check_rules.ps1` が PASS する

## 検証コマンド
- `tools\gen_project_files.ps1`、Debug / Release のビルド、`Editor.exe --selftest`、`tools\check_rules.ps1`、一時プロジェクトでのビルド (使い終わったら消す)

## 実装メモ (coder が追記)

SELF_EVAL: sub-10 (round 1)
- 実装: Engine/Compute/KernelCppCodegen.{h,cpp} (新規。BuildKernelsCpp / WriteCppKernels。引数名は LintKernelSource の KernelProgram.kernel.buffers、ハッシュは result.sourceHash = HashKernelSource、書き込みは schema::WriteIfChanged、検査に落ちたカーネルは表に入れずログ)。AssetOps.cpp: PrepareProjectScriptsBat が `<project>\assets\shaders\kernels` を走査して `cache\Generated\Kernels.gen.{h,cpp}` を出す、WriteGeneratedVcxproj が Kernels.gen.cpp を ClCompile に足し、include パスにエンジンの `assets\shaders\kernels` を足す。KernelLintSelfTest.cpp: TestCppTable。
- include の方式: カーネル本体の #include は絶対パス (理由: cache\Generated からの相対だとプロジェクトの置き場所で表の中身が変わり、無駄な再ビルドが出る)。`kernel_common.hlsli` は vcxproj の AdditionalIncludeDirectories にエンジンの `assets\shaders\kernels` を足して解決 (プロジェクトの kernels に同名があればそちらが先)。GPU 側は既存の ShaderManager::ResolveInclude が dirs_ (プロジェクト → エンジン) → `kernels\` の順で引くので矛盾しない。
- kernelDir の確認: SimInit.cpp:47 `assetsRoot = projectRoot + \assets` → EngineLoop.cpp:521 の kernelDir は `<project>\assets\shaders\kernels`。実走で確認済み (宣言どおり成功し、版ずれ時は hash-mismatch)。エンジンの SelfTest* カーネルはプロジェクトの kernels に無いので表に入らない (実走の表は 1 本だけ)。
- selftest の一致比較で外した所: 先頭の banner (4 行。生成器ごとに文面が違う) だけ。include パス行も含めて全部一致 (genDir 基準の相対パスで作り直して比較)。PowerShell 版が出す表は名前・ハッシュ・引数名と C++ の型 (RW/RO・stride・params は GameLogic のコンパイル時の型) なので、文字列一致 = 同じ表。src\GameLogic\Generated が無い配布物では SKIP。
- 仕様との差分: [追加] KernelCppCodegen の API は WriteCppKernels(kernelDir, genDir, headerInclude, absoluteIncludes) — エンジン側 (相対) と外部 (絶対) の両用。[追加] banner は PowerShell 版と別文面。[追加] PowerShell 版はカーネル名とファイル名の不一致で throw、C++ 版は検査器の KL020 で表から外してログ。
- 検証: gen_project_files.ps1 (pwsh) → Kernels.gen.* は変化なし / Debug・Release MyEngine.sln ビルド成功 / Debug Editor --selftest: 1 回目は exit 1 (Server/client net self test の「LoadPersist / LoadGame」2 件 FAIL。sub-05 で既知の不安定箇所)、2 回目 exit 0・FAIL 0。Release exit 0・FAIL 0 / check_rules.ps1 → 0 error。
- 実走 (一時プロジェクトを scratchpad に作成、後で削除): `Editor.exe --project <p> --autoplay --frames N` で、DLL 未ロード時の自動 Rebuild (= [Rebuild Scripts] と同じ PrepareProjectScriptsBat → bat → MSBuild) が通り、`TmpSaxpy.kernel.hlsl` 1 本 + 宣言付き `TmpProbe` で State の ForEach が job=1 (CPU 完了)、y[3] = 3.5 → 5.5 → 7.5 → 9.5 (期待どおり)。カーネルへ `// edited` を足して再ビルドせず実行 → `ForEach 'TmpSaxpy' failed: hash-mismatch`・job=0・y 不変。DLL を消して実行 (自動再ビルドでハッシュが更新) → 次の起動で job=1 に復帰。カーネル 0 本 + カーネルを使わないスクリプトでもビルドが通りロードされ、空の表が出る。注意: 初回は DLL ロード前にシーンを読むため TmpProbe は unknown component になる (既存の挙動)。
- 自己採点: 仕様適合 4 (全項目を実施。GUI の [Rebuild Scripts] ボタンそのものではなく自動 Rebuild 経由) / 正しさ 4 / コード品質 4 / テスト 4 (一致・0 本・違反・絶対パスを selftest。実ビルドは手動手順)。
- 不安・質問: 初回起動の自動 Rebuild 後に hot reload が走ったとき、unknown component で保持されたシーンの TmpProbe が復活するかは未確認 (本サブの範囲外)。
- 触ったファイル: src/Engine/Engine/Compute/KernelCppCodegen.{h,cpp}、src/Engine/Engine/Compute/KernelLintSelfTest.cpp、src/Editor/Asset/AssetOps.cpp、build/Engine.vcxproj(.filters)。
- 申し送り: 外部プロジェクトの kernels に engine と同名のカーネルがあると ShaderManager のプロジェクト索引では競合扱いにならない可能性 (未確認)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。差分 4 件はすべて採用した。net の selftest の不安定さは sub-11 で切り分ける。同名のカーネルがプロジェクトとエンジンの両方にある場合の扱いは sub-07 に申し送った。
