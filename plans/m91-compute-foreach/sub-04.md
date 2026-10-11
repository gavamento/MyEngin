# sub-04: State バッファのスナップショット・ハッシュ・寿命

- 依存: sub-03
- 状態: 未着手
- 往復: 0
- 再編: 2026-10-12 (旧 sub-03。中身は同じ。U3 はユーザー確認済み)

## やること
spec D7 / D8、4.2 (SimSnapshot・WorldHash)。

1. **GameLogic 用ストア**の State 部を、SimSnapshot の新節「ComputeState」に保存・復元する (スロット表 = 世代・使用中・所有エンティティ・stride・count + 中身)。
   `kSimSnapshotVersion` を着手時点の次の版に上げる (今なら 48→49)。版を固定値で見ているテスト (`AcousticAudioSelfTest.cpp:129` など) も直す。
2. `HashSources` に ComputeState を足す (使用中のスロットを昇順に)。`HashWorldDetailed` と `--hash-diff` にも出す。
3. 所有: 作成したエンティティの破棄 (tick 末) で解放する。Play 停止とシーン読込では全部解放する (`ReleasePlaySessionEngineState`、`PlayModeController.cpp:99`)。C# 用のストアも Play 停止で解放する。
4. Fx と C# 用のストアは、スナップショットにもハッシュにも入れない。
4b. (sub-03 から移管) **HeadlessSim の経路に ComputeService を配線する** (所有・Bind、tick 頭の BeginTick(resim)、SimRefs。device-less なので GPU は無し)。Server.exe (`ServerLoop.cpp` は HeadlessSim を使う) と headless の replay 検証で、State の ForEach が EngineLoop と同じ結果になること。
5. 復元した後、FIELD のハンドルが同じバッファを指すことを selftest で確かめる。

## やらないこと (このサブでは)
- ABI (sub-05)

## 触る場所 (planner の見立て)
- `src/Engine/Engine/Replay/SimSnapshot.{h,cpp}`、`HashSources` の定義、エンティティを破棄する処理、`PlayModeController.cpp:99`
- `HeadlessSim` / `EngineLoop` で SimRefs を組み立てる箇所 (Server / Runtime / Editor のどれでも同じ参照が入るように)

## 受け入れ条件 (このサブ)
1. spec 5-5 (selftest)
2. `tools\replay_verify.bat` の snapshot round-trip と既存のシーンが PASS する
3. HeadlessSim に ComputeService があり、デバイスが無くても State の ForEach が動くこと (selftest または headless で確認)

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
