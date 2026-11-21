#!/usr/bin/env python3
"""MM-CTA payoff matrix over p1 (MM half-spread) and p3 (CTA entry).

Input: f_p1_p2_p3.csv from mm_arb_cta_grid with n_arb=0. Rows are p1,
columns are p3; p2 is a fixed dummy value.

    ./venv/bin/python visualize/nash_p1p3.py build/runs/<run> --stat median
"""

import argparse
import csv
import itertools
import os
import sys

import numpy as np


def load(path, stat):
    if os.path.isdir(path):
        path = os.path.join(path, "f_p1_p2_p3.csv")
    rows = list(csv.DictReader(open(path, newline="")))
    mm_col = "mm_pnl" if stat == "mean" else "mm_pnl_med"
    cta_col = "cta_pnl" if stat == "mean" else "cta_pnl_med"
    p1s = sorted({float(r["p1"]) for r in rows})
    p3s = sorted({float(r["p3"]) for r in rows})
    A = np.full((len(p1s), len(p3s)), np.nan)
    B = np.full_like(A, np.nan)
    for r in rows:
        i = p1s.index(float(r["p1"]))
        j = p3s.index(float(r["p3"]))
        A[i, j] = float(r[mm_col]) / (2 * int(r["n_mm"]))
        B[i, j] = float(r[cta_col]) / (2 * int(r["n_cta"]))
    return path, p1s, p3s, A, B


def show(name, p1s, p3s, M):
    print(name)
    print("p1\\p3 " + " ".join(f"{v:>8.0f}" for v in p3s))
    for i, p1 in enumerate(p1s):
        print(f"{p1:>5.0f} " + " ".join(f"{M[i, j]:>8.0f}"
                                         for j in range(len(p3s))))


def best_responses(p1s, p3s, A, B):
    br_mm = [(p3s[j], p1s[max(range(len(p1s)), key=lambda i: A[i, j])])
             for j in range(len(p3s))]
    br_cta = [(p1s[i], p3s[max(range(len(p3s)), key=lambda j: B[i, j])])
              for i in range(len(p1s))]
    print("MM best response (p3 -> p1):", br_mm)
    print("CTA best response (p1 -> p3):", br_cta)
    pure = []
    for i, p1 in enumerate(p1s):
        j = max(range(len(p3s)), key=lambda k: B[i, k])
        i2 = max(range(len(p1s)), key=lambda k: A[k, j])
        if i2 == i:
            pure.append((p1, p3s[j], A[i, j], B[i, j]))
    print("pure Nash (p1,p3,MM,CTA):", pure if pure else "none")


def mixed_nash(p1s, p3s, A, B, max_support=3, tol=1e-6):
    sols = []
    n1, n2 = A.shape
    for k in range(1, min(n1, n2, max_support) + 1):
        for I in itertools.combinations(range(n1), k):
            for J in itertools.combinations(range(n2), k):
                try:
                    M = np.zeros((k + 1, k + 1))
                    M[:k, :k] = A[np.ix_(I, J)]
                    M[:k, k] = -1
                    M[k, :k] = 1
                    rhs = np.array([0.0] * k + [1.0])
                    s = np.linalg.solve(M, rhs)
                    q, u = s[:k], s[k]
                    M2 = np.zeros((k + 1, k + 1))
                    M2[:k, :k] = B[np.ix_(I, J)].T
                    M2[:k, k] = -1
                    M2[k, :k] = 1
                    s2 = np.linalg.solve(M2, rhs)
                    p, v = s2[:k], s2[k]
                except np.linalg.LinAlgError:
                    continue
                if min(p) < -1e-8 or min(q) < -1e-8:
                    continue
                if any(A[r, list(J)] @ q > u + tol
                       for r in range(n1) if r not in I):
                    continue
                if any(p @ B[list(I), c] > v + tol
                       for c in range(n2) if c not in J):
                    continue
                sols.append((I, J, p, q, u, v))
    uniq = []
    for s in sols:
        if not any(s[0] == t[0] and s[1] == t[1] for t in uniq):
            uniq.append(s)
    return uniq


