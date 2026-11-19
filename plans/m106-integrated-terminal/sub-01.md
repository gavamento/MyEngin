# sub-01: libvterm 同梱と VT 機能充足の確認

- 依存: なし
- 状態: 未着手
- 往復: 0

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
