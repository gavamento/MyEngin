#====================================================================================
#                          conftest.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          pytest の import path とスレッド数の固定
#====================================================================================
"""tests/ から `import shapegen` できるよう DeepModalModel を sys.path に足す
(shapegen 自身が tools/deepmodal を足す)。BLAS のスレッド数は tools/deepmodal/conftest.py と
同じ理由で 1 に固定する (テスト実行順で数値が変わるのを防ぐ)。"""
import os
import sys
from pathlib import Path

os.environ.setdefault("OMP_NUM_THREADS", "1")
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
