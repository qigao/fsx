# 原生内容后端：同机热缓存对照（2026-11-27）

本次固定语料中，嵌入式 matcher 与 bftw 后端降低了内容任务的墙钟中位数，小文件差距尤其明显。大文件 native 路径使用更多受管内存；并发元数据查询的个别尾延迟也有回退。这组结果支持继续验证原生后端，不构成所有场景加速或跨机器性能验收。

## 条件、版本与结果核对

- Linux x86_64、内核 6.18.57、可见 9 个逻辑 CPU，Clang 19.1.7；`linux-content-release`，ASan/UBSan/TSan 均关闭。OS 缓存已预热但未受控，全部对照来自同一台机器、同一次运行。
- `small` 为 64 × 1 KiB，`large` 为 4 × 4 MiB。固定 UTF-8 语料包含 `anchor` 稀疏命中、`needle` 密集命中（仅 small）和 `absent` 无命中。每文件身份、行号、字节偏移、完整文本和重复文件均由现有 oracle 核对；两种 native source 均要求零子进程。
- `scope` / `native_scope` 使用相同索引快照与 `file:` 选择；`live` / `native_bfs` 分别使用外部 bfs + ugrep、嵌入式 bftw + matcher。文件身份、文本验证、读后检查和结果背压保留。它们比较完整后端，差额不能全部归为进程启动或遍历成本。
- 外部工具为 ugrep 7.8.5、bfs 4.1.5，Core 0.1.2、CMeta 0.1.3；原生 literal matcher 来自固定版本的 ugrep RE/flex。工具绝对路径、版本、SHA256、源码及 vendor 指纹在配套 JSON 中。
- 每个组合先预热。scope/native_scope/direct/profile 的 p1/p2 共八种组合测八轮，每种占据八个执行位置各一次；live/native_bfs 的 p1/p2 共四种组合测四轮，同样轮转。原有 direct、profile、基线和取消测量完整保留。v4 汇总器接受 **478 次 operation**，校验输入、记录、计数、轮转和源码指纹。下表使用未启用私有阶段计时的配对后端。
- 归档版本为 `744fbb22c3a9611dbb3d22aed08cd4738af9a9b1`。实际采集时 native 主体源码已完成；随后新增全局 record limit 测试并修正 Windows 读错误分类，均不改变此次 Linux 性能路径。归档没有重跑性能：JSON 的 `revision` 和源码指纹对应归档时提交，编译时嵌入的 benchmark 源码指纹另经严格核对。

## 任务墙钟中位数

p1/p2 分别表示一个/两个文件 worker，单位 ms。每列是该组合独立样本的中位数，按相同语料成对列示；不是逐轮比值的中位数。时间包含输出核对和观察器开销。

| 语料 | 模式 | scope p1 | native_scope p1 | scope p2 | native_scope p2 |
| --- | --- | ---: | ---: | ---: | ---: |
| small | anchor | 138.601 | 2.416 | 72.210 | 1.665 |
| small | needle | 133.127 | 2.521 | 70.397 | 1.795 |
| small | absent | 130.314 | 1.211 | 68.622 | 1.020 |
| large | anchor | 24.600 | 11.786 | 9.187 | 7.098 |
| large | absent | 16.349 | 4.311 | 9.364 | 3.055 |

| 语料 | 模式 | live p1 | native_bfs p1 | live p2 | native_bfs p2 |
| --- | --- | ---: | ---: | ---: | ---: |
| small | anchor | 139.120 | 2.542 | 74.592 | 2.121 |
| small | needle | 141.879 | 2.860 | 72.077 | 2.547 |
| small | absent | 136.827 | 1.071 | 71.215 | 1.245 |
| large | anchor | 20.505 | 13.333 | 11.348 | 6.585 |
| large | absent | 17.323 | 4.453 | 9.840 | 2.559 |

