# AgentVenue

基于撮合核心 `OoOobookCPP` 的多智能体（agent-based）仿真框架。目标是用
RTL 风格的 agent 读取（带延迟的）市场数据、提交订单，在同一个撮合核心上
做博弈/微观结构实验。

当前第一版包含：

- 完整的 agent 生命周期框架（注册、每 tick 多线程轮询、prepare/comb/commit）。
- 真实撮合 venue（封装 `OoOobookCPP` 的 `core_top`）。
- 合成参考市场 venue（固定 bid/ask、无限流动性、下一 tick 成交）。
- 延迟市场数据 + 提前一周期计算的 signal 模块。
- 成交/挂单/组合（现货式）的路由与记录。
- 随机交易者样例（泊松下单/撤单，高斯定价）。
- doctest 单元测试。

`OoOobookCPP` 是 git submodule（只读，改动请回核心仓库改）。`libs/` 存放
本地 doctest（已 gitignore）。

---

## 1. 仓库结构

```text
AgentVenue/
  OoOobookCPP/                  # submodule：撮合核心（OP -> mOP -> uOP）
  libs/doctest/doctest.h        # 本地测试依赖（gitignore）
  agents/
    agent_types.hpp             # 市场/信号/组合/私有视图/订单 等 wire 记录
    agent_base.hpp              # AgentBase（prepare/comb/commit）
    signal_base.hpp             # SignalModule
    signals/standard.hpp        # mid / spread
    strategies/random_trader.hpp
  runner/
    delay_line.hpp              # MarketView 固定延迟环
    order_source.hpp            # 与 core 握手的 OP FIFO（同 InstructionsFromCSV）
    venue.hpp                   # RealVenue + SyntheticVenue + build_venue_view
    fill_router.hpp             # canonical events -> fills/updates/open orders/portfolio
    portfolio.hpp               # 现货式 position/cash/avg/realized
    agent_runner.hpp            # 注册表 + parallel_for + 三阶段
    sim_recorder.hpp            # CSV 记录（复用 OutputStreamToCSV）
    sim_engine.hpp              # tick 时间线
    sim_main.cpp                # CLI
  test/                         # doctest 单元测试
  Makefile
```

---

## 2. 总体架构

```text
                 ┌────────────────────────────────────────────┐
   agent tick T  │ SimEngine                                  │
                 │                                            │
   high freq ───►│  RealVenue(core_top + IDallocator + CSV)   │
   (core sub-    │     ▲  AgentOrderSource (OP FIFO handshake) │
    ticks)       │     │                                      │
                 │  SyntheticVenue (固定 ref bid/ask)          │
                 │                                            │
   switch  ─────►│  build MarketView -> DelayLine              │
                 │  SignalModule 读 delay.at(T+1-D)            │
                 │  给 agents 填 mkt_in / signals_in /         │
                 │  priv_in / portfolio_in                     │
                 │                                            │
   low freq  ───►│  AgentRunner: prepare -> comb -> commit     │
   (agents +     │     每个 agent 写 out_next，commit 后       │
    signals)     │     out_committed                           │
                 │                                            │
   gearshift ───►│  收集 out_committed -> OPcontext ->         │
                 │  IDallocator::allocate -> venue source      │
                 └────────────────────────────────────────────┘
```

职责划分：

- **OoOobookCPP core**：唯一的撮合/订单簿真相。header-only；`core_top::comb`
  一次处理一个 OP，产出 `CanonicalEvent` 流。
- **RealVenue**：`core_top` + 每个 venue 的 `IDallocator`（internal id/context、
  `(account, client) -> internal id`）+ `AgentOrderSource` + 成交流水 CSV。
- **SyntheticVenue**：参考市场，无 core。固定 `bid/ask`，无限流动性，
  limit 必须 cross，否则 reject；订单在下一 agent tick 成交。
- **AgentBase**：RTL 风格。输入是框架驱动的 wire（`mkt_in`/`signals_in`/
  `priv_in`/`portfolio_in`）；输出是寄存器（`out_next` 在 comb 写，
  `out_committed` 在 commit 后可见）。
- **SignalModule**：和 agent 同 phase 运行，但读的是“agents 下一周期将看到的”
  MarketView，结果 reg 一周期后与同一 view 一起交付。
