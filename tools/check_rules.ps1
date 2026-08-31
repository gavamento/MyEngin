# check_rules.ps1 — コーディング規則の静的検査 (engine_spec.md 11.2)
# 実行: pwsh -File tools\check_rules.ps1  (違反があれば exit 1)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$srcDirs = @("$repo\src")
$errors = 0
$warnings = 0

function Get-Sources {
    Get-ChildItem -Recurse -File $srcDirs | Where-Object { $_.Extension -in '.cpp', '.h', '.hpp' }
}

Write-Host "=== MyEngine coding rule check (spec 11.2) ==="

# C++ ソースの行から // コメント部を除去して検査する (コメント内の言及は許容)
function Test-CodeLines([System.IO.FileInfo]$file, [string]$pattern, [string]$rule, [string]$msg) {
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
        $lineNo++
        $code = ($line -split '//', 2)[0]
        if ($code -match $pattern) {
            Write-Host "ERROR [$rule] $($file.FullName):${lineNo}: $msg"
            $script:errors++
        }
    }
}

# 規則 8: エンジン RNG 以外の乱数禁止
foreach ($f in Get-Sources) {
    Test-CodeLines $f '\brand\s*\(\s*\)|\bsrand\s*\(|std::random_device|std::mt19937' 'rule 8' 'use engine Pcg32 instead'
}

# 規則 1: ロジックの #ifdef _DEBUG 分岐禁止 (許可リスト: 診断出力のみのファイル)
$debugWhitelist = @('GraphicsDevice.cpp')
foreach ($f in Get-Sources) {
    if ($debugWhitelist -contains $f.Name) { continue }
    $hits = Select-String -Path $f.FullName -Pattern '#\s*if(def)?\s+.*\b(_DEBUG|NDEBUG)\b'
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 1] $($h.Path):$($h.LineNumber): $($h.Line.Trim())"
        $script:errors++
    }
}

# 規則 2: assert 内の副作用 (CRT assert 自体を禁止 — MYE_CHECK を使う)
foreach ($f in Get-Sources) {
    $hits = Select-String -Path $f.FullName -Pattern '^\s*assert\s*\('
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 2] $($h.Path):$($h.LineNumber): use MYE_CHECK instead of assert"
        $script:errors++
    }
}

# 規則 4: /fp:fast 禁止 (ビルド設定 — 実際の設定値のみ検査、XML コメントは無視)
$buildFiles = Get-ChildItem -Recurse -File "$repo\build" | Where-Object { $_.Extension -in '.vcxproj', '.props' }
foreach ($f in $buildFiles) {
    $hits = Select-String -Path $f.FullName -Pattern '<FloatingPointModel>Fast|<AdditionalOptions>[^<]*fp:fast'
    foreach ($h in $hits) {
        Write-Host "ERROR [rule 4] $($h.Path):$($h.LineNumber): /fp:fast is forbidden"
        $script:errors++
    }
}

# 規則 7 (参考警告): unordered コンテナの range-for (順序がロジックに影響しないか要確認)
foreach ($f in Get-Sources) {
    $hits = Select-String -Path $f.FullName -Pattern 'for\s*\(.*:\s*\w*unordered_(map|set)'
    foreach ($h in $hits) {
        Write-Host "WARN  [rule 7] $($h.Path):$($h.LineNumber): unordered iteration (verify order-independence)"
        $script:warnings++
    }
}

