"""DeepModal 用の形状生成器 (DeepModalModel/shapegen)。

tools/deepmodal (layout / meshio / voxelize) を import するため、このパッケージの import 時に
そのディレクトリを sys.path へ足す。multiprocessing (spawn) の子プロセスも shapegen を
import した時点で同じ設定になる。"""
import sys
from pathlib import Path

_TOOLS_DEEPMODAL = Path(__file__).resolve().parents[2] / "tools" / "deepmodal"
if str(_TOOLS_DEEPMODAL) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DEEPMODAL))