- **FillRouter**：把 canonical events 翻译成每个 agent 的成交、挂单状态、
  持仓/现金/PnL，并维护框架侧 open-order 表。
- **AgentRunner**：注册表 + `parallel_for`（默认单线程；`threads>1` 时按
  phase 分块并行，join 即 phase barrier）。
- **SimEngine**：高低频、gearshift、延迟、记录。

---

## 3. Tick 时序（重要）

设 `D = market_delay >= 1`。每个 agent tick T：

1. **高倍频（main 单线程）**：用 `AgentOrderSource` 与 core 握手，反复
   `ops = source.issue(core.op_queue.ready_num()); core.comb(ops.size(), ops);`
   直到 source/ROB 排空（或到 `substep_max`）。每个 OP 产出的
   `CanonicalEvent` 立即被 `FillRouter` 消费。
2. 合成参考市场 `ref.step()`：成交上一 tick 排队到 ref 的订单，产出 fills。
3. **低频切换**：
   - `build_venue_view(real.idvol, ...)` + `ref.view()` 组成 `MarketView(T)`，
     发布进 `DelayLine`。
   - signal 模块读 `delay.at(T + 1 - D)`（即 agents **下个周期**会看到的 view），
     写入 `signals_next`；本 tick 结束时 commit 成为 `signals_cur`。
   - 给每个 agent 填：
     - `mkt_in = delay.at(T - D)`
     - `signals_in = signals_cur`（上一周期算的、与 `mkt_in` 对齐的信号）
     - `priv_in` / `portfolio_in`（本 tick 路由出的成交/挂单/组合）
4. **低频三相**：`prepare -> comb -> commit`（agents 和 signal 模块在同一个
   并行阶段；phase 边界 join 同步）。
5. **Gearshift**：按 agent 下标顺序收 `out_committed`，校验、构造
   `OPcontext`、`IDallocator::allocate` 得到 `OP`，推入对应 venue 的 source
   （ref 单推入合成 venue 的 pending）。

关键时间语义：

- agent 在 T 写的单，最早在 **T+1 的高倍频段** 被 core 读取/撮合；core 内部
  的 pipeline 延迟不额外模拟。
- 由此得到 **1 个 agent tick 的 order/fill reg-out 延迟**，和“市场数据延迟 D”
  一起避免了“看到当前盘口立刻抢单”。
- signal 在 T 读 `delay.at(T+1-D)`、reg 一周期，所以 T+1 时 agent 的
  `signals_in` 与 `mkt_in = delay.at(T+1-D)` 来自同一个 view；signal 计算
  和 agent comb 之间没有先后依赖。

---

## 4. 接口速查

所有类型在 `agents/agent_types.hpp`。

### 4.1 市场（框架 -> agent 的 wire）

```cpp
struct LevelView { Price_t price; QtySum_t qty; };        // price==0 表示空

struct VenueView {
    Sym_t sym;
    Price_t bid_price, ask_price;                          // 0 = 空侧
    QtySum_t bid_qty, ask_qty;
    Price_t last_price; Qty_t last_qty;                    // 最近成交
    uint64_t trade_count; QtySum_t total_volume;           // 累计
    std::array<LevelView, DEPTH> bids, asks;               // [0]=best
};

struct MarketView {
    uint64_t tick;
    bool valid;
    std::array<VenueView, MAX_VENUES> venues;
};
```

当前 `MAX_VENUES=2`：venue 0 = 真实撮合核心，venue 1 = 合成参考市场。

### 4.2 信号（框架 -> agent 的 wire）

```cpp
struct SignalSet {
    uint64_t tick; bool valid;
    std::array<Price_t, MAX_VENUES> mid;
    std::array<bool,    MAX_VENUES> mid_valid;
    std::array<Qty_t,   MAX_VENUES> spread;
    std::array<int64_t, MAX_SIGNALS> custom;   // 预留：MACD 等
};
```

### 4.3 私有视图（框架 -> agent 的 wire）

```cpp
struct Fill { uint8_t venue; ClientOrderId client_id; Side_t side;
              Price_t price; Qty_t qty; bool maker; };

struct OrderUpdate { enum class Kind { Accepted, Cancelled, Rejected, Filled } kind;
                     uint8_t venue; ClientOrderId client_id;
                     RejectReason reason; Qty_t remaining; };

struct OpenOrder { uint8_t venue; ClientOrderId client_id;
                   Side_t side; Price_t price; Qty_t remaining; };

struct PrivateView {
    uint64_t tick;
    std::vector<Fill> fills;
    std::vector<OrderUpdate> updates;
    std::vector<OpenOrder> open_orders;   // 当前挂单快照
};
```

