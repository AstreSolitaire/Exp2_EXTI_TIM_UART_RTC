/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   串口 USART2 配置 + DMA 收发实现
  *
  *  本文件两部分：
  *    1) 外设配置：9600-8-N-1，PA2/PA3 复用为 AF7（HAL_UART_MspInit）
  *    2) DMA 收发：TX 走 DMA1_Stream6 + 512B 环形 FIFO；
  *       RX 走 DMA1_Stream5 + 空闲(IDLE)中断
  *  上层调用 Uart_SendAsync()/printf 只入队立即返回，字节由 DMA 硬件搬运。
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
#include "usart.h"

/* USER CODE BEGIN 0 */
/*
 * 串口 DMA 收发实现
 * -----------------
 * TX：上层（printf / RTC 时间串 / 按键信息）只把数据写进 512 B 环形 FIFO，
 *     立即返回；由 DMA1_Stream6 把 FIFO 里的数据块搬到 USART2->DR。
 *     一次 DMA 传完触发 TxCplt，中断里检查 FIFO 是否还有剩余，有就接着发。
 * RX：DMA1_Stream5 + 串口空闲(IDLE)中断，一帧数据收完（总线空闲）才进一次中断，
 *     把整段缓冲交给帧状态机 App_FrameFeed()。
 *
 * 注意：DMA 直接从内存搬数据，源缓冲区必须是静态/全局的（不能是栈上局部变量）。
 */
#define UART_TX_FIFO_SIZE   512U
#define UART_DMA_CHUNK      128U

static uint8_t  s_txFifo[UART_TX_FIFO_SIZE];
static volatile uint16_t s_txHead = 0U;      /* 写指针 */
static volatile uint16_t s_txTail = 0U;      /* 读指针 */
static volatile uint8_t  s_txBusy = 0U;      /* 1 = DMA 正在搬数据 */
static uint8_t  s_dmaTxBuf[UART_DMA_CHUNK];  /* DMA 搬运块（必须静态） */

uint8_t g_uartRxDma[64];                     /* RX DMA 接收缓冲（必须静态） */
static DMA_HandleTypeDef hdma_usart2_tx;
static DMA_HandleTypeDef hdma_usart2_rx;

/* ---------------- 环形 FIFO ---------------- */
static void Fifo_Put(uint8_t b)
{
  uint16_t next = (uint16_t)((s_txHead + 1U) % UART_TX_FIFO_SIZE);
  if (next == s_txTail)                      /* 满了：丢弃新字节（避免覆盖未发数据） */
  {
    return;
  }
  s_txFifo[s_txHead] = b;
  s_txHead = next;
}

static uint16_t Fifo_Pop(uint8_t *dst, uint16_t maxLen)
{
  uint16_t n = 0U;
  while ((n < maxLen) && (s_txTail != s_txHead))
  {
    dst[n++] = s_txFifo[s_txTail];
    s_txTail = (uint16_t)((s_txTail + 1U) % UART_TX_FIFO_SIZE);
  }
  return n;
}

/* ---------------- TX 调度 ---------------- */
/**
  * @brief  FIFO 非空且 DMA 空闲时，取一块数据启动 DMA 发送
  * @note   可能在主循环或 TxCplt 中断里被调用，故临界区短暂关中断
  */
void Uart_Kick(void)
{
  uint16_t n;

  if (s_txBusy != 0U)
  {
    return;
  }
  /* 只看"发送"状态：不能用 HAL_UART_GetState()，它返回 gState|RxState 的组合值，
     RX 侧一直开着 DMA 会使其永远不等于 READY，导致 TX 永远启动不了 */
  if (huart2.gState != HAL_UART_STATE_READY)
  {
    return;                                  /* 上一笔发送还没彻底结束 */
  }
  if (s_txHead == s_txTail)
  {
    return;                                  /* FIFO 空 */
  }

  __disable_irq();
  if ((s_txBusy != 0U) || (s_txHead == s_txTail))
  {
    __enable_irq();
    return;
  }
  n = Fifo_Pop(s_dmaTxBuf, UART_DMA_CHUNK);
  if (n != 0U)
  {
    s_txBusy = 1U;
  }
  __enable_irq();

  if (n != 0U)
  {
    if (HAL_UART_Transmit_DMA(&huart2, s_dmaTxBuf, n) != HAL_OK)
    {
      s_txBusy = 0U;                         /* 启动失败：下次重试（可能丢这一块） */
    }
  }
}

/**
  * @brief  异步发送一段数据（立即返回，不阻塞）
  */
void Uart_SendAsync(const uint8_t *data, uint16_t len)
{
  uint16_t i;
  for (i = 0U; i < len; i++)
  {
    Fifo_Put(data[i]);
  }
  Uart_Kick();
}

