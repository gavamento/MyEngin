# sub-05: VS 開発者環境とエンジン環境変数

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## やること
spec 2・4.2。
- `/home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.cpp:238-290` の匿名 namespace の vswhere 探索を名前付き関数に昇格 (既存呼び出しの挙動は不変)。
- VS 開発者環境: `cmd /s /c ""<VsDevCmd.bat>" -arch=x64 -no_logo && set"` を非表示で実行し出力を解析、親環境との差分をキャッシュ (エディタ起動中1回。読み込み完了まで窓に「読み込み中」を表示して待ち、完了後にシェルを起動 [2026-11-19 ユーザー確定]。タイムアウト 30 秒で素の環境にフォールバック、警告表示)。出力解析は Win32 非依存の純関数にして g++ テスト。
- エンジン環境変数: MYE_PROJECT_ROOT / MYE_TERMINAL_ID / TERM_PROGRAM / COLORTERM / PATH 前置 `<root>\tools` を sub-02 の環境組立に渡す。
- 設定 terminalLoadVsDevEnv=false で読み込まない。

## やらないこと (このサブでは)
- AI 追跡側の読み取り。

## 受け入れ条件 (このサブ)
1. spec 5-8。
2. Linux: `set` 出力解析 (= を含む値、空値、`=C:` 系の隠し変数、Unicode) と差分計算の g++ テスト成功。
3. VisualStudioLauncher の既存挙動不変 (差分レビューで関数移動のみであること)。
4. Windows (要確認): `where cl`、`msbuild -version`、`$env:MYE_TERMINAL_ID` が窓内で通る。VS 未導入擬似 (vswhere パスを壊す) で警告+起動。

## 検証コマンド
- Linux: g++。Windows: ビルド、手動。
