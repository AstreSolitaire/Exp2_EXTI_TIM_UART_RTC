# 实验二：按键外部中断、定时器与 RTC 参数设置

> 课程：微处理器结构与嵌入式系统设计
> 硬件：STM32F407G-DISC1（STM32F407VGT6，LQFP100，8 MHz 晶振）
> 工具：STM32CubeMX 6.15.0 + Keil MDK 5.29（ARMCC 5.06）+ STM32Cube FW_F4 V1.28.0
> 许可：[MIT License](LICENSE)

本工程在实验一基础上增加了 **EXTI 外部中断**、**TIM 定时中断**、**串口数据帧协议解析**，
并进一步把串口收发升级为 **DMA + 环形缓冲**；已编译通过（0 Error / 0 Warning）并完成上板实测，
`MDK-ARM/Objects/` 下含 `Exp2_EXTI_TIM_UART_RTC.hex` / `.bin`。

## 运行效果（实拍与实测）

**⓪ 模块讲解视频**（3 分 59 秒，中文旁白，逐模块讲架构与代码）：

▶ [`docs/media/exp2_explainer.mp4`](docs/media/exp2_explainer.mp4)

| 时间 | 内容 |
|---|---|
| 00:00 | 封面与工程概览 |
| 00:13 | 总体架构：四个中断 + 一个主循环 |
| 00:42 | 系统时钟 168 MHz（PLL 推导、Scale1、5WS） |
| 01:07 | GPIO 与 EXTI0 按键中断（菱形 LED 排布、三级链路） |
| 01:32 | TIM3 250 ms 定时（分频计算、顺/逆时针） |
| 01:56 | USART2 与 DMA（FIFO 链式发送、RX 空闲中断） |
| 02:23 | RTC（LSI 分频、读时序、星期计算） |
| 02:49 | 串口帧协议与解析（状态机、sscanf、校验） |
| 03:16 | 主循环与并发（标志位、volatile） |
| 03:36 | 验证与交付 |

**① 硬件实拍**：复位/按键后 LED 跑马灯按 250 ms 节拍绕圈运行

▶ [`docs/media/hardware_demo.mp4`](docs/media/hardware_demo.mp4)

**② 串口功能验证录屏**（50 s，监测软件内逐条发送并观察回执）：

▶ [`docs/media/exp2_verify.mp4`](docs/media/exp2_verify.mp4)

录屏中按时间顺序完成 5 项验证，画面内可直接看到发送框输入、`[TX]`/`[RX]` 回显与 RTC 变化：

| 步骤 | 发送内容 | 期望现象 | 实测 |
|---|---|---|---|
| 1 | `$0,1,251107,140101*` | `[RX]` 回显 + `[RTC] updated -> 2025-11-07 14:01:01`，随后秒行变为 14:01:0x | ✅ |
| 2 | `$0,1,251231,235959*` | 改到 2025-12-31 23:59:59，随后跨年滚动到 2026-01-01 00:00:00 | ✅ |
| 3 | `$0,1,251107,999999*` | 秒值非法 → `[RX] ignored`，时间不变 | ✅ |
| 4 | `$0,2,251107,120000*` | 命令字非法 → `[RX] ignored` | ✅ |
| 5 | `hello` | 帧外杂散字符，无任何解析动作 | ✅ |

按键中断（EXTI0）需在板上按 **B1** 才能触发，属于硬件侧验证：按下后串口输出
`[EXTI0] B1 pressed -> CLOCKWISE / COUNTER-CLOCKWISE` 且灯具绕行方向立即反向。

---

## 一、实验要求与实现对照

