#!/usr/bin/env python3
"""Visualise an AgentVenue recorded run.

Usage:
    ./venv/bin/python visualize/plot_sim.py build/viz
    ./venv/bin/python visualize/plot_sim.py build/viz/run_... --agent 20 --per-class 3
    ./venv/bin/python visualize/plot_sim.py build/viz --arb-agent 50

Outputs (in the run directory):
    sim_plot.png : PnL paths by class + both markets' best bid/ask + one
                   selected trader's orders.
    sim_arb.png  : a selected cross-market arbitrageur, with venue 0 and
                   venue 1 order panels, the mid basis, the positions and the
                   per-venue / common-mark PnL.

The run must contain the CSVs written by SimRecorder (market.csv, agents.csv,
agent_orders.csv, replies.csv, fills.csv).  The fills/account/agent columns
are required for the order-lifetime panels.
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import to_rgb
import numpy as np

CLASS_COLOR = {0: "0.45", 1: "tab:red", 2: "tab:green", 3: "tab:purple"}
CLASS_CMAP = {1: "Reds", 2: "Greens", 3: "Purples"}
CLASS_NAME = {0: "noise", 1: "MM", 2: "arb", 3: "CTA"}
VENUE_COLOR = {0: "tab:blue", 1: "tab:orange"}
LINESTYLES = ["-", "--", ":", "-."]
BUY_COLOR = "tab:green"
SELL_COLOR = "tab:red"


def shade(color, f):
    """Blend `color` towards white; f=1 is the color, f=0 is white."""
    r, g, b = to_rgb(color)
    return (1.0 - f + f * r, 1.0 - f + f * g, 1.0 - f + f * b)


def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def resolve_run(path):
    if os.path.isfile(os.path.join(path, "agents.csv")):
        return path
    runs = [
        os.path.join(path, d)
        for d in sorted(os.listdir(path))
        if d.startswith("run_") and os.path.isdir(os.path.join(path, d))
    ]
    if len(runs) == 1:
        return runs[0]
    if not runs:
        sys.exit(f"no run_* directory under {path}")
    sys.exit("multiple runs under %s; pass one explicitly:\n  %s" %
             (path, "\n  ".join(runs)))


def load_market(rows):
    market = defaultdict(dict)
    last_tick = 0
    for r in rows:
        t = int(r["tick"])
        v = int(r["venue"])
        last_tick = max(last_tick, t)
        bid = int(r["bid"])
        ask = int(r["ask"])
        if bid and ask:
            market[v][t] = (bid, ask)
    return market, last_tick


def mid_series(ticks, market):
    """Last-known mid per venue at each tick (None before a two-sided book)."""
    out = {}
    last = {}
    for t in ticks:
        for v in (0, 1):
            pt = market.get(v, {}).get(t)
            if pt:
                last[v] = 0.5 * (pt[0] + pt[1])
        out[t] = (last.get(0), last.get(1))
    return out


def load_pnl(rows, market):
    """Equity at the current market mid: cash + position * mid, summed venues.

    The agents.csv `unrealized` uses the delayed view; using the mid from
    market.csv at the same tick gives a smoother mark-to-market path.
    """
    pnl = defaultdict(dict)
    class_of = {}
    last_mid = {}
    for r in rows:
        a = int(r["agent"])
        t = int(r["tick"])
        v = int(r["venue"])
        class_of[a] = int(r["class"])
        pt = market.get(v, {}).get(t)
        if pt:
            last_mid[v] = 0.5 * (pt[0] + pt[1])
        mid = last_mid.get(v)
        if mid is None:
            # No two-sided book yet: value the inventory at its average cost,
            # which reduces to the realized PnL and avoids huge mark jumps.
            mid = float(r["avg_cost"])
        val = float(r["cash"]) + float(r["position"]) * mid
        pnl[a][t] = pnl[a].get(t, 0.0) + val
    return pnl, class_of


def pick_agents(pnl, class_of, per_class, classes=(1, 2, 3)):
    if not pnl:
        return []
    last = max(max(v) for v in pnl.values())
    chosen = []
    for c in sorted(set(class_of.values())):
        if c not in classes:
            continue
        ids = [a for a in pnl if class_of[a] == c and pnl[a]]
        if not ids:
            continue
        ids.sort(key=lambda a: pnl[a].get(last, 0.0))
        if len(ids) <= per_class:
            pick = ids
        else:
            idx = np.linspace(0, len(ids) - 1, per_class).round().astype(int)
            pick = [ids[i] for i in sorted(set(idx))]
        chosen.extend((a, c) for a in pick)
    return chosen


def choose_agent(orders, class_of, classes=(1, 2, 3)):
    counts = defaultdict(int)
    for r in orders:
        a = int(r["agent"])
        if class_of.get(a, 0) in classes and r["kind"] == "limit":
            counts[a] += 1
    if counts:
        return max(counts, key=counts.get)
    for r in orders:
        a = int(r["agent"])
        if class_of.get(a, 0) in classes:
            return a
    return None


def load_agent_data(agent, orders, fills, replies):
    my_orders = [r for r in orders if int(r["agent"]) == agent]
    my_fills = [r for r in fills if int(r["agent"]) == agent]
    my_replies = [r for r in replies if int(r["agent"]) == agent]

    fills_by = defaultdict(list)
    for f in my_fills:
        fills_by[(int(f["venue"]), int(f["client_id"]))].append(f)

    reply_for = {}
    cancel_reply = {}
    for r in my_replies:
        key = (int(r["venue"]), int(r["client_id"]))
        if r["kind"] == "cancel":
            cancel_reply[key] = r
        elif key not in reply_for:
            reply_for[key] = r

    cancels_by = defaultdict(list)
    for o in my_orders:
        if o["kind"] == "cancel":
            cancels_by[(int(o["venue"]), int(o["client_id"]))].append(o)

    return my_orders, fills_by, reply_for, cancel_reply, cancels_by


def order_end(o, fills_by, reply_for, cancel_reply, last_tick):
    v = int(o["venue"])
    cid = int(o["client_id"])
    qty = int(o["qty"])
    start = int(o["tick"])
    end = None
    reason = "open"

    reply = reply_for.get((v, cid))
    if reply and reply["reject"] != "none":
        return start, start, "rejected"

    cum = 0
    for f in sorted(fills_by.get((v, cid), []), key=lambda x: int(x["tick"])):
        cum += int(f["qty"])
        if cum >= qty:
            end = int(f["tick"])
            reason = "filled"
            break

    cr = cancel_reply.get((v, cid))
    if cr and cr["cancel_found"] == "1":
        cand = int(cr["tick"])
        if end is None or cand < end:
            end = cand
            reason = "cancelled"

    if end is None:
        end = last_tick
    if end < start:
        end = start
    return start, end, reason


def draw_orders(ax, orders, fills_by, reply_for, cancel_reply,
                last_tick, market, venues, title, shade_market=False):
    """Draw limit lifetimes, fills and cancels for `orders`."""
    for v in venues:
        pts = market.get(v, {})
        if not pts:
            continue
        ts = sorted(pts)
        color = VENUE_COLOR.get(v, "0.5")
        if shade_market:
            bid_color = to_rgb(color)
            ask_color = shade(color, 0.5)
            ax.plot(ts, [pts[t][0] for t in ts], color=bid_color,
                    linewidth=1.0, alpha=0.45, zorder=1)
            ax.plot(ts, [pts[t][1] for t in ts], color=ask_color,
                    linewidth=1.0, alpha=0.45, zorder=1)
        else:
            ax.plot(ts, [pts[t][0] for t in ts], color=color,
                    linewidth=0.8, alpha=0.35, zorder=1)
            ax.plot(ts, [pts[t][1] for t in ts], color=color,
                    linewidth=0.8, alpha=0.35, linestyle="--", zorder=1)

    n_limit = n_market = n_cancel = n_open = n_reject = 0
    for o in orders:
        kind = o["kind"]
        side = o["side"]
        price = int(o["price"])
        tick = int(o["tick"])
        venue = int(o["venue"])
        cid = int(o["client_id"])
        color = BUY_COLOR if side == "B" else SELL_COLOR

        if kind == "cancel":
            n_cancel += 1
            target = None
            for oo in orders:
                if (oo["kind"] == "limit" and
                        int(oo["client_id"]) == cid and
                        int(oo["venue"]) == venue and
                        int(oo["tick"]) <= tick):
                    target = oo
            y = int(target["price"]) if target else price
            ax.scatter([tick], [y], marker="x", s=35, color="k", zorder=6)
            continue

        if kind == "market":
            n_market += 1
            f = fills_by.get((venue, cid), [])
            if f:
                ff = min(f, key=lambda x: int(x["tick"]))
                ax.scatter([int(ff["tick"])], [int(ff["price"])],
                           marker="^", s=35, color=color, zorder=5)
            else:
                ax.scatter([tick], [price], marker="^", s=35,
                           color=color, zorder=5)
            continue

        start, end, reason = order_end(
            o, fills_by, reply_for, cancel_reply, last_tick)
        if reason == "rejected":
            n_reject += 1
            ax.scatter([start], [price], marker="x", s=40,
                       color="k", zorder=6)
            continue

        n_limit += 1
        if reason == "open":
            n_open += 1
        ax.plot([start, end], [price, price], color=color,
                linewidth=2.0, alpha=0.75,
                linestyle=":" if reason == "open" else "-", zorder=4)

        for ff in fills_by.get((venue, cid), []):
            maker = ff["maker"] == "1"
            ax.scatter([int(ff["tick"])], [int(ff["price"])],
                       marker="o" if maker else "^",
                       s=18, color=color, alpha=0.85, zorder=5)
        if reason == "cancelled":
            cr = cancel_reply.get((venue, cid))
            ct = int(cr["tick"]) if cr else end
            ax.scatter([ct], [price], marker="x", s=35,
                       color="k", zorder=6)

    ax.set_title(title)
    ax.set_ylabel("price")
    ax.grid(alpha=0.25)
    handles = [
        plt.Line2D([], [], color=BUY_COLOR, lw=2, label="limit buy"),
        plt.Line2D([], [], color=SELL_COLOR, lw=2, label="limit sell"),
        plt.Line2D([], [], color="0.5", lw=2, linestyle=":",
                   label="open at end"),
        plt.Line2D([], [], marker="o", color="0.3", linestyle="None",
                   label="maker fill"),
        plt.Line2D([], [], marker="^", color="0.3", linestyle="None",
                   label="taker fill"),
        plt.Line2D([], [], marker="x", color="k", linestyle="None",
                   label="cancel / reject"),
    ]
    ax.legend(handles=handles, fontsize=7, ncol=3, loc="best")
    return n_limit, n_market, n_cancel, n_open, n_reject


def load_agent_tracks(agent, agents, market):
    """Per-venue (cash, position, realized, avg_cost, checker base, checker cash)."""
    per = {0: {}, 1: {}}
    ticks = set()
    for r in agents:
        if int(r["agent"]) != agent:
            continue
        v = int(r["venue"])
        t = int(r["tick"])
        ticks.add(t)
        per.setdefault(v, {})[t] = (
            float(r["cash"]), float(r["position"]),
            float(r["realized"]), float(r["avg_cost"]),
            float(r.get("acct_base", 0)), float(r.get("acct_cash", 0)))
    return per, sorted(ticks)


def make_arb_figure(run, agent, class_of, agents, market, last_tick,
                    orders, fills, replies, transfers, out):
    my_orders, fills_by, reply_for, cancel_reply, cancels_by = load_agent_data(
        agent, orders, fills, replies)
    per, ticks = load_agent_tracks(agent, agents, market)
    if not ticks:
        return

    by_tick = defaultdict(set)
    for o in my_orders:
        if o["kind"] != "cancel":
            by_tick[int(o["tick"])].add(int(o["venue"]))
    pairs = sorted(t for t, vs in by_tick.items() if 0 in vs and 1 in vs)
    settle = [t for t in transfers
              if int(t["agent"]) == agent and t["ok"] == "1"]
    settle_ticks = [int(t["tick"]) for t in settle]

    # mids / basis / positions / PnL tracks
    mids = mid_series(ticks, market)
    basis = []
    pos0, pos1, net = [], [], []
    p0s, p1s, totals, commons = [], [], [], []
    localb0, localb1, localc0, localc1 = [], [], [], []
    for t in ticks:
        m0, m1 = mids[t]
        r0 = per.get(0, {}).get(t)
        r1 = per.get(1, {}).get(t)
        mid0 = m0 if m0 is not None else (r0[3] if r0 else None)
        mid1 = m1 if m1 is not None else (r1[3] if r1 else None)
        basis.append((m0 - m1) if (m0 is not None and m1 is not None)
                     else np.nan)
        a0 = r0[1] if r0 else 0
        a1 = r1[1] if r1 else 0
        pos0.append(a0)
        pos1.append(a1)
        net.append(a0 + a1)
        p0 = r0[0] + a0 * mid0 if (r0 and mid0 is not None) else 0.0
        p1 = r1[0] + a1 * mid1 if (r1 and mid1 is not None) else 0.0
        p0s.append(p0)
        p1s.append(p1)
        totals.append(p0 + p1)
        common = 0.0
        if r0 is not None:
            common += r0[0] + a0 * (mid1 if mid1 is not None else mid0)
        if r1 is not None:
            common += r1[0] + a1 * (mid1 if mid1 is not None else mid0)
        commons.append(common)
        localb0.append(r0[4] if r0 else 0)
        localb1.append(r1[4] if r1 else 0)
        localc0.append(r0[5] if r0 else 0)
        localc1.append(r1[5] if r1 else 0)

    fig, axes = plt.subplots(5, 1, figsize=(15, 19), sharex=True)
    ax0, ax1, axb, axl, axp = axes

    for ax, v in ((ax0, 0), (ax1, 1)):
        for t in pairs:
            ax.axvline(t, color="0.9", linewidth=0.6, zorder=0)
        for t in settle_ticks:
            ax.axvline(t, color="tab:cyan", linewidth=0.7, alpha=0.3, zorder=0)
        sub = [o for o in my_orders if int(o["venue"]) == v]
        title = f"arb a{agent} (class {class_of.get(agent, '?')}) — venue {v}"
        draw_orders(ax, sub, fills_by, reply_for, cancel_reply,
                    last_tick, market, [v], title)

    for t in settle_ticks:
        axb.axvline(t, color="tab:cyan", linewidth=0.7, alpha=0.3, zorder=0)
    axb.set_title(f"basis mid(v0) - mid(v1) and inventory "
                  f"({len(pairs)} two-venue order ticks, "
                  f"{len(settle)} settlements)")
    axb.plot(ticks, basis, color="k", linewidth=1.4, label="mid0-mid1")
    axb.axhline(0.0, color="0.7", linewidth=0.8)
    axb.set_ylabel("basis (ticks)")
    axb.grid(alpha=0.25)
    axb2 = axb.twinx()
    axb2.plot(ticks, pos0, color=VENUE_COLOR[0], linewidth=1.2,
              label="pos v0")
    axb2.plot(ticks, pos1, color=VENUE_COLOR[1], linewidth=1.2,
              label="pos v1")
    axb2.plot(ticks, net, color="0.4", linewidth=1.0, linestyle="--",
              label="net")
    axb2.set_ylabel("inventory")
    h1, l1 = axb.get_legend_handles_labels()
    h2, l2 = axb2.get_legend_handles_labels()
    axb.legend(h1 + h2, l1 + l2, fontsize=7, ncol=4, loc="best")

    axl.set_title(f"checker balances (local inventory) — "
                  f"{len(settle)} settled, last q="
                  f"{settle[-1]['qty'] if settle else 0}")
    axl.plot(ticks, localb0, color=VENUE_COLOR[0], linewidth=1.3,
             label="base v0")
    axl.plot(ticks, localb1, color=VENUE_COLOR[1], linewidth=1.3,
             label="base v1")
    axl.set_ylabel("checker base")
    axl.grid(alpha=0.25)
    axl2 = axl.twinx()
    axl2.plot(ticks, localc0, color=VENUE_COLOR[0], linewidth=1.0,
              linestyle="--", alpha=0.6, label="cash v0")
    axl2.plot(ticks, localc1, color=VENUE_COLOR[1], linewidth=1.0,
              linestyle="--", alpha=0.6, label="cash v1")
    axl2.set_ylabel("checker cash")
    h1, l1 = axl.get_legend_handles_labels()
    h2, l2 = axl2.get_legend_handles_labels()
    axl.legend(h1 + h2, l1 + l2, fontsize=7, ncol=4, loc="best")

    for t in pairs:
        axp.axvline(t, color="0.9", linewidth=0.6, zorder=0)
    for t in settle_ticks:
        axp.axvline(t, color="tab:cyan", linewidth=0.7, alpha=0.3, zorder=0)
    axp.plot(ticks, p0s, color=VENUE_COLOR[0], linewidth=1.2,
             label="pnl v0 (own mid)")
    axp.plot(ticks, p1s, color=VENUE_COLOR[1], linewidth=1.2,
             label="pnl v1 (own mid)")
    axp.plot(ticks, totals, color="k", linewidth=1.5, label="total")
    axp.plot(ticks, commons, color="k", linewidth=1.2, linestyle="--",
             label="common mark (v1 mid)")
    axp.axhline(0.0, color="0.7", linewidth=0.8)
    axp.set_xlabel("agent tick")
    axp.set_ylabel("PnL")
    axp.set_title(f"arb a{agent} PnL: per-venue vs common mark")
    axp.grid(alpha=0.25)
    axp.legend(fontsize=7, ncol=4, loc="best")

    fig.tight_layout()
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run", help="run_* directory (or its parent)")
    ap.add_argument("--per-class", type=int, default=5,
                    help="how many agents per class in the PnL image")
    ap.add_argument("--mm-agent", type=int, default=None,
                    help="MM agent for trader_mm.png (default: most limits)")
    ap.add_argument("--arb-agent", type=int, default=None,
                    help="arb agent for trader_arb.png and sim_arb.png")
    ap.add_argument("--cta-agent", type=int, default=None,
                    help="CTA agent for trader_cta.png (default: most limits)")
    ap.add_argument("--out", default=None,
                    help="output directory (default: the run directory)")
    ap.add_argument("--arb-out", default=None)
    ap.add_argument("--no-arb", action="store_true",
                    help="skip the arbitrageur figure")
    args = ap.parse_args()

    run = resolve_run(args.run)
    out_dir = args.out or run
    os.makedirs(out_dir, exist_ok=True)

    agents = read_csv(os.path.join(run, "agents.csv"))
    market_rows = read_csv(os.path.join(run, "market.csv"))
    orders = read_csv(os.path.join(run, "agent_orders.csv"))
    replies_path = os.path.join(run, "replies.csv")
    fills_path = os.path.join(run, "fills.csv")
    if not os.path.exists(fills_path):
        sys.exit("fills.csv not found: re-record with the updated recorder")
    replies = read_csv(replies_path)
    fills = read_csv(fills_path)
    transfers_path = os.path.join(run, "transfers.csv")
    transfers = read_csv(transfers_path) if os.path.exists(transfers_path) else []

    market, last_tick = load_market(market_rows)
    pnl, class_of = load_pnl(agents, market)

    # ---- 1. PnL paths by strategic class (no noise) ----------------------
    chosen = pick_agents(pnl, class_of, args.per_class, classes=(1, 2, 3))
    by_class = defaultdict(list)
    for a, c in chosen:
        by_class[c].append(a)
    fig, ax = plt.subplots(figsize=(13, 6))
    for c in sorted(by_class):
        ids = sorted(by_class[c], key=lambda a: pnl[a].get(last_tick, 0.0))
        cmap = matplotlib.colormaps[CLASS_CMAP[c]]
        n = len(ids)
        for k, a in enumerate(ids):
            frac = k / (n - 1) if n > 1 else 0.6
            color = cmap(0.45 + 0.55 * frac)
            ts = sorted(pnl[a])
            ys = [pnl[a][t] for t in ts]
            ax.plot(ts, ys, color=color, linewidth=1.6, alpha=0.95,
                    label=f"a{a} {CLASS_NAME[c]}")
    ax.axhline(0.0, color="0.7", linewidth=0.8)
    ax.set_xlabel("agent tick")
    ax.set_ylabel("PnL (cash + position x market mid)")
    ax.set_title(f"PnL by class  ({run})")
    ax.grid(alpha=0.25)
    ax.legend(fontsize=7, ncol=3, loc="best")
    fig.tight_layout()
    path = os.path.join(out_dir, "pnl_by_class.png")
    fig.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path}")

    # ---- 2. market best bid / ask (shades, solid) -------------------------
    fig, ax = plt.subplots(figsize=(13, 5))
    for v, color in VENUE_COLOR.items():
        pts = market.get(v, {})
        if not pts:
            continue
        ts = sorted(pts)
        ax.plot(ts, [pts[t][0] for t in ts], color=to_rgb(color),
                linewidth=1.5, label=f"v{v} bid")
        ax.plot(ts, [pts[t][1] for t in ts], color=shade(color, 0.5),
                linewidth=1.5, label=f"v{v} ask")
    ax.set_xlabel("agent tick")
    ax.set_ylabel("price")
    ax.set_title("market best bid / ask (same market -> same colour, "
                 "bid dark / ask light)")
    ax.grid(alpha=0.25)
    ax.legend(fontsize=8, ncol=2, loc="best")
    fig.tight_layout()
    path = os.path.join(out_dir, "market_bidask.png")
    fig.savefig(path, dpi=150, bbox_inches="tight")
    print(f"wrote {path}")

    # ---- 3. one trader of each class -------------------------------------
    picks = [(1, "mm", args.mm_agent), (2, "arb", args.arb_agent),
             (3, "cta", args.cta_agent)]
    for c, name, forced in picks:
        agent = forced if forced is not None else choose_agent(
            orders, class_of, classes=(c,))
        if agent is None:
            print(f"no {name} class in this run; skipped trader_{name}.png")
            continue
        my_orders, fills_by, reply_for, cancel_reply, cancels_by = \
            load_agent_data(agent, orders, fills, replies)
        used_venues = sorted({int(o["venue"]) for o in my_orders})
        fig, ax = plt.subplots(figsize=(15, 6))
        n_limit, n_market, n_cancel, n_open, n_reject = draw_orders(
            ax, my_orders, fills_by, reply_for, cancel_reply,
            last_tick, market, used_venues,
            f"trader a{agent} ({CLASS_NAME.get(c, c)})", shade_market=True)
        ax.set_xlabel("agent tick")
        ax.set_title(
            f"trader a{agent} ({CLASS_NAME.get(c, c)}): "
            f"{n_limit} limit lines, {n_market} market, {n_cancel} cancels, "
            f"{n_reject} rejected, {n_open} open at end")
        fig.tight_layout()
        path = os.path.join(out_dir, f"trader_{name}.png")
        fig.savefig(path, dpi=150, bbox_inches="tight")
        print(f"wrote {path}")

    # ---- 4. arbitrageur detail (unchanged) -------------------------------
    if not args.no_arb:
        arb = args.arb_agent
        if arb is None:
            arb = choose_agent(orders, class_of, classes=(2,))
        if arb is not None:
            arb_out = args.arb_out or os.path.join(out_dir, "sim_arb.png")
            make_arb_figure(run, arb, class_of, agents, market, last_tick,
                            orders, fills, replies, transfers, arb_out)
        else:
            print("no class-2 arbitrageur in this run; skipped sim_arb.png")


if __name__ == "__main__":
    main()
