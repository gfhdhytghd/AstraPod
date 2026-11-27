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

每条 `route=` 摘要提供 `period_us`、`interval_p99_f`、
`interval_worst_f` 和 `interval_misses`。用帧间隔乘以 `period_us`
换算微秒。按相同主题、路由和刷新倍率比较。`database_frames` 和
`art_frames` 是对应后台任务处于忙状态时采到的帧数。
`pcm_chunks` 大于零表示采样期间播放消耗了 PCM 数据。
PCM 欠载计数覆盖曲中耗尽缓冲的事件。正常曲尾和主动淡出不计入。

## 无窗口模拟器

用 `PODBOX_MOTION_PROBE=1 make` 构建 iPod 6G SDL 模拟器。使用
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy PODBOX_TEST_MOTION=1`
启动模拟器。测试磁盘必须没有 `ui-perf.enable`。在标准错误流中检查
`PASS motion:`。

压力测试脚本接受两个带 PCM 和后台计数的模拟器、带主题的资源 ZIP 和输出目录。
在基线上应用 `4fb10f444e` 的测量提交，以及候选版本对
`ui/perf.c` 的后台工作计数改动。脚本逐个启动 SDL 模拟器，并保留采样和任务日志。

```sh
python3 apps-ipod/sim/animation_schedule_sim.py \
    /path/to/baseline/rockboxui /path/to/candidate/rockboxui \
    /path/to/sim/rockbox.zip /path/to/output
```