| 实验要求 | 实现位置 | 实现方式 |
|---|---|---|
| 按键输入**中断**，中断中输出中断信息到串口 | `main.c` → `HAL_GPIO_EXTI_Callback()`；`gpio.c`（PA0 配 `GPIO_MODE_IT_RISING`） | B1(PA0) 上升沿触发 EXTI0 中断；中断里切换方向并生成 `[EXTI0] B1 pressed -> ...` 字符串，主循环发送 |
| 定时器按指定时间控制 LED 循环点亮 | `tim.c` → `MX_TIM3_Init()`；`main.c` → `HAL_TIM_PeriodElapsedCallback()` | TIM3：84 MHz/8400 = 10 kHz，计 2500 次 → **250 ms** 更新中断，每次走一步跑马灯 |
| 按键中断控制 LED3~LED6 **正转/反转**切换 | `main.c` → `Led_Marquee_Step()` | 按**物理位置绕圈**：顺时针 LD3(上)→LD5(右)→LD6(下)→LD4(左)（序号+1）；逆时针反向（序号−1）；每次按键切换 |
| 串口发送时间参数帧，ARM 中断接收并解析，更新 RTC | `main.c` → `Frame_RxByte()` / `Frame_DecodeSetRtc()`；`usart.c` 接收中断 | 帧格式 `$0,1,YYMMDD,HHMMSS*`；逐字节状态机收帧，`sscanf` 解析，校验范围后写 RTC 时间/日期（含星期计算） |
| 上传 RTC 数据验证是否改对 | `rtc.c` → 秒中断回调 + `main.c` 发送 | RTC 1 Hz 唤醒中断格式化时间字符串，主循环经 USART2 每秒上传 |

附加：修改成功后立刻回显 `[RTC] updated -> 2025-11-07 14:01:01`，便于当场核对。

---

## 二、目录结构

```
Exp2_EXTI_TIM_UART_RTC/
├─ Exp2_EXTI_TIM_UART_RTC.ioc     CubeMX 配置（EXTI0 / TIM3 / USART2 / RTC）
├─ Core/
│  ├─ Inc/  main.h  gpio.h  usart.h  rtc.h  tim.h  stm32f4xx_it.h  stm32f4xx_hal_conf.h
│  └─ Src/  main.c  gpio.c  usart.c  rtc.c  tim.c
│           stm32f4xx_hal_msp.c  stm32f4xx_it.c  system_stm32f4xx.c
├─ Drivers/                       CMSIS + STM32F4xx HAL（CubeF4 V1.28.0）
├─ MDK-ARM/
│  ├─ Exp2_EXTI_TIM_UART_RTC.uvprojx
│  └─ Objects/  .axf / .hex / .bin / .map
├─ tools/                        串口监测 GUI + 命令行版
└─ docs/工程详解.md               逐行注释与设计说明
```

---

## 三、与实验一的差异（新增的三块）

| 项目 | 实验一 | 实验二 |
|---|---|---|
| 按键 | 主循环 `Key_Scan()` 轮询 + 20 ms 软件消抖 | **EXTI0 硬件中断**（上升沿），无需轮询 |
| LED 节拍 | 主循环 `HAL_GetTick()` 差值定时 | **TIM3 更新中断**（250 ms），硬件定时更准 |
| 串口 | 逐字节回显、轮询发送（阻塞） | **数据帧协议** + **DMA 收发**（见第四节） |
| 逻辑解耦 | — | 中断只做“快动作”（改状态、生成字符串/标志），串口发送放主循环 |

### 串口 DMA 收发（本工程相对实验一的增强）

```
printf/fputc ─┐
RTC 秒时间串 ─┼→ 512 B 环形 FIFO → DMA1_Stream6 → USART2->DR（硬件搬字节）
按键中断信息 ─┘                    ↑
                    每块传完 TxCplt 中断里继续取 FIFO，形成链式发送

PC → PA3 → DMA1_Stream5 收进 64 B 缓冲 → 总线空闲(IDLE)中断 → 帧状态机
```

| 项 | 参数 |
|---|---|
| TX | DMA1 Stream6 Channel4，块大小 128 B，FIFO 512 B |
| RX | DMA1 Stream5 Channel4，缓冲 64 B，`HAL_UARTEx_ReceiveToIdle_DMA` |
| 效果 | `printf`/发送调用**立即返回**，不再阻塞 23 ms/帧；CPU 占用几乎为 0 |

两个必须注意的坑（都已修正，详见 `docs/工程详解.md` 第五节）：

1. **`HAL_UART_GetState()` 返回 `gState | RxState` 的组合值**——RX 常开 DMA 时它永远不等于 `READY`，用它判断“能否发”会导致 TX 一次都不启动。必须只看 `huart2.gState`。
2. **DMA 初始化必须在第一次 printf 之前**——`HAL_UART_Transmit_DMA` 会解引用 `huart->hdmatx`，未初始化就调用会 HardFault。

### LED 物理排布（为什么跑马灯要按位置排序）

四颗 LED 在板上不是一排，而是菱形排列：

```
            LD3 (PD13)
 LD4 (PD12)          LD5 (PD14)
            LD6 (PD15)
```

