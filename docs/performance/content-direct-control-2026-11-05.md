# 直接 ugrep 对照：热缓存固定语料（2026-11-05）

本轮增加 POSIX 每文件原生 ugrep 对照，保留 FSX 完整运行时契约。小文件两条链路的总耗时接近，大文件存在明显差距，下一步值得分解 FSX 的读取、UTF-8 校验与 stdin 输送成本；当前数据不能将差距归因于某个单独阶段。

## 条件与语义

- 产品基线 `95f26c4bcfab5906ad042cb534c7b36d351e2ecb`（PR #11）；生产源文件未修改。测量包含本 PR 的测试/报告改动，revision 标记 `+direct-control-working-tree`，JSON 记录编译源码、工具与原始数据 SHA256。
- Linux x86_64、Clang 18.1.3、Release、无 sanitizer，ugrep 7.8.5、bfs 4.1.5，Core 0.1.2、CMeta 0.1.3。宿主和文件系统标识见 JSON；这不是受控冷缓存测量。
- 语料沿用首轮：64 × 1 KiB 小文件、4 × 4 MiB 大文件，稀疏/密集/无命中模式。每条链路每文件都启动一个 `ugrep -J1 -F`，使用同一 JSON 格式和 C locale；复用生产 JSON 解码器及逐行号、字节偏移、完整文本的核对。
- scope 单文件、scope 双文件、direct 单文件、direct 双文件各预热一次，再测四轮；每条链路占据四个先后位置各一次。同一 engine 继续并发采样名字查询。BFS_LIVE 保留独立的单/双文件对照。
- POSIX 总计 160 次操作；Windows 仍是 56 次 scope 操作，本轮没有 Windows direct 对照。所有内容配置仍是 7 个正式 CTest 条目。

| 行为 | FSX scope | POSIX direct 测试对照 |
| --- | --- | --- |
| 候选 | 固定提交快照中的查询结果 | 受控夹具的已知文件列表 |
| 输入 | FSX 验证对象与文本，再通过非阻塞 stdin 管道输送 | ugrep 按路径自行重开文件 |
| 输出 | 双管道并发排空，受预算约束的文件批次 | 临时普通文件，子进程退出后读取与核对 |
| 完成 | 包含读取后版本/路径关联检查、背压、取消与原生回收 | 阻塞 waitpid；受控不变夹具，无 FSX 版本检查或协作取消 |
| 结果槽 | 各模式分别最多 1/2 个 | 各工作线程最多暂存一个文件，同样分配 64 KiB 输出及 10,000 个记录视图 |

普通输出文件与路径输入可能改变工具 I/O 行为。direct 只是测试对照，不提供生产入口、替代后端或降级路径。临时输出的 64 KiB 检查发生在进程退出后，不宣称运行中磁盘硬配额；夹具与工具版本固定。direct 子进程继承测试进程组，测试总超时由 CTest 负责，未接入 FSX 的取消 API。

## 本机 Release 结果

以下均为四次操作 wall time 的中位数，包含输出核对与观察器开销。

| 语料 | 模式 | FSX 1 ms | direct 1 ms | FSX 2 ms | direct 2 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| small | anchor | 160.125 | 158.962 | 89.408 | 83.693 |
| small | needle | 154.288 | 158.873 | 83.536 | 80.672 |
| small | absent | 147.800 | 148.257 | 76.732 | 76.016 |
| large | anchor | 39.910 | 14.603 | 23.515 | 6.972 |
| large | absent | 37.413 | 13.473 | 24.154 | 7.345 |

小文件的单文件两条链路相近，因此目前没有证据把小文件成本主要归因于 FSX 的退出轮询。大文件 direct 更快，但它同时绕过了 FSX 的输入检查/复制/管道及读取后验证；这不是相同完整语义的替换方案，也不能把差额直接命名为“启动成本”。默认并行度仍为 1。

## 阶段计时

- `spawn_call_ns`：只包围 posix_spawn 调用，不包括动作/属性配置；它不是“已经准备匹配”的时间。
- `child_lifetime_ns`：从 spawn 调用前到 waitpid 成功回收，已经包含 spawn、匹配和退出。
- `collect_decode_ns`：回收后收集输出、解码并完成文本核对；不包含最后 fclose/free。
- CSV 的三个字段均累加所有文件；双工作线程时间会重叠，不能把它们当成 wall time 的互斥阶段。JSON 的 per-process 值是每轮总和除以进程数，再描述四个轮均值的分布，不是单个子进程样本的 P99。
- direct 的首结果记作 `first_checked_output_ms`，只确认工具输出正确；FSX 保留 `first_batch_ms`，还包含原有文件版本检查。direct 的进程峰值为精确的已接纳未回收根进程数，FSX 计数仍是采样下界。

| 单文件 direct | spawn 调用均值 ms | 子进程存活均值 ms | 收集/解码均值 ms |
| --- | ---: | ---: | ---: |
| small/anchor | 0.1466 | 2.4119 | 0.0120 |
| large/anchor | 0.1283 | 3.2948 | 0.0709 |

## 数据与复现

- [逐操作 CSV](content-direct-2026-11-05-linux-release.csv)
- [逐查询 CSV](content-direct-2026-11-05-linux-release-queries.csv)
- [汇总、工具/源码指纹](content-direct-2026-11-05-linux-release-summary.json)

使用 `linux-content-release` configure/build/test presets，之后按 README 运行 `tools/content_benchmark_report.py`。新测量格式为 v2，汇总器严格验证四链路轮转、记录总数、阶段计时边界和输入一致性。首轮 v1 数据及配套汇总器保留在 PR #11 的精确提交中。

## 后续工作

优先独立测量 FSX 大文件读取、UTF-8 校验、stdin 管道输送和退出回收的 CPU/墙钟成本，再选择优化点。十万文件、受控冷缓存、直接批量 ugrep、父子 CPU/RSS、长时查询尾延迟，以及 Windows direct 对照仍未验收。当前没有修改生产管道、取消行为或默认参数。
