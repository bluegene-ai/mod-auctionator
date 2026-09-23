# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore 模块

## mod-auctionator

[![core-build](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml)
[![codestyle](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml)

> 上面的徽章 URL 指向本 fork。如果你把模块推送到其他仓库，请将 `bluegene-ai/mod-auctionator` 替换为你自己的 `owner/repo`。

## 兼容性

本分支仅针对 **[azerothcore/azerothcore-wotlk](https://github.com/azerothcore/azerothcore-wotlk) `master`**。

* CI 会检出上游 `master`，把本模块放入 `modules/`，并使用 AzerothCore 官方可复用工作流（`azerothcore/reusable-workflows`）进行构建——NOPCH、clang、`-Wall -Wextra -Werror`，外加对 `modules/` 运行 cppcheck。CI 变绿意味着“能针对上游 master 编译并通过 cppcheck”。
* 不支持其他任何情况：不支持核心 fork、不支持 NPCBots 或其他补丁、不支持 release 分支回溯。如果上游 master 更改了模块使用的 API，构建会故意失败，然后在这里修复。
* 由于 CI 构建**不使用预编译头**，每个源文件都包含它所用到的内容。添加代码时请保持这一点。

<p align="left">
  <img src="https://github.com/araxiaonline/docs/blob/main/docs/media/logo-sm.png?raw=true" alt="Araxia Online" width="70" style="vertical-align: middle;"/>
  <span style="font-size: 20px; vertical-align: middle;" >最初由 Araxia Online 开发</span>
</p>

## 描述

本模块旨在让低人口服务器的拍卖行保持健康库存。目前仍处于构建/测试/配置的早期阶段，但已经能让拍卖行里保持**大量**物品。代码目前还有些粗糙，但我已经 20 多年没写 C++ 了，所以还在慢慢改进。

## 安装与设置

1. 将代码下载到源码的 `modules/` 文件夹中。
2. 带着模块构建核心（`-DMODULES=static`），然后部署新的 `worldserver` 二进制文件。
3. 确保**服务器的 `SourceDirectory` 指向包含 `modules/mod-auctionator/data/sql/` 的源码树**（留空则使用编译时的源码路径）。AzerothCore 更新器会从 `<SourceDirectory>/modules/<module>/data/sql/` 应用模块 SQL；如果该路径不可达，模块的表就永远不会创建，卖家找不到候选物品，市场导入也会失败。
   如果你更愿意手动执行，请针对对应的数据库运行以下 SQL：
   * world 数据库：`data/sql/db-world/base/2023-09-18.sql` 和 `data/sql/db-world/base/mod_auctionator_gm_list.sql`
   * characters 数据库，按以下顺序：`data/sql/db-characters/updates/2023_11_12_00_marketprice.sql`、`data/sql/db-characters/updates/2026_09_20_00_market_price_history.sql`、`data/sql/db-characters/updates/2026_01_29_00_improve_performance.sql`
   在两个数据库中都用 `SHOW TABLES LIKE 'mod_auctionator%';` 验证。
4. **创建模块配置文件。** 构建会附带 `modules/mod-auctionator/conf/mod_auctionator.conf.dist`；服务器只读取 `configs/modules/mod_auctionator.conf`（没有 `.dist`），而且 Windows 构建不会自动为你复制：
   ```
   copy modules\mod-auctionator\conf\mod_auctionator.conf.dist configs\modules\mod_auctionator.conf
   ```
   没有该文件时，所有选项都会回退到代码默认值，而 `Auctionator.Enabled` 默认为 **0**，因此模块会保持静默。
5. 可选：将模块的日志行添加到 `worldserver.conf`（它们**不能**放在模块配置中，因为模块配置是在日志系统初始化之后才加载的）：
   ```
   Appender.AuctionatorLog=2,5,0,auctionator.log,w
   Appender.AuctionatorConsole=1,4,0,"1 9 3 6 5 8"
   Logger.auctionator=4,AuctionatorLog AuctionatorConsole
   ```
6. 如果你想重新开始，用以下 SQL 命令清空拍卖行。**仅在服务器停止时**执行此操作。拍卖行会缓存在内存中，在服务器运行时执行此操作不会有好结果。请针对角色数据库运行。

> 模块本身从不硬编码 schema 名称：卖家和市场查询会在运行时从 `worldserver.conf`（`WorldDatabaseInfo` / `CharacterDatabaseInfo`）解析 world 和 characters 数据库，因此像 `acore_world80` / `acore_characters80` 这样的非默认名称可以直接使用。
>
> 由于这些查询通过同一个连接跨越两个 schema，**world** 数据库用户需要对 characters schema 拥有 `SELECT` 权限（默认 AzerothCore 设置对所有数据库使用同一用户，已满足此要求）。没有该授权时，卖家的物品查询会在 SQL 日志中报 `Table ... doesn't exist`，卖家会记录 "seller item query produced no rows"。


```
DELETE FROM `item_instance` WHERE `guid` IN (SELECT `itemguid` FROM `auctionhouse`);
DELETE FROM `auctionhouse`;
```

## GM 命令

单独使用 `.auctionator`（或 `.auctionator help`）会打印命令列表，未知子命令会给出明确错误，而不是什么都不做。

### auctionator add <house> <item[,item...]> <price> [stack] [hours] [owner]

ItemID 可以在数据库表 `item_template` 中查找。

* `house` - `2` = 联盟，`6` = 部落，`7` = 中立
* `item` - 一个 id，或多个用逗号分隔的 id（每条命令最多 200 个）
* `price` - **单价**，单位为铜币；买断价为 `price * stack`
* `stack` - 可选，默认 1，上限为该物品的最大堆叠数
* `hours` - 可选，默认 48，范围 1..720
* `owner` - 可选：`bot`（默认）、`me`，或角色 guid

**起拍价。** 起拍价为 0 的上架可以被 1 铜币拿走：核心只拒绝低于 `auction->startbid` 的出价，而 1 铜币永远不低于 0。因此本命令用与自动卖家完全相同的规则从买断价推导起拍价——`买断价 * (1 - Auctionator.Seller.BidStartModifier)`，且至少为 1——在默认 `0.3` 下，10000 铜币的上架起拍价为 7000。如果希望 GM 上架只能买断（起拍价等于买断价，出价与买断同价），把 `Auctionator.Seller.BidStartModifier` 设为 `0`。

**金币处理。** 使用默认 owner（`bot`，即配置的 `Auctionator.CharacterGuid`）时，销售所得会邮寄给拍卖机器人角色，并由邮件脚本回收：金币离开经济系统。如果无人购买，该上架的**物品**同样会被回收，因此流拍也不会留下死邮件。传入 `me` 或角色 guid 可以让金币到达真实角色。

物品会从 `item_template` 全新创建，因此任何物品都可以上架，包括拾取绑定、任务和唯一物品。当物品是任务/唯一物品时，命令会警告你，因为这些检查被绕过了。

将一个虹彩珍珠（Iridescent Pearl）以 1 金币买断价添加到中立拍卖行：

```
.auctionator add 7 5500 10000
```

在部落拍卖行以每个 5 银币上架 20 个铜螺栓（Copper Bolt），持续 12 小时，并让金币被回收：

```
.auctionator add 6 4359 500 20 12
```

为你自己上架一件拾取绑定的史诗物品，持续 3 天，金币归你：

```
.auctionator add 7 19019 500000 1 72 me
```

### auctionator addlist [house] [owner]

列出 `mod_auctionator_gm_list`（world 数据库）中所有启用的行，这样可以用一条命令补货一组精选的特殊物品。可选的 `house` 和 `owner` 参数会覆盖每行的列值。

```
.auctionator addlist          # 使用每行自己的 house/owner
.auctionator addlist 7        # 强制使用中立拍卖行
.auctionator addlist 7 me     # 中立拍卖行，金币归你
```

### auctionator auctionspercycle <value>

设置每个周期向每个拍卖行添加的拍卖数量（所有拍卖行共享）。卖家会遍历整个候选集并上架这么多物品，因此不再有需要保持同步的单独查询限制。

这会直接影响每个周期对 worldserver 和 MySQL 服务器的压力，所以在较弱的硬件上请调低此值。

```
.auctionator auctionspercycle 45
```

### auctionator bidonown <value>

允许竞标者对拍卖机器人自己的拍卖出价。仅对测试买家有用。注意，竞标者通常是一个金币*水龙头*（它向卖家付款但不向自己付款），参见下面的“经济与安全保证”。

有效值为 `1`（启用）和 `0`（禁用）。

```
.auctionator bidonown 1
```

### auctionator disable <target>

对特定阵营禁用卖家或竞标者。`<target>` 是以下之一：`hordeseller`、`allianceseller`、`neutralseller`、`hordebidder`、`alliancebidder`、`neutralbidder` 或 `all`。

更改会立即应用到正在运行的调度中：无需重启（只有总开关 `Auctionator.Enabled` 需要重启）。

```
.auctionator disable hordeseller
```

### auctionator enable <target>

对特定阵营启用卖家或竞标者（target 与 `disable` 相同）。新启用的事件第一次运行大约在一分钟后，之后按配置的周期时间执行。

```
.auctionator enable hordeseller
```

### auctionator expireall <house> [all]

在下一个 tick 使拍卖机器人在指定拍卖行的拍卖过期。玩家拍卖不受影响，除非你显式传入 `all`（此时核心会归还他们的物品并退还竞标者的金币，不会销毁任何东西）。通过 `.auctionator add ... me` 或角色 guid 指定 owner 的上架同样被视为“他人的拍卖”，需要 `all` 才会被清理。

```
.auctionator expireall 7
.auctionator expireall 7 all
```

### auctionator multiplier <typetext> <qualitytext> <multiplier>

Typetext 是 `seller` 或 `bidder`，取决于你想设置哪一个。

设置某个品质的出售和购买倍率。品质有：

* `poor`
* `normal`
* `uncommon`
* `rare`
* `epic`
* `legendary`

Multiplier 是十进制值。默认值在配置文件中设置。

对于**卖家**倍率，注意它只缩放*回退*价格（`item_template.BuyPrice`，或物品也没有商人价格时的 `Auctionator.Seller.DefaultPrice`）。导入的市场价格永远不会被乘，否则市场驱动定价就不是市场驱动了。

**买家**倍率小于 `1.0` 时会按配置生效，降低买家的可支付上限（不会被静默抬到 1.0）；`0.0` 表示买家什么都不买，唯一的硬下限是 1 铜币。卖家倍率在回退价格上的行为相同。

```
.auctionator multiplier bidder epic 10
```

### auctionator status

显示内存中配置的状态以及当前拍卖行情况。用于检查可能已更改的配置值或查看拍卖数量。示例输出如下。

```
.auctionator status
```

输出：
```
[Auctionator] Status:

 Enabled: 1

 CharacterGuid: 2
 Horde:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9319
    Bidder Enabled: 1
        Cycle Time: 2
        Per Cycle: 20
 Alliance:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 8959
    Bidder Enabled: 1
        Cycle Time: 1
        Per Cycle: 20
 Neutral:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9379
    Bidder Enabled: 1
        Cycle Time: 3
        Per Cycle: 30
 Seller Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Bidder Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Seller settings:
    Auctions per run: 100
    Default Price (no vendor/market price): 1000000
    Randomize Stack Size: 1
    Bid Start Modifier: 0.300000
    Market data max age (days, seller+bidder, 0 = never): 14
    Prefer market items: 1
    Exclude VerifiedBuild = 1 items: 0
    Min price modifier (x SellPrice): 1.000000
    Max price modifier (x market avg): 2.000000
```

## 共享阵营拍卖行（AllowTwoSide.Interaction.Auction = 1）

开启该核心选项后，三个拍卖行是同一个对象：核心把所有拍卖都以 `houseId = Neutral` 存储（`WorldSession::HandleAuctionSellItem` 强制如此），客户端也只搜索中立分区（`AuctionHouseSearcher` 依据 `AuctionEntry::GetFactionId()`，即 `houseId`，选择分区）。因此联盟/部落条目会是**任何客户端都看不到、也无法出价**的拍卖，同时仍然占用该拍卖行的配额并让 `.auctionator status` 显示虚高的数字。

模块在每一层都拒绝创建这种条目：

* `.auctionator add 2 ...` / `add 6 ...` 直接报错，`addlist` 会跳过 `house` 列（或 `house` 覆盖参数）为 2/6 的行；
* `Auctionator::CreateAuction()` 作为第二道防线拒绝它，因此任何调用方都无法创建隐形拍卖；
* `AllianceSeller`/`HordeSeller` 与 `AllianceBidder`/`HordeBidder` 事件每个周期只打一条错误日志、不做任何事（所有*玩家*拍卖都在中立行，非中立买家本来也找不到它们）。

在这种服务器上只启用 `Auctionator.NeutralSeller` 与 `Auctionator.NeutralBidder`。`.auctionator status` 会打印提醒；`.auctionator expireall 2|6` 仍然可用，正好用来清理该选项开启前遗留的旧行。

## 市场数据导入

卖家从 `mod_auctionator_market_price`（characters 数据库）获取定价。该表现在保留历史记录：主键是 `(entry, scan_datetime)`，因此导入会按物品和扫描时间 upsert，部分导出永远不会触碰它未提及的物品，你可以按需清理旧扫描。`source` 列记录该行来自哪个数据源。

### 方案 A - 服务器按定时器导入 CSV（无需外部工具）

```
Auctionator.MarketData.ImportFile = "C:/acore/marketprice.csv"
Auctionator.MarketData.ImportSource = tsm
Auctionator.MarketData.ImportIntervalMinutes = 360
```

worldserver 会在启动约一分钟后读取该文件，之后每隔一个间隔读取一次。未更改的文件（大小和 mtime 相同）会被跳过，因此间隔可以设置得很短。列，带可选表头行：

```
scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count
```

### 方案 B - 辅助脚本（需要 Node 18+ 和 mysql 客户端）

```
cd apps/marketprice

# 仅首次需要执行
npm install

node index.js mypricedata.csv --source=tsm | mysql -u <dbuser> -p <character_database_name>
```

生成的 SQL 是幂等的（在事务中批量 `INSERT ... ON DUPLICATE KEY UPDATE`），因此重新导入同一导出会更新而不是失败。进度输出到 stderr，SQL 输出到 stdout。

`minimum_buyout` 和 `minimum_bid` 不使用；`item_count`（成交量）被卖家的市场权重使用。

### 运维

```
.auctionator market              # 行数、不同物品数、有多少物品有可用扫描
.auctionator marketimport        # 立即导入 Auctionator.MarketData.ImportFile
.auctionator marketimport force  # 即使文件未更改也导入
.auctionator marketprune 30      # 删除超过 30 天的扫描
```

当文件的每一行都被拒绝（日期格式错误、物品 entry/价格非数字或为 0）时，`marketimport` 会报告“nothing was imported”并记录一条错误日志：损坏的导出不能看起来像是空的。空文件仍然是成功的无操作。

早于 `Auctionator.MarketData.MaxAgeDays` 的扫描会被卖家和竞标者同时忽略，因此损坏的导入会降级为商人定价，而不是上架垃圾价格。定期运行 `marketprune`（或从你自己的 cron 运行）以保持历史表小。

`scan_datetime` 按**数据库服务器的时区**解释：扫描的年龄由数据库本身计算（`TIMESTAMPDIFF(..., NOW())`），因此只有当 CSV 携带数据库主机的本地时间时，导入器和读取器才会一致。在 `+08:00` 主机上使用 UTC 导出会看起来老了八小时。

注意：定时导入仅在模块启用时（`Auctionator.Enabled = 1`）运行。如果需要在禁用时导入，请使用 `.auctionator marketimport`。

## 一张图看懂定价

对于每个上架物品，卖家按以下顺序从三个来源中选择一个：

| 来源 | 使用条件 | 缩放方式 |
|---|---|---|
| `mod_auctionator_market_price.average_price` | 存在扫描且新于 `MarketData.MaxAgeDays` | **不缩放**（按导入原值） |
| `item_template.BuyPrice` | 无可用市场数据 | 按品质的 `Multipliers.Seller.*` |
| `Auctionator.Seller.DefaultPrice`（默认 100 金币） | 无市场数据**且**无商人价格 | 按品质的 `Multipliers.Seller.*` |

结果会被限制在 `[SellPrice x MinPriceModifier, marketPrice x MaxPriceModifier]` 范围内（上限仅在价格来自市场时适用），乘以堆叠数量并封顶于 `MAX_MONEY_AMOUNT`。上限在**地板价之前**应用，因此地板价始终生效：即使导入的市场均价远低于商人收购价，机器人也不会以低于商人收购价的价格上架（否则玩家可以买入再卖给商人套利）。起拍价是 `(1 - BidStartModifier) x 买断价` 到买断价之间的随机值，且永远不为 0——起拍价为 0 会让玩家用 1 铜币拿走该拍卖。

## 物品选择

哪些物品可以被上架，以及以什么形式上架，是数据驱动的（world 数据库）：

| 表 | 列 | 含义 |
|---|---|---|
| `mod_auctionator_itemclass_config` | `class`、`subclass`、`bonding`、`max_count`、`stack_count` | 每个物品类别/子类别一行 |
| `mod_auctionator_disabled_items` | `item` | `item_template.entry` 的扁平黑名单 |

* `bonding` - 该行匹配所需的最低 `item_template.bonding`；`0` 表示“无额外约束”。`bonding = 1`（拾取绑定）的物品总是被排除。随附的行只使用 0 和 1，因此该列目前在该类别中充当“允许/不允许未绑定物品”的标志。
* `max_count` - 每个物品条目的配额：当卖家在该拍卖行已有 `max_count` 个拍卖时跳过该物品。**`max_count = 0` 表示“永不上架此类别/子类别”**（它是配额，不是“无限”）。
* `stack_count` - 一个上架中包含多少物品（武器/护甲 `1`，贸易商品和药水 `20`，药剂 `10`，……）。它会被物品自身的最大堆叠数限制；值为 `0` 的行回退到该最大堆叠数（该列是 `NOT NULL DEFAULT 1`，因此不会出现 `NULL`）。当 `Auctionator.Seller.RandomizeStackSize = 1` 时，上架使用 1 到 `stack_count` 之间的随机值，而不是完整值。

`item_template.VerifiedBuild = 1` 的行默认**不会**被过滤掉：在大多数 world 数据库中，该标志表示“未经内容团队验证”，跳过这些行会移除很大一部分可用目录（在标准 WotLK world 数据库中约 30%）。如果你的数据对此有不同含义，请设置 `Auctionator.Seller.ExcludeUnverifiedItems = 1`。

两张表在每个卖家周期都会重新读取，因此编辑无需重启即可生效。

## 经济与安全保证

这些是模块的不变量，而不是副作用：

1. **来自机器人和 GM 上架的金币会被系统回收。** 当机器人/GM 的上架售出时，核心会将收益邮寄给拍卖机器人角色，邮件脚本会在发送前删除该邮件（以及任何附件）。金币离开经济系统。**流拍同样会被回收**：核心的 `SendAuctionExpiredMail()` 会把未售出的物品附在过期邮件上，而该角色永不登录、核心只在角色登录时清理邮箱，所以脚本把过期邮件里的物品一并删除——否则每个流拍都会永久留下一个死邮件和一条 `item_instance` 记录，邮箱达到 100 封上限后核心还会开始静默丢弃过期邮件。向 `.auctionator add` 传入显式 `owner` 是将金币发送给角色的唯一方式；命令回复会将此类上架标记为 `GOLD SINK BYPASSED`。
2. **永远不会支付保管费。** 模块从不收取保管费，因此模块创建的拍卖以 `deposit = 0` 创建；否则核心的 "bid + deposit - cut" 支付会凭空造币。
3. **玩家拍卖永远不会被破坏。**
   * `.auctionator expireall <house>` 只使拍卖机器人角色拥有的拍卖过期；玩家拍卖不受影响，除非传入 `all`。
   * 机器人买断的物品会被销毁，但卖家在同一交易中获得付款，前一个竞标者通过核心的 `SendAuctionOutbiddedMail` 在出价被接管前获得退款，因此没有任何托管中的玩家金币被销毁。
   * 竞标者从不对已有出价的拍卖*出价*（会跳过），因此玩家的出价永远不会被覆盖。但它可以**买断**玩家已经出价的拍卖：前一个竞标者先通过核心的 `SendAuctionOutbiddedMail` 获得退款（关于该退款的含义见第 6 条），卖家在同一交易中获得付款。
4. **如果有人在玩配置的拍卖机器人角色，邮件回收会自动停止。** 邮件钩子检查接收者是否在线，如果在线则不动该邮件（并在日志中警告）。专用的、从不登录的拍卖机器人角色保持完整回收。
5. **竞标者默认禁用**（`Auctionator.*Bidder.Enabled = 0`），因为它购买玩家拍卖而不付款：核心用新创建的金币支付卖家，这会膨胀经济。仅在你确实希望如此时才启用。
6. **每次机器人购买都是无资金的，退款路径依赖邮件回收。** 模块按设计不持有任何金币，因此其出价和买断没有任何托管记录，而 `SendAuctionSuccessfulMail()` 仍会支付卖家：每次购买都会增加货币供应（机器人赢得的物品被销毁）。当*玩家*出价超过机器人时，核心退还机器人的出价——这些金币从未被托管——该退款之所以无害，只是因为邮件回收会销毁它（见第 4 条：当拍卖机器人角色在线时，钩子会故意让开，此时这样的退款就变成了真实的铸造金币）。每次购买都会以 `UNFUNDED` 前缀记录，并且在竞标者启用时启动时会打印一次警告，因此这个水龙头永远不会悄无声息。

## 当前限制

1. ~~堆叠表现异常，尤其是附魔棒之类的物品。~~ 已修复。上架的堆叠数来自 `mod_auctionator_itemclass_config.stack_count`（受物品自身最大堆叠数限制）；当 `Auctionator.Seller.RandomizeStackSize = 1` 时，它是 1 到该大小之间的随机值。
2. ~~无法控制堆叠大小，硬编码为 20。~~ 堆叠大小按类别/子类别数据驱动（`stack_count`，见“物品选择”），GM 上架接受显式 `stack` 参数。
3. 物品选择由 `mod_auctionator_itemclass_config` 和 `mod_auctionator_disabled_items` 驱动；目前仍没有游戏内编辑器（编辑表并等待下一个卖家周期）。
4. 导入是同步的，因此非常大的 CSV 在写入时会阻塞 world 线程。保持 `Auctionator.MarketData.ImportMaxRows` 合理，并优先使用增量导出。
5. 被回收的邮件是**销毁**而非退回：流拍上架的物品会随其过期邮件一起删除（见“经济与安全保证”第 1 条），因此模块既是金币水池也是物品水池。机器人不会收回自己的库存——若希望流拍物品找回，请在 `.auctionator add` 上使用真实 `owner`（那封邮件不会被回收）。