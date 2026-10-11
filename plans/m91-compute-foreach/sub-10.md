# sub-10: 外部プロジェクトの C++ カーネル表 (エディタのスクリプトビルドへの組み込み)

- 依存: sub-05
- 状態: 未着手
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

## フィードバック履歴
