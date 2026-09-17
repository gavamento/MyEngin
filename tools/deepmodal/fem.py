#====================================================================================
#                          fem.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          hex8 有限要素の Ke/Me と占有ボクセルからの組み立て
#====================================================================================
"""hex8 (8 節点等パラメトリック六面体) 要素の剛性行列 Ke・集中質量 Me と、
32^3 ボクセル占有グリッドからのグローバル行列組み立て。

要素は常に一辺 h の立方体 (ボクセルそのもの) なので、アイソパラメトリック写像の
ヤコビアンは対角 (h/2)*I になる。これにより:
  - B (歪み-変位) 行列は自然座標微分の (2/h) 倍 = h に反比例
  - dV = det(J) dxi.deta.dzeta は h^3 に比例
  - Ke = ∫ B^T (E*d_hat(nu)) B dV は正味 h 倍 (E は D から線形因子として分離できる)
なので Ke(h,E,nu) = h * E * hex8_ke_unit(nu) と書ける (spec §4.1 のスケール則の根拠)。
一般の非立方体要素ではこの分離は成り立たないが、このリポジトリの要素は常にボクセルなので
常に成り立つ。
"""
import numpy as np
from scipy.sparse import coo_matrix

# 8 節点の自然座標 (isoparametric hex、底面 CCW → 上面 CCW の標準順序)
_NATURAL = np.array([
    (-1.0, -1.0, -1.0),
    (1.0, -1.0, -1.0),
    (1.0, 1.0, -1.0),
    (-1.0, 1.0, -1.0),
    (-1.0, -1.0, 1.0),
    (1.0, -1.0, 1.0),
    (1.0, 1.0, 1.0),
    (-1.0, 1.0, 1.0),
])

