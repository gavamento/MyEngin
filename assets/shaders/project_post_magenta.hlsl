// project_post_magenta.hlsl  コンパイル失敗フォールバック: マゼンタ全画面 (M78b)
// ユーザーポストのコンパイルが失敗したとき、このシェーダでそのパスの出力を上書きする。
// 「なにかおかしい」と気づけるよう派手な色を使う (spec §4.1 失敗時挙動)。
#include "ProjectPostCommon.hlsli"

float4 PSMain(ProjectPostVSOut i) : SV_Target
{
    // マゼンタ (R=1, G=0, B=1, A=1)
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