def cell_edges(values):
    v = np.asarray(values, dtype=float)
    if len(v) == 1:
        return np.array([v[0] - 0.5, v[0] + 0.5])
    mids = 0.5 * (v[:-1] + v[1:])
    return np.concatenate(([v[0] - (mids[0] - v[0])], mids,
                           [v[-1] + (v[-1] - mids[-1])]))


def heatmaps(p1s, p3s, A, B, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    px = cell_edges(p1s)
    py = cell_edges(p3s)
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    for ax, M, title in ((axes[0], A, f"MM per-agent {stat}"),
                         (axes[1], B, f"CTA per-agent {stat}")):
        im = ax.pcolormesh(px, py, M.T, shading="flat", cmap="viridis")
        ax.set_xlim(px[0], px[-1])
        ax.set_ylim(py[0], py[-1])
        ax.set_xticks(p1s)
        ax.set_yticks(p3s)
        ax.set_xlabel("p1 (MM half-spread, ticks)")
        ax.set_ylabel("p3 (CTA entry, bps)")
        ax.set_title(title + " (x1000)")
        vmin, vmax = np.nanmin(M), np.nanmax(M)
        for i in range(len(p1s)):
            for j in range(len(p3s)):
                rel = (M[i, j] - vmin) / (vmax - vmin + 1e-9)
                ax.text(p1s[i], p3s[j], f"{M[i, j] / 1000:.1f}",
                        ha="center", va="center", fontsize=6, rotation=45,
                        color="w" if rel < 0.55 else "k")
        fig.colorbar(im, ax=ax)
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def plot3d(p1s, p3s, A, B, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d import Axes3D  # noqa: F401

    X, Y = np.meshgrid(p1s, p3s)
    base, ext = os.path.splitext(out)
    for name, M, cmap in (("mm", A, "viridis"), ("cta", B, "plasma")):
        fig = plt.figure(figsize=(11, 8))
        ax = fig.add_subplot(111, projection="3d")
        ax.plot_surface(X, Y, M.T, cmap=cmap, alpha=0.8, linewidth=0,
                        antialiased=True)
        ax.set_xlabel("p1 (MM half-spread)")
        ax.set_ylabel("p3 (CTA entry, bps)")
        ax.set_zlabel(f"{stat} per-agent payoff")
        ax.set_title(f"{name} per-agent {stat} payoff surface")
        ax.view_init(elev=25, azim=-60)
        path = f"{base}_{name}{ext}"
        fig.tight_layout()
        fig.savefig(path, dpi=150, bbox_inches="tight")
        print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("grid", help="run directory or f_p1_p2_p3.csv")
    ap.add_argument("--stat", choices=["mean", "median"], default="mean")
    ap.add_argument("--max-support", type=int, default=3)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    path, p1s, p3s, A, B = load(args.grid, args.stat)
    out_dir = args.out or os.path.dirname(os.path.abspath(path))
    os.makedirs(out_dir, exist_ok=True)
    print(f"loaded {path} stat={args.stat}")
    print("p1", p1s)
    print("p3", p3s)
    show("MM", p1s, p3s, A)
    show("CTA", p1s, p3s, B)
    best_responses(p1s, p3s, A, B)
    sols = mixed_nash(p1s, p3s, A, B, args.max_support)
    print(f"mixed Nash: {len(sols)}")
    for I, J, p, q, u, v in sols:
        print("  p1", [p1s[i] for i in I], "mix", np.round(p, 3),
              "p3", [p3s[j] for j in J], "mix", np.round(q, 3),
              f"payoff MM {u:.0f} CTA {v:.0f}")

    with open(os.path.join(out_dir, f"payoff_p1_p3_{args.stat}.csv"), "w",
              newline="") as f:
        w = csv.writer(f)
        w.writerow(["p1", "p3", "mm_per_agent", "cta_per_agent"])
        for i, p1 in enumerate(p1s):
            for j, p3 in enumerate(p3s):
                w.writerow([f"{p1:.6f}", f"{p3:.6f}",
                            f"{A[i, j]:.6f}", f"{B[i, j]:.6f}"])

    heatmaps(p1s, p3s, A, B, args.stat,
             os.path.join(out_dir, "payoff_heatmaps.png"))
    plot3d(p1s, p3s, A, B, args.stat,
           os.path.join(out_dir, "payoff_3d.png"))


if __name__ == "__main__":
    main()
