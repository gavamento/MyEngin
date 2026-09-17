#====================================================================================
#                          conftest.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          pytest が tools/deepmodal を import path に乗せるための空フック
#====================================================================================
"""tests/ に __init__.py を置かない (pytest の既定 import mode に合わせる) ため、
tests/*.py から `import layout` 等の裸 import ができるよう、pytest に
このファイルの置き場所 (tools/deepmodal) を sys.path へ足させる。

★BLAS/OpenMP のスレッド数を 1 に固定する (sub-04 で判明): `test_export.py` が
`import torch` すると、torch が同梱する MKL/OpenMP ランタイムが numpy/scipy の
それと別に初期化され、以後のスレッド数がテスト実行順序に依存して変わる。
`test_modal.py::test_lobpcg_matches_exact_on_small_mesh` の shift-invert 解の
残差 (`RESIDUAL_ACCEPT` 判定) が **torch を import した後だと数値的に変わり**、
1e-5 の境界をわずかに超えて落ちることを実測した (test_export.py を先に集めると
再現、test_modal.py を先にすると通る — pytest のファイル収集順に依存する flaky
テストだった)。`dataset.py` が multiprocessing worker に対して既にやっている
「Pool 生成前にスレッド数を 1 に固定する」のと同じ対処を、pytest プロセス全体
(collection 開始前 = どのテストファイルが numpy/torch を import するより前) に
適用する。conftest.py は pytest が最初に import するファイルなので、ここで
setdefault すれば衝突しない (呼び出し側が明示的に変えていれば尊重する)。"""
import os

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