### 4.4 组合（框架 -> agent 的 wire）

现货式、合约乘数 = 1：

```cpp
struct VenuePortfolio {
    int64_t position;   // 有符号
    int64_t cash;       // price*qty 单位；买 -> 减，卖 -> 加
    double  avg_cost;
    double  realized_pnl;
    double  unrealized_pnl;   // engine 用延迟 mid 标注
};
struct PortfolioView { uint64_t tick; std::array<VenuePortfolio, MAX_VENUES> venues; };
```

`cash`/`position` 由框架从成交更新；`unrealized = position * (delayed_mid - avg_cost)`。

### 4.5 Agent 输出

```cpp
struct AgentOrder {
    uint8_t  venue;      // 0 = real, 1 = ref
    OPKind   kind;       // Limit / Market / Cancel
    Sym_t    sym;
    Side_t   side;
    Price_t  price;      // limit 价；market/cancel 忽略
    Qty_t    qty;        // cancel 为 0
    ClientOrderId client_id;  // 新单 id，或 cancel 的目标 client id
};
struct AgentOutput { std::array<AgentOrder, MAX_ORDERS_PER_TICK> orders; size_t n; };
```

### 4.6 写一个 Agent

继承 `AgentBase`，实现 `comb()`：

```cpp
class MyAgent : public av::AgentBase {
public:
    void reset() override { /* 重置自身状态/RNG/client id */ }
    void comb() override {
        // 只读：mkt_in, signals_in, priv_in, portfolio_in
        // 写：out_next
        av::AgentOrder o{};
        o.venue = 0;
        o.kind = OPKind::Limit;
        o.side = Side_t::Buy;
        o.price = 10000;
        o.qty = 1;
        o.client_id = next_client_id_++;
        out_next.push(o);
    }
private:
    av::ClientOrderId next_client_id_ = 1;
};
```

- `prepare()` 会清空 `out_next`；`commit()` 把 `out_next` 发布为 `out_committed`。
- 想看哪些输入就 register（拷贝/维护自己的成员），不想看就忽略。
- 撤单：从 `priv_in.open_orders` 选一个，发 `kind=Cancel, client_id=该挂单 id`。
- 成交/状态：处理 `priv_in.fills` / `priv_in.updates`。
- 资金/PnL：读 `portfolio_in`。
- 每个 agent 有唯一 `account`；`client_id` 由 agent 自己分配且在该 account 内唯一。

注册：`SimEngine::setup()` 里 `agent_storage` 持有对象，`agents` 保存裸指针，
`runner.add(agent)`，然后 `router.set_accounts(agents)`。替换 class 1 为你的
策略即可（目前是第二组参数的随机交易者）。

### 4.7 写一个 Signal 模块

继承 `SignalModule`，实现：

```cpp
void comb(const av::MarketView& future_view, av::SignalSet& out) override {
    // future_view = agents 下一周期将看到的 view（已可用，因为 D>=1）
    // 写 out.mid / out.spread / out.custom[...]
}
```

注册：`signal_storage` 持有对象，`runner.add_signal(sig)`。多个 signal 模块
并行时，各自只写自己的字段。

---

## 5. 用法

### 5.1 构建 / 测试 / 运行

```bash
make test        # doctest：delay line / order source / portfolio / sim
make sim         # 编译 build/agentvenue
make run         # 用默认参数跑并写到 build/sim

# 自定义
./build/agentvenue --ticks 2000 --agents 100 --seed 42 --delay 1 \
                   --threads 1 --out build/sim

# 压测（不写盘）
./build/agentvenue --ticks 5000 --agents 1000 --seed 1 --delay 1 --no-record
```

CLI：

| 参数 | 默认 | 含义 |
|---|---|---|
| `--ticks N` | 10000 | agent tick 数 |
| `--agents N` | 100 | 总 agent 数，前后各半分成两类参数 |
| `--seed N` | 42 | 全局种子（每个 agent 由它派生独立 seed） |
| `--delay N` | 1 | 市场数据延迟（必须 >= 1） |
| `--threads N` | 1 | 低倍频 agent/signal 的并行度 |
| `--out DIR` | build/sim | CSV 输出目录（自动创建） |
| `--no-record` | — | 不写任何 CSV（压测用） |

