# 内容输入校验优化与阶段计时（2026-11-05）

阶段计时把这组大文件负载的主要 CPU 成本定位到文本校验。实现按 8 字节跳过不含 NUL 的连续 ASCII，并保留跨块 UTF-8 标量状态和全部拒绝条件。

## 实现和测试

- 用 memcpy 读取完整 64-bit 字，不依赖输入对齐或指针别名；长度不足一字不读取。只有 ASCII 字节且无待完成标量时才进入字扫描，高位或零字节出现时继续逐字节状态转移。没有 CPU 指令集探测或新的依赖。
- NUL、孤立 continuation、过长编码、surrogate、超过 U+10FFFF、未完成输入仍按原契约拒绝；BOM 剥离和读取后文件版本检查保持原有位置。Windows/Linux/macOS 共用该校验器。
- 新增一个正式内容单元测试条目，六个 TinyTest 用例：未对齐/短 ASCII、全部 NUL 位置及分块、全部两字节组合与 Core 独立解码器对照、三/四字节边界及非法序列、密集多字节与确定性 NUL 变异、实际 64 KiB 读取边界。它不是对全部三/四字节字符串空间的穷举。
- 本机 Release 全部 CTest 8/8 通过；最终基准及单元测试另行通过 2/2。基准还验证启用计时后的真实 ugrep 错误和提交前超时保留原有终态与进程回收。

## 内部计时契约

private engine.content_metrics 默认为 NULL；调用方只能在内容任务静止时配置它，提交时固定借用至终态。每个文件批次独立暂存，再在 query mutex 下累计，保留的批次不会在终态后访问 sink。未启用时，新增阶段计时不读取时钟；每批仍有 144 字节计数暂存，计入原有引擎预算，没有额外计数堆分配。公共 API/ABI 未新增计时字段或开关。该计时后端本轮限 POSIX。

| 计数/时间 | 边界 |
| --- | --- |
| read/write bytes、calls、poll calls | 实际输入读、写系统调用；无 BOM 语料完成后要求读/写字节均等于固定输入 |
| open_ns | 初次逐组件打开及对象检查 |
| spawn_ns | FSX 原生 child spawn，包含管道/属性设置 |
| read_ns / text_ns | 输入 read 调用 / 增量 UTF-8 与 NUL 校验 |
| poll_ns / write_ns / output_ns | poll 等待 / stdin write / stdout-stderr 读取与暂存 |
| finish_ns / postcheck_ns | 管道关闭与回收 / 读取后版本和路径关联验证 |
| file_ns / worker_cpu_ns | 文件 worker 调用完整墙钟 / CLOCK_THREAD_CPUTIME_ID 差；包括计时开销，不包含 decode、候选生产、查询线程或子进程 CPU |
| decode_ns | query mutex 下的 JSON 解码，单独位于 file_ns 之外 |

各文件的时间累加；两个文件工作线程会重叠，不能把总和当作任务墙钟。文件内列出的阶段不覆盖全部指令/锁/分配开销，剩余量不是另一个已识别阶段。计时保持 errno，失败路径仍传播原生状态。

## 基准设计

- 语料与首轮一致：64 × 1 KiB 小文件、4 × 4 MiB 大文件，固定 UTF-8、稀疏/密集/无命中。大文件多数为 ASCII，仅在每 64 KiB 的首行含多字节字符；不据此承诺密集 Unicode 文本加速。
- scope 单/双文件、direct 单/双文件、profile 单/双文件共六种组合，分别预热一次，再测六轮，每种占据六个先后位置各一次。profile 是启用私有计时的相同 scope 路径；同时保留未启用计时的 scope，用于观察仪器影响。BFS_LIVE 保留独立四轮对照。
- POSIX 共 270 次测量操作；Windows 仍为 56 次 scope 操作。所有内容配置增加单元测试后为 8 个 CTest 条目。
- 两个算法版本分别运行，版本先后没有随机化。最终采集顺序为优化版、原标量控制版；随后恢复并重建优化版。原标量来自精确提交 8d8cdf7ddaf8a6b87c03f264d1ab9e9dea53300d 的 text.c，其余被记录的源码指纹相同。JSON revision 明确标记 working-tree 采集。
- Linux x86_64、Clang 18.1.3、Release、无 sanitizer，ugrep 7.8.5、bfs 4.1.5、Core 0.1.2、CMeta 0.1.3；工具/源码和原始数据哈希见两个 JSON。热 OS 缓存不受控，版本间调度、温度和缓存差异没有消除。

## 本机结果

下表为未启用计时 scope 的每组六次任务墙钟中位数；单位 ms。

| 语料 | 匹配 | 标量 1 | ASCII 字扫描 1 | 标量 2 | ASCII 字扫描 2 |
| --- | --- | ---: | ---: | ---: | ---: |
| small | anchor | 165.516 | 155.407 | 84.286 | 82.926 |
| small | needle | 153.545 | 153.047 | 79.054 | 78.028 |
| small | absent | 144.339 | 141.598 | 74.455 | 73.339 |
| large | anchor | 33.471 | 17.365 | 19.575 | 10.441 |
| large | absent | 34.405 | 16.870 | 19.926 | 9.009 |

大文件 anchor、单文件并行度、启用计时的 16 MiB 操作，各时间总和中位数：

| 阶段 | 原标量 ms | ASCII 字扫描 ms |
| --- | ---: | ---: |
| file_ns | 33.002 | 16.967 |
| worker_cpu_ns | 24.772 | 8.010 |
| text_ns | 18.352 | 1.338 |
| read_ns | 2.220 | 2.074 |
| poll_ns | 8.146 | 8.992 |
| write_ns | 3.427 | 3.655 |
| finish_ns | 0.028 | 0.026 |
| decode_ns | 0.237 | 0.225 |

这组数据支持将大文件校验成本作为本轮优化目标；不支持宣称所有负载同比例加速。小文件仍主要等待工具，整体时间有波动。profile/未计时的比例有时小于 1，体现采样噪声，不能据此认为加计时会加速。默认并行度保持 1。

## 原始数据

- before：[逐操作 CSV](content-ascii-before-2026-11-05-linux-release.csv)、[逐查询 CSV.gz](content-ascii-before-2026-11-05-linux-release-queries.csv.gz)、[汇总/指纹](content-ascii-before-2026-11-05-linux-release-summary.json)
- after：[逐操作 CSV](content-ascii-after-2026-11-05-linux-release.csv)、[逐查询 CSV.gz](content-ascii-after-2026-11-05-linux-release-queries.csv.gz)、[汇总/指纹](content-ascii-after-2026-11-05-linux-release-summary.json)

查询 CSV 只做无损 gzip 压缩；解压后 SHA256 对应 JSON.raw_sha256 中的 content-benchmark-queries.csv。CI artifact 继续保存未压缩 CSV、v3 严格汇总及完整测试日志。新汇总器校验计数、阶段边界、轮转次序和结果一致性。v1/v2 的配套源码仍分别在 PR #11/#12 对应提交中。

## 剩余范围

冷缓存、十万文件、直接批量 ugrep、完整父子 CPU/RSS、密集 Unicode 的性能分布、长时生产查询尾延迟及 Windows 阶段计时仍未验收。优化没有修改文件身份检查、管道生命周期、取消、文件/输出/记录限制或默认并行度。
