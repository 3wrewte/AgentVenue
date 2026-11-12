# AgentVenue

基于撮合核心 `OoOobookCPP` 的多智能体（agent-based）仿真框架。目标是用
RTL 风格的 agent 读取（带延迟的）市场数据、提交订单，在同一个撮合核心上
做博弈/微观结构实验。

当前第一版包含：

- 完整的 agent 生命周期框架（注册、每 tick 多线程轮询、prepare/comb/commit）。
- 真实撮合 venue（封装 `OoOobookCPP` 的 `engine_top`，含 `Checker` 账户风控）。
- 合成参考市场 venue（固定 bid/ask、无限流动性、下一 tick 成交）。
- 延迟市场数据 + 提前一周期计算的 signal 模块。
- 成交/挂单/组合（现货式）的路由与记录，以及 core 的 trade/tick/reply 流。
- 随机交易者样例（泊松下单/撤单，高斯定价）+ 三个示例策略（MACD / 动量 / 套利）。
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
    strategies/strategy_exec.hpp
    strategies/macd_strategy.hpp
    strategies/momentum_strategy.hpp
    strategies/arbitrage_strategy.hpp
  runner/
    delay_line.hpp              # MarketView 固定延迟环
    order_source.hpp            # 与 engine_top 握手的 OPcontext FIFO
    venue.hpp / venue.cpp       # RealVenue + SyntheticVenue + build_venue_view
    fill_router.hpp             # TradeEvent/InputReplyEvent -> fills/updates/open orders/portfolio
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
   high freq ───►│  RealVenue(engine_top + Checker + CSV)     │
   (core sub-    │     ▲  AgentOrderSource (OPcontext FIFO)   │
    ticks)       │     │                                      │
                 │  SyntheticVenue (固定 ref bid/ask)          │
                 │                                            │
   switch  ─────►│  build MarketView -> DelayLine              │
                 │  SignalModule 读 delay.at(T+1-D)            │
                 │  给 agents 填 mkt_in / signals_in /         │
                 │  priv_in(account) / portfolio_in            │
                 │                                            │
   low freq  ───►│  AgentRunner: prepare -> comb -> commit     │
   (agents +     │     每个 agent 写 out_next，commit 后       │
    signals)     │     out_committed                           │
                 │                                            │
   gearshift ───►│  收集 out_committed -> OPcontext ->         │
                 │  Checker::check -> venue source             │
                 └────────────────────────────────────────────┘