# 規則 9: C++ と HLSL で共有する定数の一致
# (食い違うと定数バッファ不一致やトラバーサル破綻として静かに壊れる)
$constGroups = @(
    @{
        label = 'kMaxBones / MYE_MAX_BONES'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'            = 'constexpr\s+int\s+kMaxBones\s*=\s*(\d+)'
            'assets\shaders\forward_skinned.hlsl'          = '#\s*define\s+MYE_MAX_BONES\s+(\d+)'
            'assets\shaders\deferred_gbuffer_skinned.hlsl' = '#\s*define\s+MYE_MAX_BONES\s+(\d+)'
        }
    },
    @{
        label = 'kEmissiveMaxIntensity / MYE_EMISSIVE_MAX'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kEmissiveMaxIntensity\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'       = '#\s*define\s+MYE_EMISSIVE_MAX\s+(\d+)'
        }
    },
    @{
        label = 'kRtStackDepth / MYE_RT_STACK_DEPTH'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtStackDepth\s*=\s*(\d+)'
            'assets\shaders\rt_common.hlsli'           = '#\s*define\s+MYE_RT_STACK_DEPTH\s+(\d+)'
        }
    },
    @{
        label = 'kRtMaxVisit / MYE_RT_MAX_VISIT'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtMaxVisit\s*=\s*(\d+)'
            'assets\shaders\rt_common.hlsli'           = '#\s*define\s+MYE_RT_MAX_VISIT\s+(\d+)'
        }
    },
    @{
        label = 'kRtTemporalMaxHistory / MYE_RT_TEMPORAL_MAX_HISTORY'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtTemporalMaxHistory\s*=\s*(\d+)'
            'assets\shaders\rt_temporal.cs.hlsl'       = '#\s*define\s+MYE_RT_TEMPORAL_MAX_HISTORY\s+(\d+)'
        }
    },
    @{
        label = 'kRtAtrousRadius / MYE_RT_ATROUS_RADIUS'
        sites = @{
            'src\Engine\Renderer\RayTracing\RtTypes.h' = 'constexpr\s+int\s+kRtAtrousRadius\s*=\s*(\d+)'
            'assets\shaders\rt_atrous.cs.hlsl'         = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
            'assets\shaders\rt_variance.cs.hlsl'       = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
            'assets\shaders\rt_shadow_filter.cs.hlsl'  = '#\s*define\s+MYE_RT_ATROUS_RADIUS\s+(\d+)'
        }
    },
    # M55a: ここから下の 2 件は M17/M38d 以来ずっと未登録だった穴の回収。
    # ライト配列長は CB のレイアウトそのものなので、食い違うと「静かに壊れる」の典型
    # (RtPasses.cpp:40 の static_assert は C++ 内の 2 者しか見ていない)
    @{
        label = 'kMaxLights / MAX_LIGHTS / MYE_RT_MAX_LIGHTS'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kMaxLights\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'       = '#\s*define\s+MAX_LIGHTS\s+(\d+)'
            'assets\shaders\rt_common.hlsli'    = '#\s*define\s+MYE_RT_MAX_LIGHTS\s+(\d+)'
        }
    },
    @{
        # HLSL 側は #define ではなくローカル配列の宣言 (SampleShadowCSM の vps[3]) が
        # カスケード数の正体なので、配列長そのものを拾う。
        # ★限界: SampleShadowCSM は vp0/vp1/vp2 を個別引数で受けるので、カスケードを
        #   4 本にするときは引数の本数も増える。$constGroups は「1 ファイル 1 整数」しか
        #   比べられないのでそこまでは検査できない — 配列長の食い違い (= 一番静かに
        #   壊れる形) だけを止める。引数側は SampleShadowCSM のコメントで注意喚起する
        label = 'ShadowPass::kCascades / SampleShadowCSM vps[]'
        sites = @{
            'src\Engine\Renderer\ShadowPass.h' = 'static\s+constexpr\s+int\s+kCascades\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'      = 'float4x4\s+vps\[(\d+)\]'
        }
    },
    @{
        # M54c: シャドウアトラスのタイル数 = 定数バッファ内の配列長そのもの。
        # 食い違うと ShadowTile 配列の後ろが読めなくなる (= 静かに壊れる) 典型
        label = 'kMaxShadowTiles / MYE_MAX_SHADOW_TILES'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kMaxShadowTiles\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'       = '#\s*define\s+MYE_MAX_SHADOW_TILES\s+(\d+)'
        }
    },
    @{
        # M58c: 地形の CB スロット。地形パスは b0 (ホストパスの PerFrame) をそのまま読み、
        # 自分の値は b1-b3 を避けて b4 に置く (b1-b3 を張り替えると後段の透明描画が壊れる)。
        # C++ 側の定数と HLSL の register(b4) が食い違うと **CB が丸ごと 0 のまま描かれる** —
        # 地形が真っ黒になるだけでコンパイルも実行も通るので、機械照合が唯一の防波堤
        # ★M58d で cbuffer の宣言は terrain_common.hlsli へ移った (deferred / forward の
        #   2 本が同じ地表を出すための共有点。宣言が 1 箇所になったので照合先も 1 本)
        label = 'kTerrainObjectCbSlot / register(b4)'
        sites = @{
            'src\Engine\Renderer\TerrainPass.h'      = 'constexpr\s+uint32_t\s+kTerrainObjectCbSlot\s*=\s*(\d+)'
            'assets\shaders\terrain_common.hlsli'    = 'cbuffer\s+TerrainObject\s*:\s*register\(b(\d+)\)'
        }
    },
    # M58d: 地形パス専用の SRV スロット (t20 以降)。ホストのスロットとも他マイルストーンの
    # 予約席とも隣り合わない位置に逃がしてある。食い違うと**地形だけが真っ黒**になるが
    # コンパイルも実行も通る (テクスチャが張られていないスロットは 0 を返すため)
    @{
        label = 'kTerrainSplatSrvSlot / register(t20)'
        sites = @{
            'src\Engine\Renderer\TerrainPass.h'   = 'constexpr\s+uint32_t\s+kTerrainSplatSrvSlot\s*=\s*(\d+)'
            'assets\shaders\terrain_common.hlsli' = 'Texture2D\s+gTerrainSplat\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        label = 'kTerrainAlbedoSrvSlot / register(t21)'
        sites = @{
            'src\Engine\Renderer\TerrainPass.h'   = 'constexpr\s+uint32_t\s+kTerrainAlbedoSrvSlot\s*=\s*(\d+)'
            'assets\shaders\terrain_common.hlsli' = 'Texture2D\s+gTerrainAlbedo\[[^\]]*\]\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        label = 'kTerrainNormalSrvSlot / register(t25)'
        sites = @{
            'src\Engine\Renderer\TerrainPass.h'   = 'constexpr\s+uint32_t\s+kTerrainNormalSrvSlot\s*=\s*(\d+)'
            'assets\shaders\terrain_common.hlsli' = 'Texture2D\s+gTerrainNormal\[[^\]]*\]\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        # レイヤ数は CB の配列長そのもの。ずれると tint / tiling が丸ごと別の場所を指す
        label = 'kTerrainLayerCount / MYE_TERRAIN_LAYERS'
        sites = @{
            'src\Engine\Renderer\TerrainPass.h'   = 'constexpr\s+uint32_t\s+kTerrainLayerCount\s*=\s*(\d+)'
            'assets\shaders\terrain_common.hlsli' = '#\s*define\s+MYE_TERRAIN_LAYERS\s+(\d+)'
            'src\Engine\Engine\Asset\TerrainAsset.h' = 'constexpr\s+uint32_t\s+kMaxLayers\s*=\s*(\d+)'
        }
    },
    @{
        # M56c: HZB の縮小 CS のスレッドグループ辺長。C++ はディスパッチ数の切り上げに、
        # HLSL は numthreads に使う。食い違うと**画面の右端・下端だけ**が縮小されずに
        # 前フレームの値が残る — 絵は普通に出てしまうので、機械照合が唯一の防波堤
        label = 'kHzbThreadGroupSize / MYE_HZB_TG'
        sites = @{
            'src\Engine\Renderer\HzbPass.h'     = 'constexpr\s+int\s+kHzbThreadGroupSize\s*=\s*(\d+)'
            'assets\shaders\hzb_reduce.cs.hlsl' = '#\s*define\s+MYE_HZB_TG\s+(\d+)'
        }
    }
    @{
        # M56d: SSR の光線 1 本あたりの最大反復回数。C++ 側は SsrSelfTest が上限の妥当性を
        # 見るためだけに持っているが、食い違うと **HLSL 側だけが本当の予算**になり、
        # 「反射が途中で切れる」形でしか現れない (絵は普通に出る) ので機械照合しておく
        label = 'kSsrMaxSteps / MYE_SSR_MAX_STEPS'
        sites = @{
            'src\Engine\Renderer\SsrPass.h' = 'constexpr\s+int\s+kSsrMaxSteps\s*=\s*(\d+)'
            'assets\shaders\ssr_trace.hlsl'  = '#\s*define\s+MYE_SSR_MAX_STEPS\s+(\d+)'
        }
    }
    @{
        # M56f: 反射プローブの最大数 = 定数バッファ内の配列長そのもの。**光パス
        # (deferred_light) と SSR (ssr_trace) の 2 本が同じ配列を持つ**ので、C++ と
        # 食い違うと CB のレイアウトごとずれる (絵は普通に出る) — 機械照合が唯一の防波堤
        label = 'kMaxReflectionProbes / MYE_MAX_REFLECTION_PROBES'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kMaxReflectionProbes\s*=\s*(\d+)'
            'assets\shaders\common.hlsli'       = '#\s*define\s+MYE_MAX_REFLECTION_PROBES\s+(\d+)'
        }
    },
    @{
        # M57a: フロクセル CS のスレッドグループ (XY)。C++ 側はこの値でディスパッチの
        # グループ数を切り上げ計算し、HLSL 側は numthreads に使う。食い違うと
        # **グリッドの一部が書かれないまま残り、前フレームの残骸を積分する** —
        # 絵は出るのにフォグだけがちらつくという、目で追いにくい壊れ方をする
        label = 'froxel::kGroupSize / MYE_FROXEL_GROUP'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'   = 'constexpr\s+int\s+kGroupSize\s*=\s*(\d+)'
            # M57c: HLSL 側の正本は froxel_common.hlsli 1 本にまとめた (clear / inject /
            # temporal / integrate の 4 本が同じ割り方を要求するようになったため)。
            # 各 .cs.hlsl が #define を持たなくなったので、照合先もここへ移す
            'assets\shaders\froxel_common.hlsli'  = '#\s*define\s+MYE_FROXEL_GROUP\s+(\d+)'
        }
    },
    @{
        # M57c: フロクセルのジッタ列の周期。C++ が実際にジッタ値を計算して CB へ載せるので
        # HLSL 側には出てこないが、**カメラジッタ (TAA) の周期と同じ長さ**でなければ
        # 「1 巡」が最小公倍数まで伸びて、決定的撮影で撮った 2 枚がどちらも収束前の
        # 別状態になる。両者は別ヘッダに住んでいるので機械照合しておく
        label = 'froxel::kJitterSequenceLength / camerajitter::kSequenceLength'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+uint32_t\s+kJitterSequenceLength\s*=\s*(\d+)'
            'src\Engine\Renderer\PostFxMath.h'  = 'constexpr\s+uint32_t\s+kSequenceLength\s*=\s*(\d+)'
        }
    },
    @{
        # M57d: Deferred 光パスがフロクセルの積分結果を読む SRV スロット (統合契約 予約 2 の t15)。
        # C++ 側は gbSrvs[] の**位置**でしか表現されないので、static_assert で定数へ結び直し
        # てある (DeferredPath.cpp)。ここが HLSL の register(t15) と食い違うと
        # **霧が丸ごと 0 になるだけ**でコンパイルも実行も通る (張られていないスロットは 0)。
        # ★t13/t14 は M56 (SSR / 反射プローブ) の空席。詰めると統合時に無言で潰し合う
        label = 'froxel::kSrvSlot / deferred_light register(t15)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kSrvSlot\s*=\s*(\d+)'
            'assets\shaders\deferred_light.hlsl' = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        # M57e: Forward 系がフロクセルの積分結果を読む SRV スロット (統合契約 予約 2 の t7)。
        # **同じ番号を要求するファイルが 5 つ**ある — forward_lit / _instanced / _skinned /
        # _terrain の 4 本と、スカイボックス (ホストが張った t7 をそのまま読む)。
        # C++ 側は frameSrvs[] / fwdSrvs[] の**位置**でしか表現されないので static_assert で
        # 定数へ結び直してある (ForwardPath.cpp / DeferredPath.cpp)。食い違うと
        # **その 1 本だけ霧が 0 になる** = 半透明だけ霧が抜ける、という形で静かに壊れる
        label = 'froxel::kForwardSrvSlot / forward_* + skybox register(t7)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'          = 'constexpr\s+int\s+kForwardSrvSlot\s*=\s*(\d+)'
            'assets\shaders\forward_lit.hlsl'            = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
            'assets\shaders\forward_lit_instanced.hlsl'  = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
            'assets\shaders\forward_skinned.hlsl'        = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
            'assets\shaders\forward_terrain.hlsl'        = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
            'assets\shaders\skybox.hlsl'                 = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
            'assets\shaders\skybox_cubemap.hlsl'         = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        # M57e: パーティクル PS のフロクセルスロット。t0=インスタンス / t1=テクスチャ /
        # t2=深度 の次。CPU バックエンドの PSSetShaderResources と食い違うと
        # **粒子だけ霧が 0** (加算合成なので「粒子が浮く」形で出る)
        label = 'froxel::kParticleSrvSlot / particle_render register(t3)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'      = 'constexpr\s+int\s+kParticleSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render.hlsl'    = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        # M57追補: **GPU** パーティクル PS のフロクセルスロット。CPU 版 (t3) と番号が違う —
        # GPU 版は t3 をフリップブックテクスチャが占有しているので t4。
        # 食い違うと **GPU 粒子だけ霧が 0** になるが、コンパイルも実行も通る
        # (張られていないスロットは 0 を返す)。CPU 版と揃っていないのは意図的なので、
        # 「番号が違う = バグ」と早合点して片方を書き換えないこと
        label = 'froxel::kGpuParticleSrvSlot / particle_render_gpu register(t4)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'        = 'constexpr\s+int\s+kGpuParticleSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render_gpu.hlsl'  = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
        }
    },
    @{
        # M57追補: VFX (Sprite / Trail / TextMesh) PS のフロクセルスロット。
        # vfx_sprite.hlsl は t0 (自前テクスチャ) しか使わないので t1。食い違うと
        # **VFX だけ霧が 0** になるが、コンパイルも実行も通る
        label = 'froxel::kVfxSrvSlot / vfx_sprite register(t1)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h' = 'constexpr\s+int\s+kVfxSrvSlot\s*=\s*(\d+)'
            'assets\shaders\vfx_sprite.hlsl'    = 'Texture3D\s+gFroxelVolume\s*:\s*register\(t(\d+)\)'
        }
    },
    # M42追補: GPU alpha ソートのブロック長とスレッド数。C++ 側はパス表 (どのパスを何グループ
    # 起動するか) の計算に、HLSL 側は groupshared 配列長と numthreads に同じ値を使う。
    # 食い違うと **ソートが途中までしか効かない** — 絵が微妙に乱れるだけでコンパイルも実行も
    # 通ってしまうので、機械照合が唯一の防波堤。
    # ★ブロック長は LDS 消費に直結する (uint 2 本 x 2048 = 16KB)。cs_5_0 の上限は 32KB
    @{
        label = 'kParticleSortBlock / MYE_PARTICLE_SORT_BLOCK'
        sites = @{
            'src\Engine\Engine\Particles\ParticleCurves.h' = 'kParticleSortBlock\s*=\s*(\d+)'
            'assets\shaders\particle_sort_common.hlsli'    = '#\s*define\s+MYE_PARTICLE_SORT_BLOCK\s+(\d+)'
        }
    },
    @{
        label = 'kParticleSortThreads / MYE_PARTICLE_SORT_THREADS'
        sites = @{
            'src\Engine\Engine\Particles\ParticleCurves.h' = 'kParticleSortThreads\s*=\s*(\d+)'
            'assets\shaders\particle_sort_common.hlsli'    = '#\s*define\s+MYE_PARTICLE_SORT_THREADS\s+(\d+)'
        }
    },
    @{
        label = 'kParticleSortMergeThreads / MYE_PARTICLE_SORT_MERGE_THREADS'
        sites = @{
            'src\Engine\Engine\Particles\ParticleCurves.h' = 'kParticleSortMergeThreads\s*=\s*(\d+)'
            'assets\shaders\particle_sort_common.hlsli'    = '#\s*define\s+MYE_PARTICLE_SORT_MERGE_THREADS\s+(\d+)'
        }
    },
    # ---- M63d: パーティクルのライティングのスロット ----
    # particle_light.hlsli は **register 宣言を持つ .hlsli** で、番号は include する側が
    # マクロで渡す (CPU 版と GPU 版で空きが違うため)。C++ 側は張る場所として同じ番号を
    # 使うので、食い違うと「CB が全 0 のまま = 粒子が真っ黒」「CSM が別のテクスチャ =
    # 影が出鱈目な位置」になる。**どちらもコンパイルも実行も通る**ので機械照合が唯一の防波堤。
    # ★1 グループ = 1 整数なので、CPU 版と GPU 版は別グループに分けてある
    #   (両者は別シェーダでバインド空間を共有せず、番号が違うのが正しい)。
    @{
        label = 'particlelight::kCpuLightCbSlot / MYE_PARTICLE_LIGHT_SLOT_CB (CPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'      = 'constexpr\s+int\s+kCpuLightCbSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render.hlsl'    = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_CB\s+b(\d+)'
        }
    },
    @{
        label = 'particlelight::kGpuLightCbSlot / MYE_PARTICLE_LIGHT_SLOT_CB (GPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'        = 'constexpr\s+int\s+kGpuLightCbSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render_gpu.hlsl'  = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_CB\s+b(\d+)'
        }
    },
    @{
        label = 'particlelight::kCpuShadowSrvSlot / MYE_PARTICLE_LIGHT_SLOT_CSM (CPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'      = 'constexpr\s+int\s+kCpuShadowSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render.hlsl'    = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_CSM\s+t(\d+)'
        }
    },
    @{
        label = 'particlelight::kGpuShadowSrvSlot / MYE_PARTICLE_LIGHT_SLOT_CSM (GPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'        = 'constexpr\s+int\s+kGpuShadowSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render_gpu.hlsl'  = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_CSM\s+t(\d+)'
        }
    },
    @{
        label = 'particlelight::kCpuIrradianceSrvSlot / MYE_PARTICLE_LIGHT_SLOT_IRR (CPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'      = 'constexpr\s+int\s+kCpuIrradianceSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render.hlsl'    = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_IRR\s+t(\d+)'
        }
    },
    @{
        label = 'particlelight::kGpuIrradianceSrvSlot / MYE_PARTICLE_LIGHT_SLOT_IRR (GPU)'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'        = 'constexpr\s+int\s+kGpuIrradianceSrvSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render_gpu.hlsl'  = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_IRR\s+t(\d+)'
        }
    },
    @{
        # 比較サンプラだけは CPU / GPU とも s1 = 3 サイトを 1 グループで見られる
        label = 'particlelight::kShadowSamplerSlot / MYE_PARTICLE_LIGHT_SLOT_SAMP'
        sites = @{
            'src\Engine\Renderer\RenderTypes.h'        = 'constexpr\s+int\s+kShadowSamplerSlot\s*=\s*(\d+)'
            'assets\shaders\particle_render.hlsl'      = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_SAMP\s+s(\d+)'
            'assets\shaders\particle_render_gpu.hlsl'  = '#\s*define\s+MYE_PARTICLE_LIGHT_SLOT_SAMP\s+s(\d+)'
        }
    }
)
foreach ($g in $constGroups) {
    $values = @{}
    foreach ($rel in $g.sites.Keys) {
        $path = Join-Path $repo $rel
        if (-not (Test-Path $path)) {
            Write-Host "ERROR [rule 9] missing file: $rel"
            $errors++
            continue
        }
        $hit = Select-String -Path $path -Pattern $g.sites[$rel] | Select-Object -First 1
        if (-not $hit) {
            Write-Host "ERROR [rule 9] ${rel}: $($g.label) definition not found"
            $errors++
            continue
        }
        $values[$rel] = [int]$hit.Matches[0].Groups[1].Value
    }
    if ($values.Count -eq $g.sites.Count -and ($values.Values | Select-Object -Unique).Count -ne 1) {
        foreach ($rel in $values.Keys) { Write-Host "ERROR [rule 9] ${rel}: $($values[$rel])" }
        Write-Host "ERROR [rule 9] $($g.label) must match across C++ and HLSL"
        $errors++
    }
}

