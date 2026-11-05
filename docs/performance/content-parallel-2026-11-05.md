# 文件内容并行基准：第一组热缓存数据（2026-11-05）

这一轮证明了同一固定语料在单文件、双文件模式下结果一致，并建立了可复现的性能测量。它只完成阶段 F 的第一步，尚不能升级为全平台性能验收或默认开启双文件的依据。

## 条件与方法

- 产品基线为 `686f9d672888f7274ca2695ae610ab5e07f7db8c`（PR #10）。测量时包含本 PR 的测试/构建改动，未修改生产源文件；JSON 的 revision 明确带 `+benchmark-working-tree`。
- Linux x86_64、Clang 18.1.3、Release、无 sanitizer；进程可见逻辑 CPU 数为 9，不代表容器实际 CPU 配额。文件系统 statfs magic 为 `0x794c7630`。
- Core SDK 0.1.2、CMeta SDK 0.1.3；ugrep 7.8.5、bfs 4.1.5，工具完整版本、SHA256、编译器、源文件指纹见汇总 JSON。本机工具沿用显式绝对路径，未更新生产依赖固定版本。
- 小文件为 64 × 1 KiB；大文件为 4 × 4 MiB。都是确定性的 UTF-8 文本；`anchor` 分别产生 64/256 条记录，小文件 `needle` 产生 1,024 条记录，`absent` 无命中。
- 两种候选来源分别为固定元数据范围 scope、真实 bfs live。每种负载先各预热一次，再各测四次；两种并行度交替先后，每种占据第一个、第二个位置各两次。语料创建和索引扫描不计入内容耗时，scan_ns 单独保留。
- 请求沿用 10,000 条全局记录上限，显式设置每文件输出预算 64 KiB，其余工厂限制不变。完成用例逐文件检查重复、行号、UTF-8 字节偏移和完整文本；非命中也核对候选、扫描及进程总数。
- 同一 engine 的独立线程每次完成名字查询后休眠 1 ms；查询 `file-`、结果窗口 1，同时核对全量计数与固定 epoch。每个语料先采样约 50 ms 无内容负载的基线。仅把完整落在内容操作窗口内的查询纳入该负载分位数；跨边界和窗口外样本单独计数。
- 内容 wall time 包括结果核对与采样开销，不是裸 ugrep 匹配时间；采样峰值是下界。取消在获得第一个已确认文件批次时开始，量到任务终态，覆盖工作线程及原生进程回收。

## 本机 Release 结果

时间为四次测量的中位数。耗时比为单文件中位数 ÷ 双文件中位数，仅描述这一轮数据。

| 语料 | 来源 | 模式 | 单文件 ms | 双文件 ms | 耗时比 |
| --- | --- | --- | ---: | ---: | ---: |
| small | scope | anchor | 161.629 | 87.784 | 1.84× |
| small | scope | needle | 166.010 | 88.356 | 1.88× |
| small | scope | absent | 159.431 | 82.067 | 1.94× |
| small | live | anchor | 163.582 | 83.988 | 1.95× |
| small | live | needle | 167.984 | 81.722 | 2.06× |
| small | live | absent | 148.437 | 76.906 | 1.93× |
| large | scope | anchor | 35.558 | 19.753 | 1.80× |
| large | scope | absent | 34.791 | 17.859 | 1.95× |
| large | live | anchor | 38.005 | 19.383 | 1.96× |
| large | live | absent | 32.777 | 21.573 | 1.52× |

双文件在这轮所有负载中总耗时更短；名字查询尾延迟没有同步改善。例如小文件 scope/anchor：

| 并行度 | 窗口内查询数 | P50 ms | P95 ms | P99 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 530 | 0.063 | 0.213 | 1.799 |
| 2 | 253 | 0.069 | 1.706 | 2.588 |

短任务的观察窗口更短，两个模式的查询样本数量也不同。P50/P95/P99 都按 nearest-rank 计算，只是样本描述；不能由四次任务测量推导生产 P99 保证。默认并行度继续为 1。

## 数据与复现

本页保存 v1 数据，配套源码/汇总器为 `95f26c4bcfab5906ad042cb534c7b36d351e2ecb`。
最新 v2 的 POSIX 直接 ugrep 对照见[独立报告](content-direct-control-2026-11-05.md)。

- [逐操作原始 CSV](content-2026-11-05-linux-release.csv)
- [逐查询原始 CSV](content-2026-11-05-linux-release-queries.csv)
- [机器可读汇总与工具/源码哈希](content-2026-11-05-linux-release-summary.json)

在提供固定 Core/CMeta/host-tool 根目录后，使用版本化配置：

```sh
cmake --preset linux-content-release
cmake --build --preset linux-content-release
ctest --preset linux-content-release
python tools/content_benchmark_report.py \
  build/linux-content-release/tests/content-benchmark.csv \
  build/linux-content-release/tests/content-benchmark-queries.csv \
  --revision "$(git rev-parse HEAD)" --capture-tools \
  --output build/linux-content-release/tests/content-benchmark-summary.json
```

所有启用内容测试的平台都会运行这一个正式原生基准。POSIX 有 110 次操作，Windows 只有 scope，共 56 次；无可用 BFS_LIVE 时不代入其他遍历器。CI 增加 Linux Release，原有六组 Debug/sanitizer 配置用于正确性回归，分别上传 CSV、JSON 与 LastTest.log。它们不能合并成一个性能总体；计时没有通过阈值。

## 尚未验收

冷缓存、十万文件、长时多轮与更真实的查询分布、直接 ugrep 启动/批处理对照、独立候选/启动阶段计时、父子 CPU/RSS/峰值内存、慢消费者性能、读取中与无进展子进程的取消分布，以及扫描/监听的独立性能基准仍待进行。当前 managed allocation 采样不等于 RSS，查询延迟数据也不能替代 NTFS 索引速度。
