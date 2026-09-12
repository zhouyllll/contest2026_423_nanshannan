# 自检命令模板

一个可以直接抄的骨架。它的每一条都对应一次真实的返工。

```c
int probe_main(int argc, char *argv[])
{
  struct timespec deadline;
  int  seconds = 2;                 /* 1. 参数化，默认值保守 */
  int  got = 0, total = 0;
  int  ret = ERROR;
  int  fd = -1;

  if (argc > 1) seconds = atoi(argv[1]);
  if (seconds < 1 || seconds > 10) seconds = 2;   /* 夹住，别信输入 */

  fd = open(DEVPATH, O_RDWR);
  if (fd < 0)
    {
      /* 5. 打观测值，不是"失败了" */
      printf("打不开 %s: errno=%d\n", DEVPATH, errno);
      return ERROR;
    }

  /* 2. 整体墙钟上界。不是每次等待的上界 —— 是整件事的上界。 */
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += seconds * 3 + 5;

  while (total < want)
    {
      struct timespec now, ts;

      /* 4. 每轮开头检查 deadline */
      clock_gettime(CLOCK_REALTIME, &now);
      if (now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
          printf("总时限到（已收 %d 个 %d 字节）—— 跟不上速率\n", got, total);
          break;
        }

      /* 3. 每一步的等待都用 timed 版本 */
      ts = now; ts.tv_sec += 2;
      if (mq_timedreceive(mq, buf, sizeof(buf), &prio, &ts) != sizeof(buf))
        {
          printf("等待超时（已收 %d 个）—— 没有产出数据\n", got);
          break;
        }

      /* ... 处理 ... */
      got++;
    }

  ret = OK;

  /* 6. 成功失败走同一条清理路径 */
  if (fd >= 0) close(fd);

  /* 7. 结论用阈值判定，别让人去看数字猜 */
  if (got == 0)        printf("结论：没有数据\n");
  else if (peak < 32)  printf("结论：静默（peak=%d）\n", peak);
  else if (peak < 512) printf("结论：偏弱（peak=%d）\n", peak);
  else                 printf("结论：正常（peak=%d avg=%d）\n", peak, avg);

  return ret;
}
```

## 为什么是这七条

| # | 省掉它的代价 |
|---|---|
| 1 | 每换一组参数就要重烧一次板子 |
| 2 | 产出不来时命令永不返回，控制台没了，只能物理复位 |
| 3 | 单点阻塞，同上 |
| 4 | 有 deadline 但不检查 = 没有 deadline |
| 5 | 下一轮还得再烧一次板子才知道发生了什么 |
| 6 | 失败路径漏关设备，第二次运行行为不同，误导后续判断 |
| 7 | 一堆数字，每次都要重新想阈值；换个人看就下不了结论 |

## 判定阈值怎么定

别拍脑袋。**先在已知正常的条件下跑一次，记下数值**，再据此定阈值。
没有已知正常的基线时，至少把"零"和"非零"分开，并在输出里说明
"此阈值未经基线校准"。

> 一个永远返回"通过"的自检，比没有自检更糟 —— 它会让你在错误的方向上
> 继续深挖。写完自检后**故意制造一次失败**（拔掉线、改错地址），
> 确认它真的会报错。
