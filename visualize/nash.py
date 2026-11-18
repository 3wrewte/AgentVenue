#!/usr/bin/env python3
"""Per-agent payoff matrix, best responses and mixed Nash for a mm_arb_grid run.

    ./venv/bin/python visualize/nash.py build/nash-grid4

The MM class has 2*n_mm agents (n_mm per market); the arb class has n_arb.
Payoffs are per representative agent. The mixed Nash is found by support
enumeration on equally sized supports (pure equilibria included).
"""
import argparse
import csv
import itertools
import os
import sys

import numpy as np


def load(path, stat="mean"):
    if os.path.isdir(path):
        path = os.path.join(path, "f_p1_p2.csv")
    rows = list(csv.DictReader(open(path, newline="")))
    mm_col = "mm_pnl" if stat == "mean" else "mm_pnl_med"
    arb_col = "arb_pnl" if stat == "mean" else "arb_pnl_med"
    p1s = sorted({float(r["p1"]) for r in rows})
    p2s = sorted({float(r["p2"]) for r in rows})
    A = np.full((len(p1s), len(p2s)), np.nan)
    B = np.full_like(A, np.nan)
    for r in rows:
        i = p1s.index(float(r["p1"]))
        j = p2s.index(float(r["p2"]))
        A[i, j] = float(r[mm_col]) / (2 * int(r["n_mm"]))
        B[i, j] = float(r[arb_col]) / int(r["n_arb"])
    return p1s, p2s, A, B


def show(name, p1s, p2s, M):
    print(name)
    print("p1\\p2 " + " ".join(f"{b:>7.0f}" for b in p2s))
    for i, a in enumerate(p1s):
        print(f"{a:>5.1f} " + " ".join(f"{M[i, j]:>7.0f}" for j in range(len(p2s))))


def best_responses(p1s, p2s, A, B):
    br_mm = [(p2s[j], p1s[max(range(len(p1s)), key=lambda i: A[i, j])])
             for j in range(len(p2s))]
    br_arb = [(p1s[i], p2s[max(range(len(p2s)), key=lambda j: B[i, j])])
              for i in range(len(p1s))]
    print("MM best response (p2 -> p1):", br_mm)
    print("arb best response (p1 -> p2):", br_arb)
    pure = []
    for i, a in enumerate(p1s):
        jj = max(range(len(p2s)), key=lambda j: B[i, j])
        ii = max(range(len(p1s)), key=lambda k: A[k, jj])
        if ii == i:
            pure.append((a, p2s[jj]))
    print("pure Nash:", pure if pure else "none")


def mixed_nash(p1s, p2s, A, B, tol=1e-6):
    sols = []
    n1, n2 = A.shape
    for k in range(1, min(n1, n2) + 1):
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
    # dedupe
    uniq = []
    for s in sols:
        if not any(s[0] == t[0] and s[1] == t[1] for t in uniq):
            uniq.append(s)
    return uniq


def heatmaps(p1s, p2s, A, B, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    for ax, M, title in ((axes[0], A, f"MM per-agent {stat}"),
                         (axes[1], B, f"arb per-agent {stat}")):
        im = ax.imshow(M, origin="lower", aspect="auto",
                       extent=[p2s[0] - 0.25, p2s[-1] + 0.25,
                               p1s[0] - 0.25, p1s[-1] + 0.25],
                       cmap="viridis")
        ax.set_xticks(p2s)
        ax.set_yticks(p1s)
        ax.set_xlabel("p2 (arb edge threshold)")
        ax.set_ylabel("p1 (MM half-spread)")
        ax.set_title(title)
        vmin, vmax = np.nanmin(M), np.nanmax(M)
        for i in range(len(p1s)):
            for j in range(len(p2s)):
                if np.isnan(M[i, j]):
                    continue
                rel = (M[i, j] - vmin) / (vmax - vmin + 1e-9)
                ax.text(p2s[j], p1s[i], f"{M[i, j]:.0f}",
                        ha="center", va="center", fontsize=6,
                        color="w" if rel < 0.55 else "k")
        fig.colorbar(im, ax=ax)
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def plot3d(p1s, p2s, A, B, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    from mpl_toolkits.mplot3d import Axes3D  # noqa: F401

    X, Y = np.meshgrid(p2s, p1s)
    fig = plt.figure(figsize=(12, 8))
    ax = fig.add_subplot(111, projection="3d")
    ax.plot_surface(X, Y, A, cmap="viridis", alpha=0.75, linewidth=0,
                    antialiased=True)
    ax.plot_surface(X, Y, B, cmap="plasma", alpha=0.60, linewidth=0,
                    antialiased=True)
    ax.set_xlabel("p2 (arb edge threshold)")
    ax.set_ylabel("p1 (MM half-spread)")
    ax.set_zlabel(f"{stat} per-agent payoff")
    ax.set_title(f"MM and arb per-agent {stat} payoff surfaces")
    ax.view_init(elev=25, azim=-60)
    handles = [Patch(facecolor=plt.cm.viridis(0.6), alpha=0.75, label="MM"),
               Patch(facecolor=plt.cm.plasma(0.6), alpha=0.60, label="arb")]
    ax.legend(handles=handles, loc="upper left")
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("grid", help="grid output dir or f_p1_p2.csv")
    ap.add_argument("--stat", choices=["mean", "median"], default="mean")
    ap.add_argument("--heatmap", default=None,
                    help="output PNG path (default: <grid>/nash_heatmap.png)")
    ap.add_argument("--3d", dest="plot3d", default=None,
                    help="3D surface PNG path (default: <grid>/nash_3d.png)")
    args = ap.parse_args()
    p1s, p2s, A, B = load(args.grid, args.stat)
    mm_col = "mm_pnl" if args.stat == "mean" else "mm_pnl_med"
    print(f"stat = {args.stat} (column {mm_col})")
    show("MM per-agent payoff", p1s, p2s, A)
    show("arb per-agent payoff", p1s, p2s, B)
    best_responses(p1s, p2s, A, B)
    sols = mixed_nash(p1s, p2s, A, B)
    print(f"mixed Nash: {len(sols)}")
    for I, J, p, q, u, v in sols:
        print("  MM support p1", [p1s[i] for i in I], "mix", np.round(p, 3),
              "arb support p2", [p2s[j] for j in J], "mix", np.round(q, 3),
              f"payoff MM {u:.0f} arb {v:.0f}")

    out = args.heatmap
    if out is None:
        out = (os.path.join(args.grid, "nash_heatmap.png")
               if os.path.isdir(args.grid) else "nash_heatmap.png")
    heatmaps(p1s, p2s, A, B, args.stat, out)

    out3d = args.plot3d
    if out3d is None:
        out3d = (os.path.join(args.grid, "nash_3d.png")
                 if os.path.isdir(args.grid) else "nash_3d.png")
    plot3d(p1s, p2s, A, B, args.stat, out3d)


if __name__ == "__main__":
    main()
