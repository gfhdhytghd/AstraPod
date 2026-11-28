# iPod Classic 6G 动画调度验收

## 真机步骤

1. 备份当前固件和设置。安装候选固件。
2. 保存原有的 `/.rockbox/ui-perf.txt`。新建空文件
   `/.rockbox/ui-perf.enable`。重启 iPod。
3. 播放一首至少三分钟的本地 FLAC。保持同一首歌，避免切歌干扰。
4. 在调试菜单打开「Buffers and CPU」。记录 `PCM underruns`。
5. 发起数据库更新。滑动首页长列表。进入 Cover Flow 后连续转轮十秒。
6. 发起封面缓存更新。重复列表和 Cover Flow 操作。
7. 反复打开并退出 Now Playing、分栏页面和弹层。重复一次按键中断动画。
8. 回到「Buffers and CPU」。记录 `PCM underruns` 的增量。
9. 等待后台工作完成。用后台任务页面确认扫描和封面缓存恢复。
10. 导出 `/.rockbox/ui-perf.txt`。移除 `ui-perf.enable`。重启 iPod。
11. 重复第 3 至 9 步，确认关闭日志时的可见效果。
12. 在同一台 HDD 机型上重复一次。等硬盘休眠后发起非紧急重扫。
13. 在同一轮动画中换到文件缓冲尚未填满的曲目。核对 PCM 欠载增量。
14. 动画结束后观察后台重扫和缓存是否继续。

每条 `route=` 摘要提供 `period_us`、`interval_p99_f`、
`interval_worst_f` 和 `interval_misses`。用帧间隔乘以 `period_us`
换算微秒。按相同主题、路由和刷新倍率比较。`database_frames` 和
`art_frames` 是对应后台任务处于忙状态时采到的帧数。
`pcm_chunks` 大于零表示采样期间播放消耗了 PCM 数据。
CSV 中 `runnable_wait_us` 是 UI 可运行时的排队时间。
`button_us` 和 `button_calls` 是两个提交点之间按键队列调用的总耗时和次数。
`sleep_us` 和 `queue_us` 分别记录内核睡眠和无继承队列等待。
`ready_us` 是最长等待中 UI 从就绪到实际运行的时间。
`wait_site` 是相对于 `motion_sched_watch` 的十六进制地址偏移。用同一构建的符号表定位。
`runner` 和 `runner_us` 记录最长等待中连续占用调度的线程及时间。
Linux 模拟器另记该线程的实际 CPU 时间 `runner_cpu_us`。
`db_busy`、`art_busy`、`pcm_active` 和 `pcm_chunks` 记录逐帧重叠。
`lock_wait_us` 是支持优先级继承的等待。`lock_site` 是最后一个该类阻塞点。
`other_wait_us` 包含按键和其他阻塞。`action_wait_us` 为最长一次按键等待。
PCM 欠载计数覆盖曲中耗尽缓冲的事件。正常曲尾和主动淡出不计入。

## 无窗口模拟器

用 `PODBOX_MOTION_PROBE=1 make` 构建 iPod 6G SDL 模拟器。使用
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PODBOX_TEST_MOTION=1`
启动模拟器。测试磁盘必须没有 `ui-perf.enable`。在标准错误流中检查
`PASS motion:`。

压力测试脚本接受两个启用 `PODBOX_MOTION_PROBE=1` 的模拟器、主题资源 ZIP 和输出目录。
两个模拟器必须支持逐帧负载计数及模拟器的播放和后台任务触发。
脚本等待实际后台任务开始，然后送入按键。
脚本逐个运行 SDL 模拟器，并保留帧记录和任务日志。
`acceptance.json` 汇总比较结果。每个负载和路由必须记录到同时播放的帧。
`--vsync-off` 用于单独检查 SDL 垂直同步对帧间隔的影响。

```sh
python3 apps-ipod/sim/animation_schedule_sim.py \
    /path/to/baseline/rockboxui /path/to/candidate/rockboxui \
    /path/to/sim/rockbox.zip /path/to/output
```
