#!/usr/bin/env python3
"""Payoff matrix for a fixed-MM slice of the MM + arb + CTA grid.

The input is `f_p1_p2_p3.csv` from `mm_arb_cta_grid`.  With p1 fixed, the
players are arb (p2, rows) and CTA (p3, columns); MM's payoff is shown as the
environment but is not a choice variable in this slice.

Usage:
    ./venv/bin/python visualize/nash_p2p3.py build/runs/<run>
    ./venv/bin/python visualize/nash_p2p3.py <dir>/f_p1_p2_p3.csv --p1 2

Outputs:
    payoff_p2_p3.csv       per-agent MM/arb/CTA payoff matrix
    payoff_heatmaps.png    three heatmaps (MM, arb, CTA)
    payoff_3d.png          three payoff surfaces
"""

import argparse
import csv
import itertools
import os
import sys

import numpy as np


def load(path, p1, stat):
    if os.path.isdir(path):
        path = os.path.join(path, "f_p1_p2_p3.csv")
    rows = list(csv.DictReader(open(path, newline="")))
    rows = [r for r in rows if abs(float(r["p1"]) - p1) < 1e-9]
    if not rows:
        sys.exit(f"no rows with p1={p1} in {path}")

    mm_col = "mm_pnl" if stat == "mean" else "mm_pnl_med"
    arb_col = "arb_pnl" if stat == "mean" else "arb_pnl_med"
    cta_col = "cta_pnl" if stat == "mean" else "cta_pnl_med"

    p2s = sorted({float(r["p2"]) for r in rows})
    p3s = sorted({float(r["p3"]) for r in rows})
    A = np.full((len(p2s), len(p3s)), np.nan)
    B = np.full_like(A, np.nan)
    C = np.full_like(A, np.nan)
    for r in rows:
        i = p2s.index(float(r["p2"]))
        j = p3s.index(float(r["p3"]))
        A[i, j] = float(r[mm_col]) / (2 * int(r["n_mm"]))
        B[i, j] = float(r[arb_col]) / int(r["n_arb"])
        C[i, j] = float(r[cta_col]) / (2 * int(r["n_cta"]))
    return path, p2s, p3s, A, B, C


def show_matrix(name, p2s, p3s, M):
    print(f"\n{name} (per-agent, x1000)")
    print("p2\\p3 " + " ".join(f"{v:>7.0f}" for v in p3s))
    for i, p2 in enumerate(p2s):
        print(f"{p2:>5.0f} " + " ".join(f"{M[i, j] / 1000:>7.1f}"
                                        for j in range(len(p3s))))


def write_csv(path, p2s, p3s, A, B, C):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["p2", "p3", "mm_per_agent", "arb_per_agent", "cta_per_agent"])
        for i, p2 in enumerate(p2s):
            for j, p3 in enumerate(p3s):
                w.writerow([f"{p2:.6f}", f"{p3:.6f}",
                            f"{A[i, j]:.6f}", f"{B[i, j]:.6f}", f"{C[i, j]:.6f}"])
    print(f"wrote {path}")


def best_responses(p2s, p3s, B, C):
    br_arb = [(p3s[j], p2s[max(range(len(p2s)), key=lambda i: B[i, j])])
              for j in range(len(p3s))]
    br_cta = [(p2s[i], p3s[max(range(len(p3s)), key=lambda j: C[i, j])])
              for i in range(len(p2s))]
    print("\narb best response (p3 -> p2):", br_arb)
    print("CTA best response (p2 -> p3):", br_cta)

    pure = []
    for i, p2 in enumerate(p2s):
        j = max(range(len(p3s)), key=lambda k: C[i, k])
        i2 = max(range(len(p2s)), key=lambda k: B[k, j])
        if i2 == i:
            pure.append((p2, p3s[j], B[i, j], C[i, j]))
    print("pure Nash (arb p2, CTA p3):", pure if pure else "none")
    return pure


def mixed_nash(p2s, p3s, B, C, max_support=3, tol=1e-6):
    """Equal-size support enumeration for the arb (row) / CTA (column) game."""
    sols = []
    n1, n2 = B.shape
    for k in range(1, min(n1, n2, max_support) + 1):
        for I in itertools.combinations(range(n1), k):
            for J in itertools.combinations(range(n2), k):
                try:
                    M = np.zeros((k + 1, k + 1))
                    M[:k, :k] = B[np.ix_(I, J)]
                    M[:k, k] = -1
                    M[k, :k] = 1
                    rhs = np.array([0.0] * k + [1.0])
                    s = np.linalg.solve(M, rhs)
                    q, u = s[:k], s[k]

                    M2 = np.zeros((k + 1, k + 1))
                    M2[:k, :k] = C[np.ix_(I, J)].T
                    M2[:k, k] = -1
                    M2[k, :k] = 1
                    s2 = np.linalg.solve(M2, rhs)
                    p, v = s2[:k], s2[k]
                except np.linalg.LinAlgError:
                    continue

                if min(p) < -1e-8 or min(q) < -1e-8:
                    continue
                if any(B[r, list(J)] @ q > u + tol
                       for r in range(n1) if r not in I):
                    continue
                if any(p @ C[list(I), c] > v + tol
                       for c in range(n2) if c not in J):
                    continue
                sols.append((I, J, p, q, u, v))

    uniq = []
    for s in sols:
        if not any(s[0] == t[0] and s[1] == t[1] for t in uniq):
            uniq.append(s)
    return uniq


