# sub-01: libvterm 同梱と VT 機能充足の確認

- 依存: なし
- 状態: 判定待ち → OK (コミット待ち)
- 往復: 1

## やること
- spec 2・7。libvterm 0.3.3 のソース (include/, src/ の .c/.h/.inc, LICENSE) を `/home/user/MyEngin/external/libvterm/` に改変なしで置く。取得物は `/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/libvterm-probe/libvterm-0.3.3/` (Ubuntu ミラー経由、sha256 は spec 7)。bin/ t/ Makefile は入れない。
- 手書き vcxproj (静的ライブラリ、C としてコンパイル、/fp:precise、警告は外部ライブラリ扱い) を作り MyEngine.sln に追加。Editor から参照 (まだ使わないが include パスとリンクを通す)。既存 external の vcxproj (例: miniz / libtess2) を手本にする。
- `/home/user/MyEngin/external/VERSIONS.md` に行を追加。
- VT 機能スモーク: Linux で gcc -std=c99 によりコンパイルし、1049(代替画面)/2004(bracketed paste)/38;2(truecolor)/全角幅2/OSC 8 無害/未知 DECSET 2026 無害/カーソル移動・消去 (CUP/ED/EL)/スクロール領域 (DECSTBM) を確認するスモークプログラムをスクラッチパッドで実行 (リポジトリには入れない。恒久テストは sub-02)。

## やらないこと (このサブでは)
- ラッパ・UI・ConPTY。

## 触る場所 (planner の見立て)
- 新規 `/home/user/MyEngin/external/libvterm/`、`/home/user/MyEngin/build/` 配下の新 vcxproj、`/home/user/MyEngin/MyEngine.sln`、Editor の vcxproj (参照追加)、`/home/user/MyEngin/external/VERSIONS.md`。gen_project_files.ps1 が external を手書き扱いすること (`/home/user/MyEngin/tools/gen_project_files.ps1:100`) を確認。

## 受け入れ条件 (このサブ)
1. 同梱ファイルが取得物と完全一致 (diff -r で差分なし、除外したのは bin/ t/ Makefile vterm.pc.in のみ)。
2. Linux: 同梱物を gcc -std=c99 -Wall で全 .c コンパイル警告 0、スモーク全項目期待どおり。
3. vcxproj が XML として整形式で、sln の ProjectGuid・構成 (Debug/Release x64) と整合、Editor の ProjectReference が新 GUID を指す (python -I で XML パースして検査)。
4. Windows (要確認): Debug|x64 / Release|x64 ビルド成功。

## 検証コマンド
- Linux: `diff -r`、gcc コンパイル、スモーク実行、XML/GUID 整合スクリプト。
- Windows: VS2022 でソリューションビルド。

## 実装メモ (coder が追記)

### round 1

SELF_EVAL: sub-01 (round 1)
実装:
  - /home/user/MyEngin/external/libvterm/ — libvterm 0.3.3 の include/ (vterm.h, vterm_keycodes.h)、src/ (.c 9 本、.h 3 本、fullwidth.inc、encoding/DECdrawing.inc・uk.inc)、LICENSE、CONTRIBUTING を取得物から無改変コピー (計 19 ファイル)
  - /home/user/MyEngin/build/libvterm.vcxproj (新規、GUID {1120A41F-3F5C-4370-A92C-4C3909DCFC62}) — C の静的ライブラリ。Common.props を Import、CompileAsC + /std:c11、Level3・TreatWarningAsError=false
  - /home/user/MyEngin/MyEngine.sln — libvterm プロジェクトと Debug|x64 / Release|x64 の ActiveCfg・Build.0 を追加
  - /home/user/MyEngin/build/Editor.vcxproj — ProjectReference (libvterm.vcxproj) と ClCompile の AdditionalIncludeDirectories に external\libvterm\include を追加 (BEGIN/END FILES の自動生成区画の外)
  - /home/user/MyEngin/external/VERSIONS.md — libvterm 行を追加 (版・同梱範囲・取得元・sha256・MIT)
