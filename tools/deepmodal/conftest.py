#====================================================================================
#                          conftest.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          pytest が tools/deepmodal を import path に乗せるための空フック
#====================================================================================
"""tests/ に __init__.py を置かない (pytest の既定 import mode に合わせる) ため、
tests/*.py から `import layout` 等の裸 import ができるよう、pytest に
このファイルの置き場所 (tools/deepmodal) を sys.path へ足させる。
conftest.py はテストコード本体ではなく、意図的に空 (副作用は pytest 側の
rootdir 検出だけで十分)。"""