按名字 LED3→LED4→LED5→LED6 点亮是「上→左→右→下」，视觉上是 **Z 字形**。
本工程按物理位置排成环 `s_ledRing = {LD3, LD5, LD6, LD4}`，序号 +1 即「上→右→下→左」= 顺时针，
序号 −1 = 逆时针。要求里的"正转/反转"就对应这两个方向。

---

## 四、关键配置

### 4.1 EXTI0（PA0 / B1）

```
PA0: GPIO_MODE_IT_RISING + GPIO_PULLDOWN     ← 按下为高 → 上升沿触发
NVIC: EXTI0_IRQn 抢占优先级 1
链路: 引脚电平跳变 → EXTI 边沿检测 → NVIC → EXTI0_IRQHandler
      → HAL_GPIO_EXTI_IRQHandler(B1_Pin) 清挂起位 → HAL_GPIO_EXTI_Callback(B1_Pin)
```

中断服务里做两件事：`s_dirReverse` 取反；`snprintf` 生成打印字符串并置标志。
发送留给主循环——9600 bps 下 40 字符要约 42 ms，在中断里直接发会拖慢定时器节拍。

### 4.2 TIM3（250 ms）

```
TIM3 挂在 APB1；APB1 分频 ≠1 时定时器时钟 = PCLK1 × 2 = 84 MHz
PSC = 8399  → 84 MHz / 8400 = 10 kHz（计数频率）
ARR = 2499  → (2499+1) / 10 kHz = 250 ms
要求区间 200~500 ms：改 ARR 即可
    200 ms → ARR = 1999
    300 ms → ARR = 2999
    500 ms → ARR = 4999
```

### 4.3 串口帧协议

| 字段 | 含义 | 示例 |
|---|---|---|
| `$` | 帧头 | `$` |
| 地址 | 设备地址（本实验未使用，仅解析） | `0` |
| 命令 | `1` = 设置时间 | `1` |
| 年月日 | YYMMDD | `251107` → 2025-11-07 |
| 时分秒 | HHMMSS | `140101` → 14:01:01 |
| `*` | 帧尾 | `*` |

完整帧：`$0,1,251107,140101*`

解析用 `sscanf(buf, "%d%*[ ,]%d%*[ ,]%d%*[ ,]%d", ...)`，`%*[ ,]` 跳过逗号/空格；
随后 `YYMMDD / 10000`、`%10000/100`、`%100` 拆分年月日（同课件方法），
再用 Sakamoto 公式补算星期（RTC 日期寄存器必须有星期字段）。

---

## 五、编译与烧录

1. Keil 打开 `MDK-ARM/Exp2_EXTI_TIM_UART_RTC.uvprojx`，`F7` 编译（0 Error / 0 Warning）。
2. 烧录二选一：
   - 把 `MDK-ARM/Objects/Exp2_EXTI_TIM_UART_RTC.bin` 拖到 `DIS_F407VG` 盘（DAPLink）；
   - Keil `F8`（首次需在 Options→Debug 选 ST-Link Debugger，Port: SW）。
3. 串口线：USB 转 TTL 模块 RXD→PA2、TXD→PA3、GND→GND（红线不接），9600-8-N-1。

---

## 六、上电后的串口输出

```
**********************************************
*  STM32F407G-DISC1  Experiment 2            *
*  EXTI0 / TIM3 / USART2 frame / RTC         *
**********************************************
SYSCLK = 168000000 Hz
TIM3   : 250 ms update interrupt, LED marquee
USART2 : 9600 bps, 8 data bits, 1 stop bit, no parity
CMD    : $0,1,YYMMDD,HHMMSS*   e.g. $0,1,251107,140101*
KEY    : press B1 -> switch direction (CLOCKWISE / COUNTER-CLOCKWISE)

2025-11-07 09:00:01
2025-11-07 09:00:02
...
```

**按键测试**（每按一次切换方向，LED 与串口同步变化）：

```
[EXTI0] B1 pressed -> COUNTER-CLOCKWISE (LD3->LD4->LD6->LD5)
[EXTI0] B1 pressed -> CLOCKWISE (LD3->LD5->LD6->LD4)
```

**改时间测试**（串口助手发送，注意结尾的 `*`）：

```
发送: $0,1,251107,140101*
回显: [RX] $0,1,251107,140101*
      [RTC] updated -> 2025-11-07 14:01:01
      ...随后每秒上传的行立刻变成 14:01:02、14:01:03...
```