# 規則 10: ローカライズ (M47)
# 10-a  Tr() を printf 系の**唯一の引数**として渡さない
#       ImGui::Text 系 / SetTooltip は printf。可変引数が無いのに訳文へ % が入ると
#       未定義動作になる。TextUnformatted(Tr(x)) か Text("%s", Tr(x)) を使うこと。
#       可変引数を伴う Text(Tr(x), a, b) は「訳文自体が書式」の正当な用法なので許す
#       — 指定子の並びは 10-b が en/ja 一致を機械検査している
# 10-b  LocalizationTable.inl の en/ja が整合していること
#       - どちらも非空
#       - "###" を含む行は "###" 以降 (= ImGui の ID) が完全一致
#       - 変換指定子の並びが一致 (MSVC printf は "%1$s" 形式に非対応で語順を変えられない)
#       - "###" の右辺がテーブル内で一意 (ウィンドウ ID の衝突防止)
# 注: 日本語を含む行を読むので Select-String は使わない
#     (Windows PowerShell 5.1 は BOM 無しファイルを ANSI として読み、マッチが不発になる)
$trOnly = '(::)?(mye::)?Tr\s*\(\s*(::)?(mye::)?StrId::\w+\s*\)\s*\)'
$trFmtPatterns = @(
    'ImGui::(?:Text|TextDisabled|TextWrapped|BulletText|LabelText|SetTooltip|SetItemTooltip)\s*\(\s*' + $trOnly,
    'ImGui::TextColored\s*\([^,]+,\s*' + $trOnly
)
foreach ($f in Get-Sources) {
    foreach ($p in $trFmtPatterns) {
        Test-CodeLines $f $p 'rule 10' 'Tr() alone as a printf format - use TextUnformatted(Tr(x)) or Text("%s", Tr(x))'
    }
}

