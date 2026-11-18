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


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "f_p1_p2.csv")
    rows = list(csv.DictReader(open(path, newline="")))
    p1s = sorted({float(r["p1"]) for r in rows})
    p2s = sorted({float(r["p2"]) for r in rows})
    A = np.zeros((len(p1s), len(p2s)))
    B = np.zeros_like(A)
    for r in rows:
        i = p1s.index(float(r["p1"]))
        j = p2s.index(float(r["p2"]))
        A[i, j] = float(r["mm_pnl"]) / (2 * int(r["n_mm"]))
        B[i, j] = float(r["arb_pnl"]) / int(r["n_arb"])
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("grid", help="grid output dir or f_p1_p2.csv")
    args = ap.parse_args()
    p1s, p2s, A, B = load(args.grid)
    show("MM per-agent payoff", p1s, p2s, A)
    show("arb per-agent payoff", p1s, p2s, B)
    best_responses(p1s, p2s, A, B)
    sols = mixed_nash(p1s, p2s, A, B)
    print(f"mixed Nash: {len(sols)}")
    for I, J, p, q, u, v in sols:
        print("  MM support p1", [p1s[i] for i in I], "mix", np.round(p, 3),
              "arb support p2", [p2s[j] for j in J], "mix", np.round(q, 3),
              f"payoff MM {u:.0f} arb {v:.0f}")


if __name__ == "__main__":
    main()
