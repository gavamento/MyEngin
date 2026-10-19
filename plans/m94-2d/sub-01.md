# sub-01: 平面拘束 `Rigidbody.plane2D` と一方通行 `Collider.oneWay`、2D 不変量の selftest

- 依存: なし
- 状態: OK (commit 86dec43)
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (第 4 章 必須制約、第 5 章 規約、第 6・7 章 進め方と検証)。spec は `C:\HAL\MyEngin\plans\m94-2d\spec.md` (2. #3, #4, #9、4.1.5、4.4)

## やること
M94 で最も荷重のかかる未知 = 「3D ソルバへの平面拘束で z / X・Y 回転が**ビット単位で**動かないか」を潰す。

1. `RigidbodyComponent` 末尾に `bool plane2D = false` (hash 対象、`Components.cpp` の登録表に和名付きで追加)。`ColliderComponent` 末尾に `bool oneWay = false` (hash 対象)。`kSimSnapshotVersion` を +1 し `SimSnapshot.h` の履歴と `AcousticAudioSelfTest.cpp:129` の固定値を更新。
2. `PhysicsSystem::Update` に拘束を入れる (spec 2. #3 の A / B / D)。
   - A: ボディ収集 (`PhysicsSystem.cpp:1571-1683`) で `b.plane2D` を持ち、`invI` 確定 (:2635-2654) で plane2D なら Z 軸成分 (ワールド zz) だけ残して他を 0 にする。
   - B: 速度ソルバ (:4099-4266) の直後・位置補正パス (:4287) の前で `vz = wx = wy = 0.0f`。
   - D: 位置・姿勢積分 (:4826-4868) の直後で `pz = z0`、`qx = qy = 0.0f` (正規化は既存経路に任せる。正規化で x/y が非 0 になるなら正規化後にもう一度 0 を書く)、`vz = wx = wy = 0.0f`。CCD の差し替え (:4840) もこの後に来ることを確認。
   - z0 は tick 開始時に `LocalTransform.position.z` から取る (サブステップをまたいで同じ値)。
   - **OFF のボディでは 1 演算も増やさない** (分岐ゲート。`+ 0.0f` は -0.0f を化けさせる — M59f1 のコメント参照)。
   - スリープ (:4929-5018)、CCD、複合コライダー (M60e)、関節 (Hinge) と組み合わせて不変量が成立すること。
3. `oneWay` の接触フィルタ: 接触生成 (:3842-4096) で、静的側 (または kinematic 側) の形状が `oneWay` なら「法線 (床→物体) の y が 0 以下」または「物体の tick 開始時の最下点 (形状のワールド AABB の y 最小) < 床の天面 (oneWay 形状のワールド AABB の y 最大) − slop」なら接触を捨てる。両方 oneWay なら従来どおり (捨てない)。`isTrigger` は従来どおり接触を作らない。
4. ABI の `AddForce` / `AddImpulse` / `AddTorque` / `AddForceAtPosition` は plane2D ボディで z 成分 / X・Y トルクを捨てる (`ApplyTorqueWorld` の freezeRotation の前例 :5318)。
5. `PhysicsSelfTest.cpp` に `Plane2D` 節 (spec 4.1.5): 箔 10 + 球 5 + Hinge(axis=(0,0,1)) 1 組を plane2D で床に落として 3000 tick。(a) 全ボディの不変量 (b) 2 ワールド並走の `HashWorld` 一致 (c) 最終 `HashWorld` を定数に焼き Debug / Release で同値 (`NavDeterminismSelfTest` の kExpected の流儀) (d) oneWay 床を球が下から抜け、上から乗り、横から当たっても止まらない (e) plane2D OFF のボディだけのシーンのハッシュが着手前と同一 (既存ハッシュを着手前に 1 回採って焼く)。
6. Inspector: Rigidbody / Collider の新フィールドは登録表から自動で出る。`plane2D` の和名の説明に「異なる z の物同士は当たらない」を書く (`MYE_JP` の和名か Components.h のコメント)。

7. selftest の入口: `EditorMain.cpp:572-583` の `MYE_SELFTEST_ONLY` 分岐に `m94` を足し、M94 の suite だけ (今は `RunPhysicsSelfTest`) を回せるようにする。以降のサブは自分の suite をこの分岐に追加する (ユーザー判断 9: 重い検証はサブ 5 本に 1 回)。

## やらないこと (このサブでは)
- CharacterController の 2D (sub-09)、2D 形状・クエリ (sub-08)、ABI の新スロット (sub-14)、デモ (sub-23)

## 触る場所 (planner の見立て)
- `src\Engine\Core\Ecs\Components.h` (Rigidbody / Collider の末尾)、`Components.cpp` (登録表)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (:1571-1683 収集、:2635-2654 invI、:4266 付近、:4826-4868 積分、:3842-4096 接触生成、:5318 / :5357 ABI の力)、`PhysicsSystem.h` (Body 構造体)
- `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp`
- `src\Engine\Engine\Replay\SimSnapshot.h` (版)、`src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp:129`
- 参考: `src\Engine\Engine\Navigation\NavDeterminismSelfTest.cpp` (焼いたハッシュの流儀)、`Components.h:381-421` の Rigidbody コメント (M59f1 / M59h の規約)

## 受け入れ条件 (このサブ)
1. (spec 6) `Plane2D` 節の (a)(b)(c) が Debug / Release で PASS。焼いたハッシュの値を SELF_EVAL に書く。
2. (spec 7) (d) が PASS。
3. (spec 8) `--physics-demo` と `--joint-demo` を着手前後で `--replay-record` → `--hash-diff` し、差分が `plane2D` / `oneWay` の append だけ (ログを SELF_EVAL に)。
4. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` で Physics suite が Debug / Release で PASS、版 bump は snapshot +1 だけ。**全体 selftest / replay_verify / golden 30 枚は sub-05 のチェックポイントで回す** (ユーザー判断 9。未実行は REWORK の理由にしない)。
5. 不変量が割れた場合は spec 7. R1 の順で拘束点を増やし、どこまで要ったかを SELF_EVAL と ADR 下書き (sub-24 が仕上げる) に書く。

## 検証コマンド
毎サブ必須 (ユーザー判断 9):
- `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` と Release
- `$env:MYE_SELFTEST_ONLY='m94'; bin\x64\Debug\Editor.exe --selftest` と Release (Plane2D 節のハッシュ行を両方から抜く。PowerShell ツールから)
- `pwsh tools\check_rules.ps1`
- `bin\x64\Release\Runtime.exe --physics-demo --replay-record cache\before.rep ...` → 着手後 → `--hash-diff` (`--joint-demo` も)
チェックポイントではない (全体 `--selftest` / `tools\replay_verify.bat` / `tools\shot_verify.bat` は sub-05 で)。

## 実装メモ (coder が追記)

round 1 (SELF_EVAL 要約): 拘束 A/B/D の 3 点だけで不変量が全 tick・全 17 体で成立 (R1 の追加拘束は不要)。
焼いた Plane2D 最終 HashWorld = 639107476EA401A3 (Debug == Release)。oneWay は spec の「最下点 < 天面 - slop」だけだと
速く着地した物が床を素通りしたため、着地速度 (相対 vy < -0.5) なら深さを問わず受ける条件を追加 (kOneWayLandingSpeed)。
着手前デモとの hash-diff は plane2D / oneWay の append のみ (UIInteraction.prevSurfX/Y は UI 表示サイズ依存で sim 外)。
NavAgentSelfTest の kExpectedYardHash を再採取 (CCF96006900E9C0A)、TagSelfTest の shadowing 警告を改名で解消。

### round 1 SELF_EVAL (全文、司会が転記)

```
SELF_EVAL: sub-01 (round 1)
実装:
  - Components.h/.cpp: `RigidbodyComponent.plane2D` と `ColliderComponent.oneWay` を末尾 append し、登録表に和名「2D 平面拘束」「一方通行」を追加。「異なる z の物同士は当たらない」は Components.h のコメントに書いた。
  - SimSnapshot.h: kSimSnapshotVersion を 50 から 51 にし、履歴 v51 を追記。AcousticAudioSelfTest.cpp:129 を 51 に更新。
  - PhysicsSystem.cpp の拘束 A/B/D:
    - A: `Body.plane2D/z0` を収集時に設定。kinematic は除外。入口で vz/wx/wy を 0 にし、姿勢の x/y が非 0 なら捨てて z/w を再正規化する。`RestrictInertiaToZ` で invI を zz だけ残す。呼び出しは慣性確定ループと wakeBody の 2 か所。
    - B: 速度ソルバの直後・位置補正の前に vz=wx=wy=0.0f を代入。
    - D: 位置・姿勢積分ループの最後 (CCD 差し替えと質量中心の逆算の後) に pz=z0、qx=qy=0.0f、vz=wx=wy=0.0f を代入。
    - 全部 `anyPlane2D` と `b.plane2D` の分岐ゲート。OFF のボディは演算が 1 つも増えない。
  - PhysicsSystem.cpp の oneWay: `Body.oneWay/startMinY/startVy`、`RejectOneWayContact`、`kOneWaySlop=0.05`、`kOneWayLandingSpeed=0.5` を追加。フィルタは接触生成、位置補正パス、睡眠ペアの報告、CCD 掃引の 4 か所に入れた。位置補正と CCD は spec の記述に無いが、入れないと下から抜ける物を押し戻した。
  - PhysicsSystem.cpp の ABI: `ApplyTorqueWorld` と `ApplyForceAtWorldPoint` は plane2D で z 力と X/Y トルクを捨てる。EngineApiTable.cpp の AddForce/AddImpulse は plane2D で z 成分を捨てる。
  - PhysicsSelfTest.cpp: Plane2D 節を追加。内容は箔 10、球 5、Hinge(axis Z) の振り子 2 体、床と壁、substeps 4 とスリープ有効の env。箔の中に初期傾き、初期の wx/wy/vz、ccd、centerOfMass、gyroscopic、z 違いを混ぜた。3000 tick の毎 tick で全 17 体の不変量を生ビット比較し、2 ワールド並走の HashWorld、焼いたハッシュ、oneWay の 4 ケース (通常と 2D の 2 モード)、OFF stacking の FNV を見る。
  - EditorMain.cpp: `MYE_SELFTEST_ONLY=m94` 分岐を追加 (今は RunPhysicsSelfTest のみ)。
  - NavAgentSelfTest.cpp: kExpectedYardHash を再採取。
  - TagSelfTest.cpp: 内側の `root` を `settingsRoot` に改名。
仕様との差分:
  - [逸脱] oneWay の判定: spec/sub の「最下点 < 天面 − slop なら捨てる」だけだと、速く落ちた球が床を素通りした。着地した tick の貫通は 1 tick の降下量 (5.7 m/s で約 0.067) まで深くなり、次 tick 開始時に slop 0.05 を超える。そのため、深さが slop を超えていても相対 vy が -0.5 m/s より速く降下中なら接触を受けるようにした (kOneWayLandingSpeed)。再現した条件は「y=2 から静止落下した球」で、修正後は 0.350 に静止する。下から抜ける・横から抜けるは維持される。
  - [追加] oneWay フィルタを位置補正パス・睡眠ペア報告・CCD 掃引にも適用した (理由は上の実装欄)。
  - [追加] 複合コライダーの子形状の oneWay は未対応。ボディ自身のコライダーの oneWay だけを見る。
  - [追加] NavAgentSelfTest.cpp の kExpectedYardHash を 0x0964E3D5948168AA から 0xCCF96006900E9C0A に再採取。Rigidbody と Collider のハッシュ欄が 2 つ増えただけで、シムの挙動は plane2D OFF の bit 一致検査 (e) で確認済み。この 1 本だけが割れていた。
  - [追加] TagSelfTest.cpp:304 の C4456 (`root` の隠蔽) は着手前の HEAD から存在した。MyeWarnAsError=true だとこの警告で落ちるため、局所変数を改名して解消した。
  - [追加] sub 更新後の項目 7 (`MYE_SELFTEST_ONLY=m94`) を実装した。
  - [未実装] 親付き (frame が identity でない) plane2D の LocalTransform.z 保証。z0 はワールド z で、書き戻しは親フレームの逆変換を通る。ルート剛体は bit 一致を検証済みで、親付きは未検証。
検証:
  - Debug / Release の `MSBuild MyEngine.sln /p:MyeWarnAsError=true` → 両方 exit 0、警告 0。ただし変更後の最初の 1 回は毎回 `SelfTestUnused.kernel.hlsl` の C4100 で落ち、同じコマンドの再実行で通る。着手前の HEAD でも同様 (初回 exit 1、2 回目 exit 0)。着手前から存在するビルド癖で、今回の変更とは無関係に見える。
  - `MYE_SELFTEST_ONLY=m94 Editor.exe --selftest` (Debug / Release) → 両方 exit 0、"Physics self test: ALL PASS"、plane2D/oneWay の PASS 16 行。
    - 焼いた Plane2D 最終 HashWorld @3000 = 639107476EA401A3 (Debug == Release、定数 kPlane2DExpectedHash に焼いた)。
    - OFF stacking の FNV = 14C1DF5F2242A072。着手前の `[phys] body` 3 行 (位置/速度ビット) から別計算した値と一致する (e)。
    - oneWay: 下から球は maxY 2.071 まで抜けて 0.350 に静止、上から落とした球は 0.350 に静止、横から (重力なし、4 m/s) は x=21.667 まで vx=4.000 のまま通過、通常の床では下から止まる (対照、maxY -0.298)。通常剛体と plane2D の両方で同値。
  - 不変量は R1 の追加拘束なしで成立した。A+B+D の 3 点だけで、3000 tick の毎 tick・全 17 体 (CCD、質量中心、ジャイロ、初期傾き、Hinge 振り子、スリープ有効、substeps 4 込み) で z/vz/wx/wy/qx/qy がビット一致した。割れなかったので「どの tick・どのボディで割れたか」の観測は不要。
  - Release の全体 `--selftest` (変更途中の別バイナリで実施) → exit 1。確認できた FAIL は上記 kExpectedYardHash のみで、再採取後は Debug/Release とも全体 exit 0。着手前の Release 全体 selftest は Fracture editor の shutdown タイミング FAIL で exit 1 だった (今回の変更とは無関係。後の 2 回は PASS)。
  - hash-diff (`Runtime.exe --physics-demo` / `--joint-demo`、Release、600 tick、dump tick 599、着手前後):
    - `--hash-diff` はフィールド追加で構造が変わるため line 86 で止まる ("0 field(s) differ"、行数差 physics 748→774、joint 3746→3846)。
    - 別途 (entity, comp, field) キーで値を突き合わせた結果、追加は Collider.oneWay と Rigidbody.plane2D のみ (physics 26 件、joint 100 件)、削除 0 件。
    - 値が変わったのは UIInteraction.prevSurfX/Y の 2 件だけで、UI 表示サイズ依存の sim 外の値に見える。sim 値の変化は 0。
  - `pwsh tools\check_rules.ps1` → 3 error (全て変更外)。EditorMain.cpp:741 の `#ifdef _DEBUG` (rule 1)、tools/engine_dist/Editor.bat と Setup.bat の CRLF (rule 14-b) で、いずれも触っていないファイルの既存分。warning 50 件も既存。0 にはなっていない。
  - 未実行 (ユーザー判断 9 で sub-05 へ): tools\replay_verify.bat、tools\shot_verify.bat と golden 30 枚の照合、全体 selftest の最終確認。
自己採点 (1-5):
  仕様適合: 4 — 項目 1〜7 は実装した。oneWay の判定条件は [逸脱] として追加した。check_rules が 0 にならない点は既存分。
  正しさ: 4 — 不変量はビット一致、Debug/Release で同値、OFF は bit 同一を確認した。親付き plane2D と複合子の oneWay は未検証・未対応。
  コード品質: 4 — 分岐ゲートと単機能の関数に整理し、コメントは日本語で短くした。Body に 4 フィールド増えた。
  テスト: 4 — 毎 tick の生ビット検査、並走ハッシュ、焼いた定数、oneWay 4 ケースの 2 モード、OFF の bit 同一を入れた。replay_verify と golden は未実行。
不安・質問:
  - kOneWayLandingSpeed=0.5 と kOneWaySlop=0.05 は私の選んだ値。planner が spec の条件式を正としたい場合は差し戻して指示してほしい。ただしその場合、速く落ちた球は床を素通りする。
  - Q-1 (z0 保持) は実装済み。z がずれた plane2D 同士が当たらない挙動はテストしていない (z が接触距離内なら Z 成分の法線を持つ接触になるが、D が z を戻す)。
  - FractureSystem.cpp:188 は破片へ freezeRotation をコピーするが plane2D はコピーしない。2D の破壊物は sub-11 以降で扱う想定で、今回は触っていない。
触ったファイル:
  - src\Engine\Core\Ecs\Components.h / Components.cpp
  - src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp / PhysicsSelfTest.cpp
  - src\Engine\Engine\Replay\SimSnapshot.h
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp
  - src\Engine\Engine\Script\EngineApiTable.cpp
  - src\Engine\Engine\Navigation\NavAgentSelfTest.cpp
  - src\Engine\Engine\Scene\TagSelfTest.cpp
  - src\Editor\App\EditorMain.cpp
  - plans\m94-2d\sub-01.md (実装メモ節のみ)
申し送り:
  - ADR 下書き (sub-24 向け): 平面拘束は慣性の zz 限定 (A)、速度ソルバ直後の代入 (B)、積分直後の代入 (D) の 3 点で足りた。R1 の毎反復射影や translateBody の z 破棄は不要。
  - 次サブ向け: `MYE_SELFTEST_ONLY=m94` 分岐は EditorMain.cpp にある。自分の suite をそこへ足す。
  - hash-diff の元ファイルは cache\m94\ (before_*.dump / after_*.dump、生成物なのでステージしない)。
  - ビルド癖: 変更後の初回ビルドが `SelfTestUnused.kernel.hlsl` の C4100 で落ちる。同じコマンドの再実行で通る。着手前の HEAD でも再現した。
  - 既存の check_rules の 3 error は着手前から存在する。解消するなら別件。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。核心 (Plane2D 節 (a)〜(d)、2 ワールド並走、焼いたハッシュ 639107476EA401A3 の Debug == Release、OFF の bit 同一、hash-diff が append のみ) を根拠欄で確認。`oneWay` の速度条件 (kOneWayLandingSpeed) は逸脱として採用し spec 4.1.5 に反映。should: Fracture の plane2D コピーは sub-08 へ (spec R9 (c))。nit: 実装メモ節は SELF_EVAL の写し (全文) を置くこと。check_rules の 3 error (EditorMain.cpp:741 の `#ifdef _DEBUG`、tools/engine_dist/*.bat の LF) は HEAD を読んで既存と確認 — M94 の範囲外、司会へ別件として申し送り。