/**
  * @brief  单字节入队（供 fputc / printf 使用）
  */
int Uart_TxChar(int ch)
{
  Fifo_Put((uint8_t)ch);
  Uart_Kick();
  return ch;
}

/* ---------------- DMA 初始化 ---------------- */
void Uart_DmaInit(void)
{
  /* TX：DMA1 Stream6 Channel4（F407 上 USART2_TX 的固定映射） */
  __HAL_RCC_DMA1_CLK_ENABLE();

  hdma_usart2_tx.Instance                 = DMA1_Stream6;
  hdma_usart2_tx.Init.Channel             = DMA_CHANNEL_4;
  hdma_usart2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
  hdma_usart2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart2_tx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart2_tx.Init.Mode                = DMA_NORMAL;
  hdma_usart2_tx.Init.Priority            = DMA_PRIORITY_LOW;
  hdma_usart2_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&hdma_usart2_tx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&huart2, hdmatx, hdma_usart2_tx);

  /* RX：DMA1 Stream5 Channel4 */
  hdma_usart2_rx.Instance                 = DMA1_Stream5;
  hdma_usart2_rx.Init.Channel             = DMA_CHANNEL_4;
  hdma_usart2_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_usart2_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_usart2_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_usart2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_usart2_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_usart2_rx.Init.Mode                = DMA_NORMAL;
  hdma_usart2_rx.Init.Priority            = DMA_PRIORITY_MEDIUM;
  hdma_usart2_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&hdma_usart2_rx) != HAL_OK)
  {
    Error_Handler();
  }
  __HAL_LINKDMA(&huart2, hdmarx, hdma_usart2_rx);

  /* DMA 流的 NVIC（DMA 传输完成/错误中断） */
  HAL_NVIC_SetPriority(DMA1_Stream6_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream6_IRQn);
  HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 3, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
}
/* USER CODE END 0 */

UART_HandleTypeDef huart2;

/**
  * @brief  USART2 通信参数配置
  * @note   波特率 9600、8 数据位、1 停止位、无校验、收发模式；HAL 会据此算出 BRR 分频
  */
void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 9600;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(uartHandle->Instance==USART2)
  {
  /* USER CODE BEGIN USART2_MspInit 0 */

  /* USER CODE END USART2_MspInit 0 */
    /* USART2 clock enable */
    __HAL_RCC_USART2_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART2 GPIO Configuration
    PA2     ------> USART2_TX
    PA3     ------> USART2_RX
    */
    GPIO_InitStruct.Pin = USART2_TX_Pin|USART2_RX_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USART2 interrupt Init */
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
  /* USER CODE BEGIN USART2_MspInit 1 */

  /* USER CODE END USART2_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART2)
  {
  /* USER CODE BEGIN USART2_MspDeInit 0 */

  /* USER CODE END USART2_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART2_CLK_DISABLE();

    /**USART2 GPIO Configuration
    PA2     ------> USART2_TX
    PA3     ------> USART2_RX
    */
    HAL_GPIO_DeInit(GPIOA, USART2_TX_Pin|USART2_RX_Pin);

    /* USART2 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
  /* USER CODE BEGIN USART2_MspDeInit 1 */

  /* USER CODE END USART2_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
/**
  * @brief  DMA 发送完成回调：结束本次搬运，继续发 FIFO 里剩余数据
  */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *uartHandle)
{
  if (uartHandle->Instance == USART2)
  {
    s_txBusy = 0U;
    Uart_Kick();                 /* 链式发送：FIFO 里还有就接着发 */
  }
}

/**
  * @brief  串口空闲(IDLE)事件回调：一段数据收完，交给帧状态机
  * @param  Size 本次收到的字节数
  */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *uartHandle, uint16_t Size)
{
  if (uartHandle->Instance == USART2)
  {
    if (App_FrameFeed(g_uartRxDma, Size) != 0U)
    {
      g_frameRdy = 1U;
    }
    /* 重新挂上 DMA + IDLE 接收，等待下一段数据 */
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart2, g_uartRxDma, sizeof(g_uartRxDma));
  }
}

/**
  * @brief  串口错误回调：清溢出标志并重启 DMA+IDLE 接收
  */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *uartHandle)
{
  if (uartHandle->Instance == USART2)
  {
    __HAL_UART_CLEAR_OREFLAG(uartHandle);
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart2, g_uartRxDma, sizeof(g_uartRxDma));
  }
}

/**
  * @brief  DMA1 Stream6 中断（USART2_TX 传输完成）
  */
void DMA1_Stream6_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart2_tx);
}

/**
  * @brief  DMA1 Stream5 中断（USART2_RX）
  */
void DMA1_Stream5_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_usart2_rx);
}
/* USER CODE END 1 */
