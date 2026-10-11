#====================================================================================
#                          stats.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          manifest から統計 (受理率・理由・ファミリ別・占有分布・FEM 見積り)
#====================================================================================
import math
from collections import Counter, defaultdict

import numpy as np

from . import constants as C

# ModelNet40 の実測 (data/modelnet40/stats.json、占有 bin ごとの eigsh 中央値 [s])。
# FEM (dataset.py) の所要時間の見積りにだけ使う
FEM_SECONDS_BY_OCC_BIN = [
    (0, 500, 5.5), (500, 1000, 10.8), (1000, 2000, 24.2), (2000, 3000, 46.5),
    (3000, 4500, 81.0), (4500, 6000, 135.6), (6000, 7500, 233.5), (7500, 9000, 361.4),
    (9000, 10 ** 9, 500.0),
]


def _fem_seconds(occ: int) -> float:
    for lo, hi, s in FEM_SECONDS_BY_OCC_BIN:
        if lo <= occ < hi:
            return s
    return 500.0


def _quantiles(values):
    if not values:
        return {}
    arr = np.asarray(values, dtype=np.float64)
    q = np.percentile(arr, [5, 25, 50, 75, 95])
    return {"p5": float(q[0]), "p25": float(q[1]), "median": float(q[2]), "p75": float(q[3]),
            "p95": float(q[4]), "min": float(arr.min()), "max": float(arr.max())}


def compute_stats(rows, target: int = 0, elapsed_seconds: float = 0.0) -> dict:
    accepted = [r for r in rows if r.get("status") == "ok"]
    rejected = [r for r in rows if r.get("status") != "ok"]
    reasons = Counter(r.get("reason", "") for r in rejected)
    by_family = defaultdict(lambda: {"total": 0, "accepted": 0, "reasons": Counter(), "occ": [],
                                     "coarse": set(), "est": [], "fem_seconds": 0.0})
    hist = Counter()
    rot = Counter()
    est_pairs = []
    fem_total = 0.0
    for r in rows:
        f = by_family[r.get("family", "?")]
        f["total"] += 1
        if r.get("status") == "ok":
            f["accepted"] += 1
            f["occ"].append(r["occ"])
            f["coarse"].add(r.get("coarse_key", ""))
            secs = _fem_seconds(int(r["occ"]))
            f["fem_seconds"] += secs
            fem_total += secs
            hist[min(int(r["occ"]) // 500 * 500, 9000)] += 1
            rot[r.get("rot_mode", "?")] += 1
            if r.get("est_occ") and r["est_occ"] > 0:
                est_pairs.append((math.log(r["est_occ"]), math.log(r["occ"])))
        else:
            f["reasons"][r.get("reason", "")] += 1
    regression = None
    if len(est_pairs) >= 10:
        x = np.array([p[0] for p in est_pairs])
        y = np.array([p[1] for p in est_pairs])
        A = np.vstack([x, np.ones_like(x)]).T
        coef, res, _, _ = np.linalg.lstsq(A, y, rcond=None)
        pred = A @ coef
        ss_res = float(np.sum((y - pred) ** 2))
        ss_tot = float(np.sum((y - y.mean()) ** 2))
        regression = {"slope": float(coef[0]), "intercept": float(coef[1]),
                      "r2": 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0,
                      "median_ratio_actual_over_est": float(np.exp(np.median(y - x)))}
    fam_out = {}
    for name, f in sorted(by_family.items()):
        fam_out[name] = {
            "total": f["total"], "accepted": f["accepted"],
            "accept_rate": f["accepted"] / f["total"] if f["total"] else 0.0,
            "reasons": dict(f["reasons"]),
            "occ": _quantiles(f["occ"]),
            "unique_coarse_keys": len(f["coarse"]),
            "fem_hours_est": f["fem_seconds"] / 3600.0,
        }
    return {
        "target": target,
        "total": len(rows),
        "accepted": len(accepted),
        "accept_rate": len(accepted) / len(rows) if rows else 0.0,
        "reasons": dict(reasons),
        "occ": _quantiles([r["occ"] for r in accepted]),
        "occ_histogram_500": {str(k): v for k, v in sorted(hist.items())},
        "rotation_modes": dict(rot),
        "unique_coarse_keys": len({r.get("coarse_key") for r in accepted}),
        "est_vs_actual_log_regression": regression,
        "fem_hours_est_serial": fem_total / 3600.0,
        "fem_hours_est_jobs11": fem_total / 3600.0 / 11.0,
        "elapsed_seconds": elapsed_seconds,
        "families": fam_out,
        "family_set_version": C.FAMILY_SET_VERSION,
    }


def format_stats(stats: dict) -> str:
    lines = []
    lines.append(f"total {stats['total']}  accepted {stats['accepted']}  rate {stats['accept_rate']:.1%}"
                 f"  unique16 {stats['unique_coarse_keys']}")
    if stats["occ"]:
        o = stats["occ"]
        lines.append(f"occ p5/25/50/75/95 = {o['p5']:.0f}/{o['p25']:.0f}/{o['median']:.0f}/{o['p75']:.0f}/{o['p95']:.0f}")
    lines.append("reasons: " + ", ".join(f"{k}={v}" for k, v in sorted(stats["reasons"].items(), key=lambda kv: -kv[1])))
    lines.append(f"FEM estimate: {stats['fem_hours_est_serial']:.1f} h serial, {stats['fem_hours_est_jobs11']:.1f} h at 11 jobs")
    if stats.get("est_vs_actual_log_regression"):
        r = stats["est_vs_actual_log_regression"]
        lines.append(f"est→actual: slope {r['slope']:.2f} r2 {r['r2']:.2f} median(actual/est) {r['median_ratio_actual_over_est']:.2f}")
    lines.append(f"{'family':20s} {'total':>6s} {'ok':>6s} {'rate':>6s} {'occ_med':>8s} {'uniq16':>7s}  top reasons")
    for name, f in stats["families"].items():
        med = f["occ"].get("median", 0.0) if f["occ"] else 0.0
        top = ", ".join(f"{k}={v}" for k, v in sorted(f["reasons"].items(), key=lambda kv: -kv[1])[:3])
        lines.append(f"{name:20s} {f['total']:6d} {f['accepted']:6d} {f['accept_rate']:6.1%} {med:8.0f} {f['unique_coarse_keys']:7d}  {top}")
    return "\n".join(lines)