技術解説:
  - 技術: libvterm — VT100/xterm 系の端末エミュレータを「パーサ (parser.c) → 状態機械 (state.c: カーソル・モード・SGR・スクロール領域) → 画面モデル (screen.c: セル配列・代替画面・スクロールバック通知)」の 3 層で持つ C99 ライブラリ。I/O を持たず、vterm_input_write でバイト列を食わせ、vterm_screen_get_cell でセルを読み、キー入力は vterm_keyboard_* が出力バッファへ VT 列を積む (vterm_output_read で取り出す)。ConPTY の出力を食わせて ImGui で描く構成 (spec 4.1) にそのまま合う。VT 解釈の自作 (spec 2 で却下) より網羅性が高い。
  - 技術: 別プロジェクトの静的 lib 化 — 既存の external (miniz, libtess2 等) は Engine.vcxproj にファイル単位で直書きされ Engine に混ぜてあるが、sub 仕様の指定どおり独立した libvterm.vcxproj にした。Editor だけが使うので Engine に混ぜると Runtime/Server まで巻き込むため、独立 lib + Editor からの ProjectReference が依存範囲を最小にする。MSBuild は ProjectReference 先の StaticLibrary を LinkLibraryDependencies (既定 true) で自動リンクするので AdditionalDependencies は書いていない。
  - コード: /home/user/MyEngin/build/libvterm.vcxproj — 要の設定は次のとおり。Common.props を Import するので /fp:precise・/MT(d)・OutDir (bin\x64\$(Configuration)\) は他プロジェクトと一致する。Common.props の LanguageStandard (stdcpp20) は C++ にしか効かず、C 側は LanguageStandard_C で別指定。
    ```xml
    <ConfigurationType>StaticLibrary</ConfigurationType>
    ...
    <CompileAs>CompileAsC</CompileAs>
    <LanguageStandard_C>stdc11</LanguageStandard_C>
    <WarningLevel>Level3</WarningLevel>
    <TreatWarningAsError>false</TreatWarningAsError>
    <AdditionalIncludeDirectories>$(RepoRoot)external\libvterm\include;$(RepoRoot)external\libvterm\src;...
    ```
    src を include パスに入れるのは encoding.c が "encoding/DECdrawing.inc" を、unicode.c が "fullwidth.inc" を相対 include するため (同一ディレクトリなので実際は不要だが、明示しておく)。
  - コード: スモーク (/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub01/smoke2.c、リポジトリ外) — vterm_new(24,80) → vterm_set_utf8 → vterm_obtain_screen → settermprop / sb_pushline コールバック登録 → enable_altscreen → reset。各 VT 列を input_write してセル・コールバックで判定する 12 項目。DECSTBM は「2..4 行目を領域にして 4 行目で LF → 領域内だけ上へずれ、1 行目と 5 行目は不変、sb_pushline は呼ばれない」、全画面スクロールでは sb_pushline が 1 行ごとに呼ばれることを確認 (スクロールバック実装 sub-02 の前提)。
  - 選ばなかった案: (1) Engine.vcxproj に .c を直書き (既存流儀) — Runtime/Server に不要なコードが入るため不採用。(2) Makefile / libtool を回して生成物を作る — 指示によりアーカイブ内ビルドツールは実行せず、必要な .inc はアーカイブに同梱済みであることを確認して使った (encoding/*.inc, fullwidth.inc)。(3) TurnOffAllWarnings — 既存 external と揃えて Level3 にした。
  - 前提・落とし穴: libvterm は非スレッドセーフ。spec 4.1 のとおり UI スレッドだけが触ること。DECSET 2026 (同期出力) は未対応で無視される (スモークで無害を確認)。libvterm のソースは LF のまま (.gitattributes の text=auto eol=lf に一致)、vcxproj/sln は CRLF で書いた (規則 14)。MSVC の C コンパイラでの通過は未確認 — gcc -Wvla で VLA 不使用は確認したが、MSVC 固有の警告 (C4244 等) は出ても Level3/非エラー扱いで止まらない想定。ProjectReference による自動リンクは Editor のみ。将来 Runtime 等から使うなら同様に参照を足す。
仕様との差分:
  - [追加] CONTRIBUTING も同梱した — 受け入れ条件 1 の「除外は bin/ t/ Makefile vterm.pc.in のみ」に従った (やること節の列挙には無い)
  - [追加] libvterm.vcxproj.filters は作っていない — 必須でなく、gen_project_files.ps1 の対象でもないため。必要なら指摘を
  - [追加] VERSIONS.md の出所欄に上流ホームページ (leonerd.org.uk) と実際の取得 URL を併記。上流 sha256 未照合と明記
検証:
  - diff -r <取得物> /home/user/MyEngin/external/libvterm → 差分は Only in: Makefile, bin, t, vterm.pc.in のみ
  - gcc -std=c99 -Wall -c src/*.c (9 本) → 警告 0・エラー 0。gcc -std=c99 -Wvla -fsyntax-only → 出力なし (VLA 不使用)
  - smoke2 (同梱物から作った libvterm.a にリンク) → 1049 on/復帰、2004 貼付括り、OSC8+DECSET2026 無害、38;2 truecolor、全角 width=2、CUP、EL、ED、DECSTBM 領域スクロール、DECSTBM でスクロールバックに積まない、全画面スクロールで sb_pushline — 12/12 OK (FAIL=0)
  - python3 -I check_proj.py (/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub01/check_proj.py) → 3 ファイルの XML 整形式、構成 Debug|x64/Release|x64、GUID が sln と Editor の ProjectReference で一致、sln の 4 構成行、ClCompile が src/*.c と一致 → OK
  - git check-ignore → 同梱 19 ファイルはどれも無視されない
  - Windows: VS2022 Debug|x64 / Release|x64 ソリューションビルド → 未実行 (Linux コンテナで MSVC 不可)
  - tools\check_rules.ps1 → 未実行 (PowerShell/Windows 前提)。規則 4 (/fp:fast)・13-c/13-d・14 (CRLF) は目視で抵触なしを確認
自己採点 (1-5):
  仕様適合: 4 — やること全項目を実施、差分は [追加] に明記。受け入れ条件 4 は未確認
  正しさ: 3 — Linux 側は検証済みだが MSVC ビルドが未実行
  コード品質: 4 — 既存 vcxproj の書式・Common.props の契約に合わせ、自動生成区画を避けた
  テスト: 4 — 指定スモークに DECSTBM/スクロールバックまで足して全通過。恒久テストは sub-02 範囲
不安・質問:
  - MSVC の C コンパイラで libvterm がそのまま通るか (spec 7 のリスク)。Windows で受け入れ条件 4 を確認してほしい。通らなければパッチ方針 (PATCHES.md) を planner が決める必要がある
  - .filters を作るべきか
触ったファイル:
  - /home/user/MyEngin/external/libvterm/ (19 ファイル)
  - /home/user/MyEngin/build/libvterm.vcxproj
  - /home/user/MyEngin/MyEngine.sln
  - /home/user/MyEngin/build/Editor.vcxproj
  - /home/user/MyEngin/external/VERSIONS.md
  - /home/user/MyEngin/plans/m106-integrated-terminal/sub-01.md (実装メモ節)
申し送り:
  - sub-02: include は <vterm.h>。Editor のみ include パス・リンク済み。sb_pushline は全画面スクロール時のみ、DECSTBM 領域内スクロールでは呼ばれない (スモークで確認)。スモークのソースは /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub01/smoke2.c (恒久テスト化の材料)
  - 上流 tarball の sha256 照合は Windows 側 / ネット到達可能な環境で要実施
- round 1: VERDICT OK (planner)。diff -rq で除外 4 件のみ・sln/Editor 参照・VERSIONS.md を実ファイルで確認。CONTRIBUTING 同梱と filters 無しは承認。受け入れ条件4 (MSVC ビルド) と上流 sha256 照合は Windows 側の未検証として申し送り。MSVC で通らない場合は最小パッチ + `external/libvterm/PATCHES.md` (recastnavigation の流儀) で対応する。
