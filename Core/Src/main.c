/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 实验二 主程序
  *                   STM32F407G-DISC1 / STM32F407VGT6
  *
  *  任务：
  *   1) 按键 B1(PA0) 外部中断 EXTI0：每次按下切换 LED3~LED6 跑马灯方向
  *      （正转 LED3->LED6  /  反转 LED6->LED3），并在中断里生成打印信息
  *   2) TIM3 定时中断（250 ms，要求 200~500 ms 区间）驱动跑马灯逐一点亮
  *   3) USART2 中断接收数据帧 $0,1,YYMMDD,HHMMSS*，解析后更新 RTC 时间
  *   4) RTC 1 Hz 秒中断把当前时间格式化后每秒上传，用于核对时间是否改对
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "rtc.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define LED_STEP_MS         250U   /* 跑马灯步进间隔，要求 200~500 ms */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* ---- RTC 秒中断与主循环共享的时间数据 ---- */
volatile uint32_t m_nRtcYMD    = 0U;
volatile uint32_t m_nRtcHMS    = 0U;
volatile uint8_t  g_rtcSecFlag = 0U;
char              g_rtcString[32] = "RTC init\r\n";

/* ---- 跑马灯 ---- */
/* 四颗 LED 在板上呈菱形排列（不是一排）：
 *             LD3 (PD13)
 *   LD4 (PD12)            LD5 (PD14)
 *             LD6 (PD15)
 * 按名字 LED3→LED4→LED5→LED6 点亮 = 上→左→右→下，视觉上是 "Z" 字形。
 * 所以这里按"物理位置绕圈"排序：LD3(上) → LD5(右) → LD6(下) → LD4(左)
 *   index+1  = 顺时针
 *   index-1  = 逆时针
 */
static const uint16_t s_ledRing[4] = {LD3_Pin, LD5_Pin, LD6_Pin, LD4_Pin};
static volatile uint8_t s_ledIndex   = 0U;   /* 当前点亮序号 0->LD3(上) 1->LD5(右) 2->LD6(下) 3->LD4(左) */
static volatile uint8_t s_dirReverse = 0U;   /* 0 = 顺时针，1 = 逆时针 */

/* ---- 外部中断产生的打印信息（中断里只准备，主循环发送）---- */
static volatile uint8_t s_keyMsgFlag = 0U;
static char s_keyMsg[64];

/* ---- 串口帧接收（DMA + IDLE，收到一段数据后由 App_FrameFeed 逐字节组帧）---- */
char    chRxBuf[128];                        /* 帧内容缓冲（去掉 $ 与 *） */
volatile uint8_t g_frameRdy = 0U;            /* 1 = 收到完整一帧 */
static volatile uint8_t s_rxInFrame = 0U;    /* 正在接收一帧 */
static volatile uint8_t s_rxLen     = 0U;    /* 当前帧长度 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void Led_AllOff(void);
static void Led_Marquee_Step(void);
uint8_t App_FrameFeed(const uint8_t *buf, uint16_t len);
static uint8_t Frame_DecodeSetRtc(const char *buf);
static uint8_t WeekDay_Calc(uint16_t year, uint8_t month, uint8_t day);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if defined(__CC_ARM)
#pragma import(__use_no_semihosting)   /* 不使用半主机模式，printf 才能脱机工作 */
struct __FILE
{
  int handle;
};
FILE __stdout;

void _sys_exit(int x)
{
  (void)x;
}

void _ttywrch(int ch)
{
  (void)ch;
}
#endif

/**
  * @brief  printf 重定向：字符写入串口 TX 环形缓冲，由 DMA 后台发送
  * @note   Uart_TxChar() 只入队，立即返回，printf 不再阻塞
  */
int fputc(int ch, FILE *f)
{
  (void)f;
  return Uart_TxChar(ch);
}
/* USER CODE END 0 */