```

职责划分：

- **OoOobookCPP core**：唯一的撮合/订单簿真相。header-only；`engine_top::comb`
  一次处理一个 OP，内部完成 OP -> mOP -> uOP、ID 分配/回收、`resting_index`
  清理，并输出自包含的 `TradeEvent` / `TickEvent` / `InputReplyEvent` 流。
- **RealVenue**：`engine_top` + `Checker` + `AgentOrderSource` + CSV 流水。
  `engine_top` 没有内建 checker；每个提交的 `OPcontext` 先过
  `Checker::check`（账户是否存在、cash/base/position_limit、RepeatedID），
  被拒时以 NOOP 形式进入 core，回执仍走正常的 `InputReplyEvent`。
- **SyntheticVenue**：参考市场，无 core。固定 `bid/ask`，无限流动性，
  limit 必须 cross，否则 reject；订单在下一 agent tick 成交。
- **AgentBase**：RTL 风格。输入是框架驱动的 wire（`mkt_in`/`signals_in`/
  `priv_in`/`portfolio_in`）；输出是寄存器（`out_next` 在 comb 写，
  `out_committed` 在 commit 后可见）。`account_cfg` 是每账号的
  cash/base/position_limit 配置（`nullopt` = 无限）。
- **SignalModule**：和 agent 同 phase 运行，但读的是“agents 下一周期将看到的”
  MarketView，结果 reg 一周期后与同一 view 一起交付。
- **FillRouter**：把 `TradeEvent` / `InputReplyEvent` 翻译成每个 agent 的成交、
  挂单状态、持仓/现金/PnL，并维护框架侧 open-order 表（key =
  `(venue, account, client_id)`）。回执不带 side/price，所以合成 limit 时在
  gearshift 记录一份 pending 元数据。
- **AgentRunner**：注册表 + `parallel_for`（默认单线程；`threads>1` 时按
  phase 分块并行，join 即 phase barrier）。
- **SimEngine**：高低频、gearshift、延迟、记录。

---

## 3. Tick 时序（重要）

设 `D = market_delay >= 1`。每个 agent tick T：

1. **高倍频（main 单线程）**：用 `AgentOrderSource` 与 `engine_top` 握手，反复
   `ctxs = source.issue(engine.op_queue.ready_num()); engine.comb(ctxs.size(), ctxs);`
   直到 source/ROB 排空（或到 `substep_max`）。每次 comb 产出的
   `TradeEvent`/`InputReplyEvent` 立即被 `FillRouter` 消费，
   `TickEvent` 更新 `MarketStats`；`Checker::update` 在同一 comb 内结算。
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
5. **Gearshift**：按 agent 下标顺序收 `out_committed`，做框架侧结构校验、
   构造 `OPcontext`（market 的 cap 价在这里填：买 = `Price_max`，卖 = 0），
   经 `Checker::check` 后推入对应 venue 的 source（ref 单推入合成 venue 的
   pending）。`Checker` 的拒绝会在下一 tick 以 `InputReplyEvent` 回来，不会
   进入撮合。

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

struct AccountView {                      // 真实 venue 的 checker 账户状态
    bool valid;
    uint64_t cash, cash_locked;
    uint64_t base, base_locked;
    uint64_t position_limit;
    bool cash_infinite, base_infinite, limit_infinite;
};

struct PrivateView {
    uint64_t tick;
    std::vector<Fill> fills;
    std::vector<OrderUpdate> updates;
    std::vector<OpenOrder> open_orders;   // 当前挂单快照
    AccountView account;                  // checker：已结算余额 + 锁定
};
```

`AccountConfig { optional cash, base, position_limit }` 在 `AgentBase` 上按账号
设置；`nullopt` = 无限。样例默认全无限，因此 MACD/动量/套利的空头信号可以
直接跑；设置有限 base/cash 后，超限订单会收到
`InsufficientFunds` / `InsufficientBase` / `PositionLimit` / `RepeatedID`。

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
- 资金/PnL：读 `portfolio_in`（成交口径）；checker 的余额/锁读
  `priv_in.account`。
- 每个 agent 有唯一 `account`；`client_id` 由 agent 自己分配且在该 account 内唯一。
  资金/持仓约束放在 `account_cfg`（`nullopt` = 无限）。

注册：`SimEngine::setup()` 里 `agent_storage` 持有对象，`agents` 保存裸指针，
`runner.add(agent)`，然后 `router.set_accounts(agents)`。

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
make test        # doctest：delay line / order source / portfolio / checker / sim / strategies
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
| `--agents N` | 100 | 噪声交易者数量（另固定加 3 个示例策略，共 N+3 个 agent） |
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
| `replies.csv` | 每个真实订单的 core 回执 | `tick,venue,seq,account,client_id,kind,reject,detail,filled,rested,discarded,cancel_found,cancel_qty` |
| `trades_v0.csv` | 真实 venue 每笔成交（core 输出） | `seq,trade_idx,taker_order_id,maker_order_id,taker_side,price,qty` |
| `ticks_v0.csv` | 真实 venue 每个 OP 的 tick（core 输出） | `seq,bid_price,bid_qty,ask_price,ask_qty,last_price,last_qty,trade_count,volume` |

`market.csv` 是 agent 实际看到的市场快照；`ticks_v0.csv` 是 core 针对每个 OP
产出的逐 OP tick，两者粒度不同。`replies.csv` 是唯一能看到 checker/core 拒绝
理由（`insufficient_funds`、`position_limit`、`unknown_cancel_target` 等）的流。

### 5.3 确定性