### 5.2 输出文件

写到 `--out DIR`：

| 文件 | 粒度 | 列 |
|---|---|---|
| `market.csv` | 每 venue 每 agent tick | `tick,venue,sym,bid,ask,mid,spread,last,last_qty,trade_count,volume` |
| `agents.csv` | 每 agent 每 venue 每 tick | `tick,agent,class,venue,position,cash,avg_cost,realized,unrealized,open_orders` |
| `agent_orders.csv` | 每提交/拒绝的单 | `tick,agent,venue,kind,side,price,qty,client_id,status,reason` |
| `ref_fills.csv` | 每笔参考市场成交 | `tick,agent,client_id,side,price,qty` |
| `trades_v0.csv` | 真实 venue 每笔成交（core 输出） | crosscheck schema |
| `ticks_v0.csv` | 真实 venue 每个 OP 的 tick（core 输出） | crosscheck schema |

`market.csv` 是 agent 实际看到的市场快照；`ticks_v0.csv` 是 core 针对每个 OP
产出的逐 OP tick，两者粒度不同。

### 5.3 确定性

- 每个 agent 独立 RNG：`seed = mix(global_seed, agent_index)`。
- 订单收集按 agent 下标顺序；core 单线程；信号模块确定性。
- **结果与 `--threads` 无关**（已验证 `threads=1` 与 `threads=4` 的全部 CSV
  逐字节一致）。

---

## 6. 当前语义细节

- **core 吞吐**：`core_top::comb` 一次只处理 1 个 OP。`high_freq_phase`
  通过反复调用把 `AgentOrderSource` 排空（受 `substep_max` 限制），因此每个
  agent tick 会消化上一轮提交的全部订单。
- **resting volume**：core 的 `Insert` 事件只带 addr；框架用
  `ctx.qty - 本 OP 的 taker 成交量` 还原挂单剩余量。
- **成交归属**：TakeLine 的 `trade_id_vec/trade_vol_vec` 同时产生 maker 和
  taker 的 `Fill`；成交价 = maker 挂单价。
- **撤单**：`CancelResult.order_id` 定位目标内部 id，删除 open order，
  推 `Cancelled`；找不到目标也会推 `Cancelled`（框架不把它当错误）。
- **ref venue**：买单成交价 = `ref_ask`，卖单 = `ref_bid`；limit 不 cross
  直接 reject；无挂单（立即成交）；成交在下一 tick 产生。
- **reject（当前框架侧）**：`InvalidVenue`、`InvalidPrice`、`InvalidQty`、
  `QtyTooLarge`（默认 `MAX_AGENT_QTY=100`）、`UnknownCancelTarget`、
  `LimitNotCrossingRef`。以后由 core 的 reject event 接管（见第 8 节）。

---

## 7. 限制 / 已知问题

1. **线程模型**：`threads>1` 目前每个 phase 临时 `std::thread` spawn，开销
   偏大（默认 1）。后续换常驻线程池即可，接口不变。
2. **内存只增**：`IDallocator` 的 `map_in/map_out` 和 core 的 `resting_index`
   不会删除已成交/已撤单条目。小规模/短时间可接受，长跑需要 core 侧清理
   （见第 8 节）。
3. **单 symbol meta**：`OoOobookCPP` 的 `BookMeta` 是单 symbol。一个 core 只
   能跑一个 symbol；跨市场请用多个 venue（各自一个 core）。
4. **记录体积**：`agents.csv` 是 per-agent × per-venue × per-tick，1000 agent
   × 5000 tick 会非常大；压测用 `--no-record`。
5. **样例 ref venue 未被使用**：随机交易者只打 venue 0；套利策略请用
   `venue=1`。
6. **资金/风险**：目前视为无限资金，框架只做基础 sanity；真正的
   checker（保证金/持仓/风控）在 `OoOobookCPP/src/bookkeeping`，后续接入。
7. **价格漂移**：随机交易者的价格是围绕延迟 mid 的随机游走；mid 会缓慢漂移，
   属预期，调 `sigma/lambda` 即可。
8. **`open_orders` 快照**：每 tick 对所有 open order 做一次过滤，复杂度
   O(agent 数 × open 数)；当前规模没问题。