/**
  * @brief  程序入口：初始化外设后进入主循环
  * @note   初始化顺序：HAL -> 时钟 -> GPIO(含 EXTI) -> RTC -> TIM3 -> USART2 -> DMA
  *         主循环只做三件事：发按键信息、解析串口帧、发 RTC 时间
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_RTC_Init();
  MX_TIM3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */
  /* ★ 必须先初始化 DMA，再使用 printf/串口发送（printf 现在走 DMA 发送路径） */
  Uart_DmaInit();

  printf("\r\n");
  printf("**********************************************\r\n");
  printf("*  STM32F407G-DISC1  Experiment 2            *\r\n");
  printf("*  EXTI0 / TIM3 / USART2 frame / RTC         *\r\n");
  printf("**********************************************\r\n");
  printf("SYSCLK = %lu Hz\r\n", (unsigned long)HAL_RCC_GetSysClockFreq());
  printf("TIM3   : %lu ms update interrupt, LED marquee\r\n", (unsigned long)LED_STEP_MS);
  printf("USART2 : 9600 bps, 8 data bits, 1 stop bit, no parity\r\n");
  printf("CMD    : $0,1,YYMMDD,HHMMSS*   e.g. $0,1,251107,140101*\r\n");
  printf("KEY    : press B1 -> switch direction (CLOCKWISE / COUNTER-CLOCKWISE)\r\n\r\n");

  /* 启动 TIM3 更新中断：此后每 250 ms 自动走一步跑马灯 */
  if (HAL_TIM_Base_Start_IT(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  /* 初始化串口 DMA：TX 走 DMA1_Stream6，RX 走 DMA1_Stream5
     （已在 USER CODE 2 开头调用，这里不再重复） */

  /* 使能串口接收：DMA + 空闲(IDLE)中断，一段数据收完再处理 */
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart2, g_uartRxDma, sizeof(g_uartRxDma)) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* 1) 外部中断里准备好的按键信息，入队后由 DMA 发送 */
    if (s_keyMsgFlag != 0U)
    {
      s_keyMsgFlag = 0U;
      Uart_SendAsync((uint8_t *)s_keyMsg, (uint16_t)strlen(s_keyMsg));
    }

    /* 2) 收到完整数据帧：回显并解析，必要时更新 RTC */
    if (g_frameRdy != 0U)
    {
      g_frameRdy = 0U;
      printf("[RX] $%s*\r\n", chRxBuf);
      if (Frame_DecodeSetRtc(chRxBuf) == 0U)
      {
        printf("[RX] ignored, format: $0,1,YYMMDD,HHMMSS*\r\n");
      }
    }

    /* 3) RTC 秒信号：时间已在中断里格式化，这里入队由 DMA 发送 */
    if (g_rtcSecFlag != 0U)
    {
      g_rtcSecFlag = 0U;
      Uart_SendAsync((uint8_t *)g_rtcString, (uint16_t)strlen(g_rtcString));
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief  系统时钟配置：HSE 8 MHz 经 PLL 倍频到 168 MHz
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/**
  * @brief  熄灭全部用户 LED
  */
static void Led_AllOff(void)
{
  HAL_GPIO_WritePin(GPIOD, LD3_Pin | LD4_Pin | LD5_Pin | LD6_Pin, GPIO_PIN_RESET);
}

/**
  * @brief  跑马灯走一步（方向由 s_dirReverse 决定）
  * @note   按板上菱形位置绕圈，形成顺时针 / 逆时针：
  *         顺时针：LD3(上) -> LD5(右) -> LD6(下) -> LD4(左)
  *         逆时针：LD3(上) -> LD4(左) -> LD6(下) -> LD5(右)
  */
static void Led_Marquee_Step(void)
{
  Led_AllOff();
  HAL_GPIO_WritePin(GPIOD, s_ledRing[s_ledIndex], GPIO_PIN_SET);

  if (s_dirReverse == 0U)
  {
    s_ledIndex++;                     /* 顺时针：序号递增 */
    if (s_ledIndex >= 4U)
    {
      s_ledIndex = 0U;
    }
  }
  else
  {
    s_ledIndex = (s_ledIndex == 0U) ? 3U : (uint8_t)(s_ledIndex - 1U);  /* 逆时针：序号递减 */
  }
}

/**
  * @brief  把一段串口数据逐字节喂给帧状态机（DMA+IDLE 收到后整段调用）
  * @param  buf 数据首地址
  * @param  len 字节数
  * @retval 1 = 这段数据中出现完整一帧（内容已在 chRxBuf）
  * @note   帧格式：$0,1,YYMMDD,HHMMSS*     以 '$' 开头、'*' 结尾
  */
uint8_t App_FrameFeed(const uint8_t *buf, uint16_t len)
{
  uint16_t i;
  uint8_t  done = 0U;

  for (i = 0U; i < len; i++)
  {
    uint8_t rx = buf[i];

    if (rx == '$')                    /* 帧头：重新开始 */
    {
      s_rxInFrame = 1U;
      s_rxLen     = 0U;
      continue;
    }

    if (s_rxInFrame == 0U)            /* 不在帧内，忽略该字节 */
    {
      continue;
    }

    if (rx == '*')                    /* 帧尾：一帧结束 */
    {
      s_rxInFrame = 0U;
      chRxBuf[s_rxLen] = '\0';
      done = 1U;
      continue;
    }

    if (s_rxLen < (sizeof(chRxBuf) - 1U))
    {
      chRxBuf[s_rxLen++] = (char)rx;
    }
    else
    {
      /* 超长帧：丢弃，防止缓冲区溢出 */
      s_rxInFrame = 0U;
      s_rxLen     = 0U;
    }
  }
  return done;
}

/**
  * @brief  Sakamoto 公式求星期
  * @retval RTC 星期常量（1=周一 ... 7=周日）
  */
static uint8_t WeekDay_Calc(uint16_t year, uint8_t month, uint8_t day)
{
  static const uint8_t t[] = {0U, 3U, 2U, 5U, 0U, 3U, 5U, 1U, 4U, 6U, 2U, 4U};
  uint16_t y = year;
  uint8_t  w;

  if (month < 3U)
  {
    y--;
  }
  w = (uint8_t)((y + y / 4U - y / 100U + y / 400U + t[month - 1U] + day) % 7U);
  return (w == 0U) ? RTC_WEEKDAY_SUNDAY : w;   /* 0=周日 -> 7 */
}

/**
  * @brief  解析 "0,1,YYMMDD,HHMMSS" 并更新 RTC
  * @param  buf 去掉 $ 和 * 之后的帧内容
  * @retval 1 = 解析并设置成功，0 = 格式或数值非法
  */
static uint8_t Frame_DecodeSetRtc(const char *buf)
{
  int nAddr = 0, nCmd = 0, nYMD = 0, nHMS = 0;
  int nY, nM, nD, nH, nMin, nS;
  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* 地址, 命令, 年月日, 时分秒 —— 用 %*[ ,] 跳过逗号/空格分隔符 */
  if (sscanf(buf, "%d%*[ ,]%d%*[ ,]%d%*[ ,]%d", &nAddr, &nCmd, &nYMD, &nHMS) < 4)
  {
    return 0U;
  }
  if (nCmd != 1)                      /* 命令 1 = 设置 RTC 时间 */
  {
    return 0U;
  }
  (void)nAddr;

  /* YYMMDD / HHMMSS 拆分：整数除 + 求余 */
  nY   = nYMD / 10000;
  nM   = nYMD % 10000 / 100;
  nD   = nYMD % 100;
  nH   = nHMS / 10000;
  nMin = nHMS % 10000 / 100;
  nS   = nHMS % 100;

  /* 取值范围检查，非法帧直接丢弃（防止把 RTC 写成无效值） */
  if ((nY < 0) || (nY > 99) || (nM < 1) || (nM > 12) || (nD < 1) || (nD > 31) ||
      (nH < 0) || (nH > 23) || (nMin < 0) || (nMin > 59) || (nS < 0) || (nS > 59))
  {
    return 0U;
  }

  sTime.Hours          = (uint8_t)nH;
  sTime.Minutes        = (uint8_t)nMin;
  sTime.Seconds        = (uint8_t)nS;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
  {
    return 0U;
  }

  sDate.WeekDay = WeekDay_Calc((uint16_t)(2000 + nY), (uint8_t)nM, (uint8_t)nD);
  sDate.Month   = (uint8_t)nM;
  sDate.Date    = (uint8_t)nD;
  sDate.Year    = (uint8_t)nY;
  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
  {
    return 0U;
  }

  printf("[RTC] updated -> 20%02d-%02d-%02d %02d:%02d:%02d\r\n",
         nY, nM, nD, nH, nMin, nS);
  return 1U;
}

/**
  * @brief  GPIO 外部中断回调（EXTI0 -> PA0/B1）
  * @note   中断里只更新方向 + 生成字符串，发送交给主循环，
  *         避免在中断中阻塞 30 ms 影响定时器与串口接收
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == B1_Pin)
  {
    s_dirReverse = (s_dirReverse == 0U) ? 1U : 0U;
    (void)snprintf(s_keyMsg, sizeof(s_keyMsg),
                   "[EXTI0] B1 pressed -> %s\r\n",
                   (s_dirReverse != 0U) ? "COUNTER-CLOCKWISE (LD3->LD4->LD6->LD5)"
                                        : "CLOCKWISE (LD3->LD5->LD6->LD4)");
    s_keyMsgFlag = 1U;
  }
}

/**
  * @brief  定时器更新中断回调（TIM3，每 250 ms）
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM3)
  {
    Led_Marquee_Step();
  }
}
/* 注：串口 TX 完成、RX 空闲事件、错误回调都在 usart.c 里实现
        （TX 走 DMA 环形缓冲，RX 走 DMA + IDLE） */
/* USER CODE END 4 */

/**
  * @brief  错误兜底：初始化失败时关中断并停在这里，方便调试器定位
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  参数断言失败时的回调（仅 USE_FULL_ASSERT 打开时启用）
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