- 每个 agent 独立 RNG：`seed = mix(global_seed, agent_index)`。
- 订单收集按 agent 下标顺序；core 单线程；信号模块确定性。
- **结果与 `--threads` 无关**（已验证 `threads=1` 与 `threads=4` 的全部 CSV
  逐字节一致）。

---

## 6. 当前语义细节

- **core 吞吐**：`engine_top::comb` 一次只处理 1 个 OP；OP/MOP 队列的 head/tail
  是寄存器，所以提交到产出之间有一到两个 comb 的流水线延迟。`high_freq_phase`
  通过反复调用把 source/ROB 排空（受 `substep_max` 限制），每个 agent tick
  会消化上一轮提交的全部订单。
- **resting volume**：`InputReplyEvent.rested_qty` 直接给出挂单剩余量；
  `maker_cleared` 来自 `TakeLineResult.trade_complete_vec`，可精确删除吃满的
  maker；`InsertLineResult.insert_qty` 也带在 core 内部。
- **成交归属**：`TradeEvent` 自包含 taker/maker 的 account/client、side、price、
  qty；成交价 = maker 挂单价。`FillRouter` 同时产生 maker 和 taker 的 `Fill`。
- **撤单**：core 解析 `(account, client)` -> internal id；找不到目标时回执
  `unknown_cancel_target`（kind=NOOP）。成功时 `cancel_found/cancel_qty`
  可用于释放 checker 的锁。
- **reject**：checker 的 `InvalidAccount`、`InsufficientFunds`、
  `InsufficientBase`、`PositionLimit`、`RepeatedID`、`InvalidPrice`、
  `InvalidQty`、`InvalidContract`，以及 core 的 `UnknownCancelTarget`，
  都通过 `InputReplyEvent` 回来；框架只保留结构校验和 ref venue 的
  `LimitNotCrossingRef`。
- **ref venue**：买单成交价 = `ref_ask`，卖单 = `ref_bid`；limit 不 cross
  直接 reject；无挂单（立即成交）；成交在下一 tick 产生。
- **框架侧结构校验**：`InvalidVenue`、`InvalidPrice`、`InvalidQty`、
  `QtyTooLarge`（默认 `MAX_AGENT_QTY=100`）、`LimitNotCrossingRef`。

---

## 7. 限制 / 已知问题

1. **线程模型**：`threads>1` 目前每个 phase 临时 `std::thread` spawn，开销
   偏大（默认 1）。后续换常驻线程池即可，接口不变。
2. **单 symbol meta**：`OoOobookCPP` 的 `BookMeta` 是单 symbol。一个 core 只
   能跑一个 symbol；跨市场请用多个 venue（各自一个 core）。
3. **记录体积**：`agents.csv` 是 per-agent × per-venue × per-tick，1000 agent
   × 5000 tick 会非常大；压测用 `--no-record`。
4. **ref venue 没有 checker**：合成参考市场仍按无限流动性/无资金约束处理；
   套利策略的 ref 腿不受 `AccountConfig` 限制。真实 venue 的检查见
   `Checker`。
5. **市场价格漂移**：随机交易者的价格是围绕延迟 mid 的随机游走；mid 会缓慢
   漂移，属预期，调 `sigma/lambda` 即可。
6. **`open_orders` 快照**：每 tick 对所有 open order 做一次过滤，复杂度
   O(agent 数 × open 数)；当前规模没问题。
7. **agent 内部状态**：样例直接用普通成员在 `comb` 里更新（每个 agent 在自己的
   线程里串行执行）。想更 RTL，可用 `core/registers.hpp` 的
   `DoubleBufferRegister` 包一层。
8. **checker 语义是 spot 的**：cash/base 非负、`open` 按
   `(account, client)`；`PortfolioView` 仍是有符号的成交口径（分析/PnL 用），
   两者不是同一个账户模型。有限资金/持仓的样例需要把策略的空头目标改成从
   初始 base 卖出，或保持 `nullopt`。