# 対応する voxel コーナーオフセット (0/1)。fem.py の節点順序と assemble() の
# グローバル節点引きが同じ並びであることを保証する唯一の正本
CORNER_OFFSETS = np.array([
    (0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
    (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1),
], dtype=np.int64)

_GAUSS_1D = np.array([-1.0, 1.0]) / np.sqrt(3.0)  # 2 点 Gauss (重みは両方 1)


def _shape_derivs(xi: float, eta: float, zeta: float) -> np.ndarray:
    """8 節点形状関数の自然座標微分。戻り値 (3,8): 行 0=d/dxi, 1=d/deta, 2=d/dzeta。"""
    dN = np.zeros((3, 8))
    for i in range(8):
        xi_i, eta_i, zeta_i = _NATURAL[i]
        dN[0, i] = 0.125 * xi_i * (1.0 + eta * eta_i) * (1.0 + zeta * zeta_i)
        dN[1, i] = 0.125 * eta_i * (1.0 + xi * xi_i) * (1.0 + zeta * zeta_i)
        dN[2, i] = 0.125 * zeta_i * (1.0 + xi * xi_i) * (1.0 + eta * eta_i)
    return dN


def elasticity_matrix(nu: float) -> np.ndarray:
    """等方弾性の D 行列を E=1 で正規化した形 (D = E * elasticity_matrix(nu))。"""
    c = 1.0 / ((1.0 + nu) * (1.0 - 2.0 * nu))
    d = np.zeros((6, 6))
    d[0, 0] = d[1, 1] = d[2, 2] = c * (1.0 - nu)
    d[0, 1] = d[0, 2] = d[1, 0] = d[1, 2] = d[2, 0] = d[2, 1] = c * nu
    g = c * (1.0 - 2.0 * nu) * 0.5
    d[3, 3] = d[4, 4] = d[5, 5] = g
    return d


def hex8_ke_unit(nu: float) -> np.ndarray:
    """単位立方体 (h=1) / E=1 の要素剛性行列 (24x24)。2x2x2 Gauss 積分。
    Ke(h,E,nu) = h * E * hex8_ke_unit(nu) (モジュールdocstring参照)。"""
    D = elasticity_matrix(nu)
    Ke = np.zeros((24, 24))
    half = 0.5  # 自然座標 [-1,1] -> 物理 [-0.5,0.5] (h=1) のヤコビアン = half*I
    detJ = half ** 3
    for xi in _GAUSS_1D:
        for eta in _GAUSS_1D:
            for zeta in _GAUSS_1D:
                dN_phys = _shape_derivs(xi, eta, zeta) / half  # dN/dx = (1/half) dN/dxi
                B = np.zeros((6, 24))
                for i in range(8):
                    bx, by, bz = dN_phys[:, i]
                    B[0, i * 3 + 0] = bx
                    B[1, i * 3 + 1] = by
                    B[2, i * 3 + 2] = bz
                    B[3, i * 3 + 0] = by
                    B[3, i * 3 + 1] = bx
                    B[4, i * 3 + 1] = bz
                    B[4, i * 3 + 2] = by
                    B[5, i * 3 + 0] = bz
                    B[5, i * 3 + 2] = bx
                Ke += (B.T @ D @ B) * detJ  # 2 点則の重みは両方 1 なので省略
    return Ke


def hex8_ke(h: float, young: float, nu: float) -> np.ndarray:
    return h * young * hex8_ke_unit(nu)


def hex8_lumped_mass_fraction() -> np.ndarray:
    """要素質量 (rho*h^3) を 8 節点へ均等配分する比率 (総和 1)。"""
    return np.full(8, 1.0 / 8.0)


def hex8_me_diag(h: float, density: float) -> np.ndarray:
    """要素の集中質量 (24,)。各節点は 3 自由度とも同じ質量を持つ。"""
    per_node = density * (h ** 3) * hex8_lumped_mass_fraction()
    return np.repeat(per_node, 3)


def assemble_from_occupancy(occ: np.ndarray, h: float, young: float, density: float,
                             nu: float):
    """占有ボクセル (nx,ny,nz の bool/整数配列) から hex8 要素を組み立て、
    占有 voxel のコーナー節点だけを compact 化したグローバル K (CSR) / M (対角、1 次元
    ndarray) を返す。

    戻り値:
      K            : scipy.sparse.csr_matrix (3n, 3n)
      M            : ndarray (3n,) — 対角質量 (M そのもの、行列化していない)
      node_coords  : ndarray (n, 3) int64 — compact 節点 id -> voxel コーナー座標
      voxel_dofs   : ndarray (Nv, 24) int64 — 各占有 voxel の 24 自由度 (compact index)
      voxels       : ndarray (Nv, 3) int64 — 占有 voxel の座標 (voxel_dofs と対応)

    決定論: 節点 id は「(x,y,z) のコーナー座標を辞書式昇順に並べた」ときの順位で決まる
    (np.unique がソート済み配列を返すことを利用)。unordered を経由しないので同入力は
    常に同じ compact index を得る。
    """
    occ = np.asarray(occ)
    nx, ny, nz = occ.shape
    voxels = np.argwhere(occ != 0).astype(np.int64)
    if voxels.shape[0] == 0:
        raise ValueError("occupancy has no occupied voxel")
    # argwhere の走査順に依存しないよう明示的に (x,y,z) 昇順へソートしておく
    order = np.lexsort((voxels[:, 2], voxels[:, 1], voxels[:, 0]))
    voxels = voxels[order]

    corners = voxels[:, None, :] + CORNER_OFFSETS[None, :, :]  # (Nv, 8, 3)
    ny1 = ny + 1
    nz1 = nz + 1
    flat = corners[:, :, 0] * (ny1 * nz1) + corners[:, :, 1] * nz1 + corners[:, :, 2]
    unique_flat, inverse = np.unique(flat.ravel(), return_inverse=True)
    node_id_per_corner = inverse.reshape(flat.shape)  # (Nv, 8)
    n_nodes = unique_flat.shape[0]
    node_coords = np.stack(
        [unique_flat // (ny1 * nz1), (unique_flat // nz1) % ny1, unique_flat % nz1], axis=1)

    dofs = (node_id_per_corner[:, :, None] * 3
            + np.arange(3, dtype=np.int64)[None, None, :]).reshape(-1, 24)  # (Nv,24)

    Ke = hex8_ke(h, young, nu)
    me_local = hex8_me_diag(h, density)

    nv = voxels.shape[0]
    rows = np.broadcast_to(dofs[:, :, None], (nv, 24, 24)).reshape(-1).astype(np.int32)
    cols = np.broadcast_to(dofs[:, None, :], (nv, 24, 24)).reshape(-1).astype(np.int32)
    vals = np.broadcast_to(Ke[None, :, :], (nv, 24, 24)).reshape(-1)

    ndof = n_nodes * 3
    K = coo_matrix((vals, (rows, cols)), shape=(ndof, ndof)).tocsr()

    M = np.zeros(ndof)
    np.add.at(M, dofs.reshape(-1), np.tile(me_local, nv))

    return K, M, node_coords, dofs, voxels