9. **agent 内部状态**：样例直接用普通成员在 `comb` 里更新（每个 agent 在自己的
   线程里串行执行）。想更 RTL，可用 `core/registers.hpp` 的
   `DoubleBufferRegister` 包一层。

---

## 8. 等待 `OoOobookCPP` 跟进的点

按价值排序（框架侧都已留好接口/位置）：

1. **Reject event**（最重要）。建议在 `instructions.hpp` 增加：

   ```cpp
   enum class UopKind : uint8_t { TakeLine, InsertSlot, CancelSlot, End, Reject };
   enum class RejectReason : uint8_t {
       InvalidPrice, InvalidQty, QtyTooLarge, InvalidContract,
       UnknownCancelTarget, InsufficientFunds, PositionLimit,
       PostOnlyWouldCross, SelfTradePrevented, RiskLimit, Other
   };
   struct RejectResult { RejectReason reason; int64_t detail0; int64_t detail1; };
   ```

   `CanonicalEvent`：`kind=Reject`、`id=被拒 OP 的内部 order id`、`op_seq`、
   `op_end=true`、`payload=RejectResult`、`snapshot`（可选）。框架收到后
   `IDallocator::get(id)` 映射回 client，推 `OrderUpdate{Rejected, reason}`，
   并删除第 6 节里的临时校验。

2. **Insert 事件带挂单量**：`struct InsertResult { CanonicalAddr_t addr; Qty_t volume; };`
   替代裸 `CanonicalAddr_t`。框架就不必用 `ctx.qty - taker_filled` 反推。

3. **TakeLineResult 带 `cleared[16]` 掩码**：被吃满的 maker 直接删 open order
   和 core 的 `resting_index`，无需维护 remaining。

4. **core 侧 `resting_index` 清理**：配合 3，在 fully-filled/cancel 时删除，
   解决长跑内存增长。

5. **Post-only 标志 + 自成交防护（STP）**：用 RejectReason 的
   `PostOnlyWouldCross` / `SelfTradePrevented`。做市 agent 尤其需要（agent
   看到的是延迟盘口，无法只靠 agent 侧保证不 crossed）。

6. **`core_top::comb` 返回本次 accept 的 OP 数**（或 accept 的 id 列表）：
   现在框架靠 `op_queue.ready_num()` 反算，能工作；以后 core 内部拒绝时返回
   值更准。

7. **多 symbol meta / 一个 core 多 symbol**：如果想在一个 core 里跑多个真实
   market。否则用多个 `core_top` 实例（推荐，无需改 core）。

8. **显式 market-data snapshot 端口**：如果以后 core 把 `idvol/meta` 藏进
   reg/RAM，agent 侧不应直接读；那时需要一个只读快照接口。

9. **每 tick 处理多个 OP**：目前 `core_top::comb` 一次 1 个 OP，框架靠循环
   drain。若 core 支持一个 comb 处理一批 OP，可减少 sub-tick 开销。

---

## 9. 扩展指南

- **加策略**：在 `agents/strategies/` 新建类继承 `AgentBase`；在
  `SimEngine::setup()` 里创建、设置 `id/account/class_id`、推入
  `agent_storage`，`runner.add(agent)`。建议第一批把 class 1 的随机交易者
  换成你的策略，class 0 保留为噪声。
- **加信号**：在 `agents/signals/` 新建 `SignalModule`，写入 `SignalSet.mid/
  spread/custom`；`runner.add_signal(sig)`。
- **加 venue / 参考价**：`SimEngine` 里已有 `real` + `ref`；多市场时可再实例化
  一个 `RealVenue`（各自 `core_top`/`IDallocator`/source），并把 `MAX_VENUES`
  调大、在 `build_venue_view`/`MarketView` 里加对应项。
- **调参**：`SimEngine::setup()` 里的 `RandomTrader::Config`（`lambda_new/
  lambda_cancel/sigma/limit_ratio/max_open/...`）与 CLI。

## 10. 测试

```bash
make test
```

覆盖：delay line 读写/回绕、order source 握手、现货组合（多/空/部分平仓）、
仿真确定性（同 seed 两次一致）、随机交易者能产生成交。

新增测试放 `test/`，`Makefile` 会自动 glob。doctest 在本地 `libs/doctest/`
（已 gitignore），新环境请自行放置。