**非法帧处理**：

```
发送: $0,1,251107,999999*       → [RX] ignored, format: $0,1,YYMMDD,HHMMSS*
发送: abc                       → 无反应（不在帧内，字节被忽略）
```

---

## 七、实测记录（上板验证）

固件烧录后通过 SWD（CMSIS-DAP）读取芯片寄存器与内存变量核对：

| 检查项 | 读回值 | 结论 |
|---|---|---|
| `RCC_PLLCFGR` | M=8, N=336, P=2, src=HSE | SYSCLK 168 MHz |
| `RCC_CFGR` | SWS=2(PLL), PPRE1=/4, PPRE2=/2 | PCLK1 42 M / PCLK2 84 M |
| `TIM3` CR1/DIER | `CEN=1, UIE=1` | 定时器已启动且开更新中断 |
| `TIM3` PSC/ARR | 8399 / 2499 | 周期 = 2500×8400/84 MHz = **250.0 ms** |
| 顺时针采样 | 连续循环 LD3→LD5→LD6→LD4（上→右→下→左） | 顺时针绕圈正确 |
| 软件触发 EXTI0（SWIER） | `dirReverse` 0→1；`s_keyMsg="[EXTI0] B1 pressed -> COUNTER-CLOCKWISE (LD3->LD4->LD6->LD5)"` | 中断链路与中断内信息生成正确 |
| 逆时针采样 | 连续循环 LD3→LD4→LD6→LD5（上→左→下→右） | 逆时针绕圈正确 |
| 注入帧 `$0,1,251107,140101*`（USART2 内部回环） | `chRxBuf="0,1,251107,140101"`、`RTC_TR=0x00140101`、`RTC_DR=0x0025B107` weekday=5 | 帧解析、RTC 时间/日期更新、星期计算全部正确 |
| 之后逐秒读取 | RTC 14:01:13→14→15，`m_nRtcHMS` 同步 | 秒中断上传的正是新时间 |

**DMA 版本串口自检**（`tools/exp2_selftest.py`，真实 COM 口，改 DMA 后复测）：

| # | 检查项 | 结果 |
|---|---|---|
| 1 | RTC 每秒上传 | ✅ 4 秒收到 3 行连续时间 |
| 2a | 时间帧被接收并回显 | ✅ `[RX] $0,1,251107,151530*` |
| 2b | RTC 被改写 | ✅ `[RTC] updated -> 2025-11-07 15:15:30` |
| 2c | 后续秒上传为新时间 | ✅ `15:15:30`、`15:15:31` |
| 3 | 非法帧被拒绝 | ✅ `[RX] ignored` |
| 4 | 帧外杂散字符被忽略 | ✅ 发 `hello` 无解析动作 |

**6/6 通过**；复位后横幅 667 字节完整无误（`printf` 全程走 DMA）。

---

## 八、常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 按键按一下灯的方向切换两次 | 机械抖动产生多个上升沿；本实验按实验要求用硬件中断未加消抖，可在回调里加“距上次中断 ≥20 ms 才处理”的判据 |
| 发了帧但没反应 | ①结尾必须是 `*`，开头必须是 `$` ②波特率 9600-8-N-1 ③串口线 TX/RX 是否交叉 |
| 时间长跳/变慢 | RTC 用 LSI（板上无 32.768 kHz 晶振），精度约 ±1%，实验可接受 |
| 改时间后星期不对 | 本工程用 Sakamoto 公式自动计算，无需人工填 |
| LED 不亮 | 确认 250 ms 定时器在跑（TIM3 CEN=1）；LED 为高电平点亮 |

---

## 九、与后续综合实验的衔接

综合实验（LIS3DSH 加速度计）可直接在本工程上增加 SPI1（PA5/PA6/PA7，CS=PE3，
INT1=PE0、INT2=PE1）：EXTI 中断框架（本工程的 EXTI0）正好可用于传感器 INT1/INT2 的事件中断，
串口上传通道已就绪。

---

## 十、许可

本项目采用 **MIT License**，详见 [LICENSE](LICENSE)。

- 可自由使用、修改、分发（含商业用途），只需保留版权与许可声明；软件按“原样”提供，不附带担保。
- `Drivers/` 下的 CMSIS 与 STM32F4xx HAL 驱动由 ARM / STMicroelectronics 提供，
  遵循其原始许可，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