两个 worker 并非总是更快：例如 native_bfs 的 small/absent 中位数从 1.071 ms 增至 1.245 ms。默认并行度保持 1，未设置速度验收阈值。

## 并发元数据查询尾延迟

同一 engine 在内容任务期间持续发起名字查询。仅统计开始和结束均处于该 operation 内的观察；跨边界和外部样本另计，P95/P99 使用 nearest-rank。以下列出 anchor 的配对样本，单元格为 **样本数 / P95 ms / P99 ms**；其他模式完整保存在 JSON。原生任务更短，查询样本明显更少，不能把这些短时 P99 当成生产尾延迟保证。

| 语料 | 配对 | worker | 外部后端 | native 后端 |
| --- | --- | ---: | ---: | ---: |
| small | scope / native_scope | 1 | 944 / 0.295 / 1.551 | 20 / 0.065 / 0.107 |
| small | scope / native_scope | 2 | 452 / 1.214 / 1.814 | 16 / 0.111 / 0.111 |
| large | scope / native_scope | 1 | 177 / 0.093 / 0.159 | 83 / 0.068 / 3.138 |
| large | scope / native_scope | 2 | 66 / 0.611 / 2.027 | 53 / 0.182 / 0.644 |
| small | live / native_bfs | 1 | 495 / 0.307 / 1.283 | 11 / 0.195 / 0.195 |
| small | live / native_bfs | 2 | 241 / 0.858 / 1.569 | 9 / 0.213 / 0.213 |
| large | live / native_bfs | 1 | 85 / 0.109 / 1.041 | 46 / 0.087 / 0.099 |
| large | live / native_bfs | 2 | 37 / 1.205 / 1.215 | 22 / 0.067 / 2.362 |

无内容任务的约 50 ms 基线中，small 有 44 个查询、P99 0.100 ms；large 有 45 个、P99 0.177 ms。large/anchor native_scope p1 的 P99 为 3.138 ms，native_bfs p2 为 2.362 ms，说明本轮不能宣称查询尾延迟全面改善。

## 内存与适用范围

large 测量的 `sampled_owned_increase_bytes`，native_scope 约为 p1 **4.510–4.511 MiB**、p2 **8.874–8.875 MiB**；外部 scope 为 **0.511–0.589 MiB**、**0.875 MiB**。native_bfs 约为 **4.499–4.500 MiB**、**8.875 MiB**。这些值是各模式采样峰值减本轮引擎基线，反映 native 全文件缓冲等受管分配，并非完整瞬时峰值。

受管字节不等于 RSS：SQLite、线程栈、外部工具内存，以及 RE/flex 上游 C++ 容器/编译和初始 matcher 缓冲的分配不计入 `owned_memory_budget`。FSX 自有输入、输出、候选队列、bftw 分配和 PCRE2 allocator 请求按运行时契约计量。不能据上述数字推断总进程内存降低。

本轮未验收冷缓存、十万文件、密集 Unicode、PCRE2 性能分布、批量 ugrep、完整父子 CPU/RSS、长期查询尾延迟或其他 OS/硬件。取消从首个已确认文件批次触发，不能替代挂起 I/O/子进程场景。不同机器、sanitizer 和 hosted CI profile 应分别分析。

## 原始证据

- [逐操作 CSV](native-content-2026-11-27-linux-release.csv)
- [逐查询 CSV.gz](native-content-2026-11-27-linux-release-queries.csv.gz)
- [v4 汇总、工具与源码指纹](native-content-2026-11-27-linux-release-summary.json)

CSV 按原字节保存；查询 CSV 仅作无损 gzip 压缩，gzip mtime 为 0。解压后的 SHA256 对应 JSON `raw_sha256` 中的 `content-benchmark-queries.csv`，逐操作 CSV 对应 `content-benchmark.csv`。本次使用现有 `tools/content_benchmark_report.py`、`--revision 744fbb22c3a9611dbb3d22aed08cd4738af9a9b1 --capture-tools` 汇总已完成的 Release 原始数据。