9. **position_limit 口径**：core checker 的买入检查用已结算 `base` + 本单
   qty 对比上限，不把其他在途买单算进去；多张同时挂着的买单仍可能一起成交后
   超过上限。做严格限仓需要 core 侧把在途买单也纳入（或策略侧自己扣减）。

---

## 8. 与 core 的接口状态

已经由新版 `OoOobookCPP` 提供、AgentVenue 已接入：

1. **自包含事件流**：`TradeEvent` / `TickEvent` / `InputReplyEvent`；
   `router` 在 `engine_top` 内部完成映射，`Checker` 的拒绝理由通过 reply
   带回，不再需要框架维护 `IDallocator::get`。
2. **`InsertLineResult`**：挂单量直接可读，不再用 `qty - taker_filled` 反推。
3. **`TakeLineResult.trade_complete_vec`**：被吃满的 maker 可直接删除
   open order，`maker_cleared` 已透传到 `TradeEvent`。
4. **core 侧 `resting_index` / ID map 清理**：`op_queue.release` +
   `IDallocator::release` 已在 `engine_top::comb` 内调用，长跑内存不再只增。
5. **`Checker`**：`cash/base/position_limit`、锁定、结算、`RepeatedID` 都在
   `bookkeeping/checker.hpp`；AgentVenue 在 gearshift 调 `check`、在每次
   comb 后调 `update`。

仍然可以继续跟进的点：

1. **Post-only 标志 + 自成交防护（STP）**：做市 agent 尤其需要（agent 看到的
   是延迟盘口，无法只靠 agent 侧保证不 crossed）。
2. **多 symbol meta / 一个 core 多 symbol**：目前一个 `engine_top` 一个
   symbol；跨市场请实例化多个 venue。
3. **显式 market-data snapshot 端口**：如果以后 core 把 `idvol/meta` 藏进
   reg/RAM，agent 侧不应直接读；那时需要一个只读快照接口。
4. **每 tick 处理多个 OP**：目前一次 1 个 OP，框架靠循环 drain。
5. **`engine_top::comb` 加 `inline`**：现在它定义在头文件里但没有 `inline`，
   多个 TU 包含会重复定义；AgentVenue 用 `runner/venue.cpp` 的单 TU 隔离
   绕开，core 侧补一个 `inline` 会更干净。

---

## 9. 扩展指南

- **加策略**：在 `agents/strategies/` 新建类继承 `AgentBase`；在
  `SimEngine::setup()` 里创建、设置 `id/account/class_id`、按需设置
  `account_cfg`、推入 `agent_storage`，`runner.add(agent)`。
  class 约定：`0` = 随机噪声，`1` = MACD，`2` = 动量，`3` = 套利。
- **加信号**：在 `agents/signals/` 新建 `SignalModule`，写入 `SignalSet.mid/
  spread/custom`；`runner.add_signal(sig)`。
- **加 venue / 参考价**：`SimEngine` 里已有 `real` + `ref`；多市场时可再实例化
  一个 `RealVenue`（各自 `engine_top`/`Checker`/source），并把 `MAX_VENUES`
  调大、在 `build_venue_view`/`MarketView` 里加对应项。
- **调参**：`SimEngine::setup()` 里的 `RandomTrader::Config`（`lambda_new/
  lambda_cancel/sigma/limit_ratio/max_open/...`）与 CLI。
- **账户约束**：给 agent 设 `account_cfg.cash/base/position_limit` 即可；
  `nullopt` 表示无限。想跑有限资金的实验，记得策略的 signed target 语义与
  checker 的 spot base 不同（见第 7 节）。

## 10. 测试

```bash
make test
```

覆盖：delay line 读写/回绕、order source 握手（OPcontext）、现货组合
（多/空/部分平仓）、checker 拒单（cash/base/position_limit）、回执路由
（挂单/撤单/成交结算）、仿真确定性（同 seed 两次一致）、随机交易者能产生成交、
三个示例策略的信号/执行逻辑。

新增测试放 `test/`，`Makefile` 会自动 glob。doctest 在本地 `libs/doctest/`
（已 gitignore），新环境请自行放置。