def print_mixed(sols, p2s, p3s):
    print(f"mixed Nash: {len(sols)}")
    for I, J, p, q, u, v in sols[:10]:
        rp = {p2s[i]: round(float(x), 4) for i, x in zip(I, p)}
        cq = {p3s[j]: round(float(x), 4) for j, x in zip(J, q)}
        print(f"  p2 {rp}  p3 {cq}  payoff arb={u:.2f} cta={v:.2f}")


def plot_heatmaps(p2s, p3s, A, B, C, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 3, figsize=(18, 6))
    for ax, M, title in ((axes[0], A, "MM"),
                         (axes[1], B, "arb"),
                         (axes[2], C, "CTA")):
        im = ax.imshow(M, origin="lower", aspect="auto",
                       extent=[p2s[0] - 0.5, p2s[-1] + 0.5,
                               p3s[0] - 2.5, p3s[-1] + 2.5],
                       cmap="viridis")
        ax.set_xticks(p2s)
        ax.set_yticks(p3s)
        ax.set_xlabel("p2 (arb edge threshold, ticks)")
        ax.set_ylabel("p3 (CTA entry threshold, bps)")
        ax.set_title(f"{title} per-agent {stat} (x1000)")
        vmin, vmax = np.nanmin(M), np.nanmax(M)
        for i in range(len(p2s)):
            for j in range(len(p3s)):
                rel = (M[i, j] - vmin) / (vmax - vmin + 1e-9)
                ax.text(p2s[i], p3s[j], f"{M[i, j] / 1000:.1f}",
                        ha="center", va="center", fontsize=5, rotation=45,
                        color="w" if rel < 0.55 else "k")
        fig.colorbar(im, ax=ax)
    fig.suptitle("Fixed p1=2: MM / arb / CTA payoff over p2 and p3")
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def plot3d(p2s, p3s, A, B, C, stat, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch
    from mpl_toolkits.mplot3d import Axes3D  # noqa: F401

    X, Y = np.meshgrid(p2s, p3s)
    fig = plt.figure(figsize=(18, 6))
    for k, (M, name, cmap) in enumerate(
            ((A, "MM", "viridis"), (B, "arb", "plasma"), (C, "CTA", "cividis"))):
        ax = fig.add_subplot(1, 3, k + 1, projection="3d")
        ax.plot_surface(X, Y, M.T, cmap=cmap, alpha=0.8, linewidth=0,
                        antialiased=True)
        ax.set_xlabel("p2 (arb edge)")
        ax.set_ylabel("p3 (CTA entry, bps)")
        ax.set_zlabel(f"per-agent {stat}")
        ax.set_title(f"{name} per-agent {stat}")
        ax.view_init(elev=25, azim=-60)
        ax.legend(handles=[Patch(facecolor=matplotlib.colormaps[cmap](0.6),
                                 alpha=0.8, label=name)], loc="upper left")
    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("grid", help="run directory, or f_p1_p2_p3.csv")
    ap.add_argument("--p1", type=float, default=2.0)
    ap.add_argument("--stat", choices=["mean", "median"], default="mean")
    ap.add_argument("--max-support", type=int, default=3)
    ap.add_argument("--out", default=None,
                    help="output prefix/directory (default: grid directory)")
    args = ap.parse_args()

    path, p2s, p3s, A, B, C = load(args.grid, args.p1, args.stat)
    out_dir = args.out or os.path.dirname(os.path.abspath(path))
    os.makedirs(out_dir, exist_ok=True)

    print(f"loaded {path}  p1={args.p1}  stat={args.stat}")
    print(f"p2 ({len(p2s)}): {p2s}")
    print(f"p3 ({len(p3s)}): {p3s}")

    show_matrix("MM", p2s, p3s, A)
    show_matrix("arb", p2s, p3s, B)
    show_matrix("CTA", p2s, p3s, C)

    write_csv(os.path.join(out_dir, f"payoff_p2_p3_{args.stat}.csv"),
              p2s, p3s, A, B, C)
    best_responses(p2s, p3s, B, C)
    sols = mixed_nash(p2s, p3s, B, C, args.max_support)
    print_mixed(sols, p2s, p3s)

    plot_heatmaps(p2s, p3s, A, B, C, args.stat,
                  os.path.join(out_dir, "payoff_heatmaps.png"))
    plot3d(p2s, p3s, A, B, C, args.stat,
           os.path.join(out_dir, "payoff_3d.png"))


if __name__ == "__main__":
    main()