$tablePath = Join-Path $repo 'src\Engine\Core\LocalizationTable.inl'
if (-not (Test-Path $tablePath)) {
    Write-Host "ERROR [rule 10] missing file: src\Engine\Core\LocalizationTable.inl"
    $errors++
} else {
    $seenIds = @{}
    # 1 エントリが複数行にまたがることがあり、隣接文字列リテラルの連結も使うので
    # 全文に対してマッチする。行番号は先頭からの改行数で求める
    $content = [System.IO.File]::ReadAllText($tablePath)
    $strSeq = '"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
    $entryRe = 'MYE_STR\(\s*(\w+)\s*,\s*(' + $strSeq + ')\s*,\s*(' + $strSeq + ')\s*\)'
    $found = [regex]::Matches($content, $entryRe)
    $declared = [regex]::Matches($content, '(?m)^\s*MYE_STR\(').Count
    if ($found.Count -ne $declared) {
        Write-Host "ERROR [rule 10] ${tablePath}: $declared MYE_STR line(s) but only $($found.Count) parsed - malformed entry"
        $errors++
    }
    # 隣接文字列リテラルを 1 本に畳む ("a" "b" -> ab)
    function Join-Literals([string]$seq) {
        ([regex]::Matches($seq, '"((?:[^"\\]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }) -join ''
    }
    foreach ($m in $found) {
        $lineNo = ($content.Substring(0, $m.Index) -split "`n").Count
        $id = $m.Groups[1].Value
        $en = Join-Literals $m.Groups[2].Value
        $ja = Join-Literals $m.Groups[3].Value

        if ($en -eq '' -or $ja -eq '') {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: empty string"
            $errors++
        }
        $enId = if ($en -match '###(.*)$') { $Matches[1] } else { '' }
        $jaId = if ($ja -match '###(.*)$') { $Matches[1] } else { '' }
        if ($enId -ne $jaId) {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: ### id differs ('$enId' vs '$jaId')"
            $errors++
        }
        if ($enId -ne '') {
            if ($seenIds.ContainsKey($enId)) {
                Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: duplicate ### id '$enId' (also $($seenIds[$enId]))"
                $errors++
            } else {
                $seenIds[$enId] = $id
            }
        }
        # 変換指定子 ("%%" はリテラルなので除外) の並びを比較
        $specRe = '%(?!%)[-+ #0-9.*]*(?:hh|h|ll|l|z|j|t|L|I32|I64|I)?[a-zA-Z]'
        $enSpecs = ([regex]::Matches($en, $specRe) | ForEach-Object { $_.Value }) -join ','
        $jaSpecs = ([regex]::Matches($ja, $specRe) | ForEach-Object { $_.Value }) -join ','
        if ($enSpecs -ne $jaSpecs) {
            Write-Host "ERROR [rule 10] ${tablePath}:${lineNo}: ${id}: format specifiers differ ('$enSpecs' vs '$jaSpecs')"
            $errors++
        }
    }
}

# 規則 11: ABI ミラーの機械照合 (M51h)
# Interop.cs は EngineAPI.h の**位置ベース**ミラーで、実行時の版検証が無い —
# 順序・件数・名前・引数個数がズレると全スロットが静かに別関数を指す。
# 11-a  EngineAPI.h (MyeEngineApi) と Interop.cs (MyeEngineApi) のスロット列が
#       順序・件数・名前とも完全一致
# 11-b  各スロットの引数個数一致 (C++ の仮引数数 == C# の delegate* 型リスト数 - 1 (戻り値))
# 11-c  MYE_API_VERSION とスロット数の対応が下表と一致 — スロットを足すときは
#       version の bump とこの表の更新を**同時に**行うこと (どちらか片方だとここで止まる)
# 11-d  EngineApiTable.cpp が全スロットに `out.<Name> =` で実装を充填している
$apiVersionSlots = @{ 11 = 73; 12 = 87; 13 = 94; 14 = 102 }
$apiHeaderPath = Join-Path $repo 'src\Shared\EngineAPI.h'
$interopPath = Join-Path $repo 'src\Scripting\Interop.cs'
$apiTablePath = Join-Path $repo 'src\Engine\Engine\Script\EngineApiTable.cpp'
if (-not (Test-Path $apiHeaderPath) -or -not (Test-Path $interopPath) -or -not (Test-Path $apiTablePath)) {
    Write-Host "ERROR [rule 11] missing EngineAPI.h / Interop.cs / EngineApiTable.cpp"
    $errors++
} else {
    $apiText = [System.IO.File]::ReadAllText($apiHeaderPath)
    $csText = [System.IO.File]::ReadAllText($interopPath)

    # C++ 側: struct MyeEngineApi { ... } の関数ポインタメンバを宣言順に抽出
    $cppSlots = @()
    $structMatch = [regex]::Match($apiText, 'struct\s+MyeEngineApi\s*\{(.*?)\n\};', 'Singleline')
    if (-not $structMatch.Success) {
        Write-Host "ERROR [rule 11] EngineAPI.h: struct MyeEngineApi not found"
        $errors++
    } else {
        foreach ($m in [regex]::Matches($structMatch.Groups[1].Value,
                                        '\(\s*\*\s*(\w+)\s*\)\s*\(([^;]*?)\)\s*;', 'Singleline')) {
            $params = $m.Groups[2].Value.Trim()
            $argc = if ($params -eq '' -or $params -eq 'void') { 0 } else { ($params -split ',').Count }
            $cppSlots += [pscustomobject]@{ Name = $m.Groups[1].Value; Args = $argc }
        }
    }

    # C# 側: internal unsafe struct MyeEngineApi { ... } の delegate* フィールドを宣言順に抽出
    $csSlots = @()
    $csStruct = [regex]::Match($csText, 'struct\s+MyeEngineApi\s*\{(.*?)\n    \}', 'Singleline')
    if (-not $csStruct.Success) {
        Write-Host "ERROR [rule 11] Interop.cs: struct MyeEngineApi not found"
        $errors++
    } else {
        foreach ($m in [regex]::Matches($csStruct.Groups[1].Value,
                                        'delegate\*\s*unmanaged<(.*?)>\s*(\w+)\s*;', 'Singleline')) {
            # 型リストは <引数..., 戻り値> — 引数個数はリスト数 - 1
            $types = ($m.Groups[1].Value -split ',').Count
            $csSlots += [pscustomobject]@{ Name = $m.Groups[2].Value; Args = $types - 1 }
        }
    }

    # 検査器自体の防御: 構造体は見つかったのにスロット 0 件 = 抽出正規表現の腐り。
    # 黙って素通りさせない (このガードが無いと将来の書式変更で検査が空になる)
    if ($structMatch.Success -and $cppSlots.Count -eq 0) {
        Write-Host "ERROR [rule 11] EngineAPI.h: parsed 0 slots - checker regex is stale"
        $errors++
    }
    if ($csStruct.Success -and $csSlots.Count -eq 0) {
        Write-Host "ERROR [rule 11] Interop.cs: parsed 0 slots - checker regex is stale"
        $errors++
    }
    if ($cppSlots.Count -gt 0 -and $csSlots.Count -gt 0) {
        if ($cppSlots.Count -ne $csSlots.Count) {
            Write-Host "ERROR [rule 11] slot count differs: EngineAPI.h=$($cppSlots.Count) Interop.cs=$($csSlots.Count)"
            $errors++
        }
        $n = [Math]::Min($cppSlots.Count, $csSlots.Count)
        for ($i = 0; $i -lt $n; $i++) {
            if ($cppSlots[$i].Name -ne $csSlots[$i].Name) {
                Write-Host "ERROR [rule 11] slot #$($i + 1) name differs: EngineAPI.h='$($cppSlots[$i].Name)' Interop.cs='$($csSlots[$i].Name)'"
                $errors++
                break # 1 個ズレると以降は全部ズレる — 先頭の乖離だけ報告する
            }
            if ($cppSlots[$i].Args -ne $csSlots[$i].Args) {
                Write-Host "ERROR [rule 11] slot '$($cppSlots[$i].Name)': arg count differs (C++=$($cppSlots[$i].Args) C#=$($csSlots[$i].Args))"
                $errors++
            }
        }

        # 11-c: version ⇄ スロット数の同時性
        $verMatch = [regex]::Match($apiText, '#define\s+MYE_API_VERSION\s+(\d+)u')
        if (-not $verMatch.Success) {
            Write-Host "ERROR [rule 11] EngineAPI.h: MYE_API_VERSION not found"
            $errors++
        } else {
            $ver = [int]$verMatch.Groups[1].Value
            if (-not $apiVersionSlots.ContainsKey($ver)) {
                Write-Host "ERROR [rule 11] MYE_API_VERSION=$ver is not in the checker's version table - add it together with the bump"
                $errors++
            } elseif ($apiVersionSlots[$ver] -ne $cppSlots.Count) {
                Write-Host "ERROR [rule 11] MYE_API_VERSION=$ver expects $($apiVersionSlots[$ver]) slots but EngineAPI.h has $($cppSlots.Count) - bump the version and this table together"
                $errors++
            }
        }

        # 11-d: EngineApiTable.cpp が全スロットを充填している
        $tableText = [System.IO.File]::ReadAllText($apiTablePath)
        foreach ($s in $cppSlots) {
            if ($tableText -notmatch ('out\.' + [regex]::Escape($s.Name) + '\s*=')) {
                Write-Host "ERROR [rule 11] EngineApiTable.cpp: slot '$($s.Name)' is never assigned (out.$($s.Name) = ...)"
                $errors++
            }
        }
    }
}

Write-Host "=== result: $errors error(s), $warnings warning(s) ==="
if ($errors -gt 0) { exit 1 }
exit 0
