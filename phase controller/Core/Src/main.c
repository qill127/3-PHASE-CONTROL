/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Digital Phase Controller (TCA785-like)
  ******************************************************************************
  * HARDWARE (STM32 Nucleo F446RE):
  *   PC0  = Zero Cross input  (H11AA1 optocoupler, rising edge EXTI)
  *   PB0  = TRIAC gate output (active HIGH pulse)
  *   PA5  = LD2 LED — toggles on each zero crossing
  *   PA2  = USART2 TX (115200 baud)
  *   PA3  = USART2 RX (115200 baud)
  *
  * HOW IT WORKS:
  *   1. EXTI0 fires on PC0 rising edge (zero crossing detected)
  *   2. ISR resets TIM3, loads firing_delay into CCR1, starts TIM3
  *   3. TIM3 CC1 interrupt fires after firing_delay microseconds
  *   4. CC1 ISR drives PB0 HIGH for gate_pulse_width, then LOW
  *   5. User sends "P=xx" via UART to set power level (0-100%)
  *
  * FORMULA:
  *   firing_delay_us = (100 - power_level) * 100
  *   e.g. P=50 → delay=5000us, P=100 → delay=0us, P=0 → delay=10000us
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define HALF_CYCLE_US      10000    /* 10ms half-cycle for 50Hz AC             */
#define GATE_PULSE_US      100      /* 100us gate pulse width                  */
#define ZCD_OFFSET_US      0        /* Optocoupler alignment offset (tune me!) */
#define DEFAULT_POWER      0        /* Default power level at startup (OFF)    */
#define UART_BUF_SIZE      32       /* UART receive buffer size                */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
#ifndef TRIAC_GATE_1_Pin
#define TRIAC_GATE_1_Pin GPIO_PIN_0
#define TRIAC_GATE_1_GPIO_Port GPIOB
#define TRIAC_GATE_2_Pin GPIO_PIN_1
#define TRIAC_GATE_2_GPIO_Port GPIOC
#endif

/* ==================== PHASE CONTROL STATE ==================== */
volatile uint32_t power_level    = DEFAULT_POWER;    /* 0-100%                */
volatile uint32_t firing_delay   = HALF_CYCLE_US;    /* Delay in microseconds */

/* ==================== UART RECEIVE STATE ==================== */
uint8_t  uart_rx_byte;                               /* Single byte RX buffer */
char     uart_rx_buf[UART_BUF_SIZE];                 /* Command accumulator   */
uint8_t  uart_rx_idx = 0;                            /* Current index         */
volatile uint8_t new_command = 0;                    /* Flag: command ready   */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */
static void MX_TIM3_Init(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ======================== HELPER ======================== */
void UART_Print(const char *str)
{
    HAL_UART_Transmit(&huart2, (uint8_t*)str, strlen(str), 100);
}

/* ======================== CALLBACKS ======================== */

/* ==================== ZCD STATE ==================== */
volatile uint32_t last_capture = 0;
volatile uint32_t measured_half_cycle = HALF_CYCLE_US;
volatile uint8_t  is_positive_half = 0;

/**
  * @brief  Zero Crossing ISR — fires on every rising edge on PC0
  *         Resets TIM3, loads firing delay, starts the one-shot timer.
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
    {
        /* Measure the actual AC frequency period */
        uint32_t now = __HAL_TIM_GET_COUNTER(&htim2);
        uint32_t diff = now - last_capture;

        /* Anti-bounce: Ignore bounces/noise (anything faster than 1ms / 1000Hz) */
        if (diff < 1000) {
            return; /* Absolutely ignore this noise spike! */
        }
        
        last_capture = now;
        
        /* If the reading is within reasonable 50/60Hz bounds (e.g. 5ms to 12ms) */
        if (diff >= 5000 && diff <= 12000) {
            /* Apply simple smoothing filter: 80% old, 20% new */
            uint32_t raw_half_cycle = ((measured_half_cycle * 4) + diff) / 5;

            /* Convert half-cycle period directly to frequency (Hz) */
            float freq = 1000000.0f / (float)(raw_half_cycle * 2);

            /* Offset/Snap-to-Grid Logic: 
               Lock strictly to 50Hz or 60Hz if we're close (within 1 Hz either way) */
            if (freq >= 49.0f && freq <= 51.0f) {
                measured_half_cycle = 10000;   /* Exactly 50Hz (10,000us half-cycle) */
            } 
            else if (freq >= 59.0f && freq <= 61.0f) {
                measured_half_cycle = 8333;    /* Exactly 60Hz (8,333us half-cycle)  */
            } 
            else {
                /* If it's a completely weird frequency, just use the raw rounded value */
                measured_half_cycle = raw_half_cycle;
            }
        }

        /* Toggle LED to show zero crossing is being detected */
        HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);

        /* Flip our half-cycle state. Starts at 0, first pulse makes it 1 (0-180deg) */
        is_positive_half = !is_positive_half;

        /* Pulse on BOTH the positive and negative half-cycles (0-180 and 180-360) */
        // if (!is_positive_half) {
        //     return;
        // }

        /* 0 = 0 (0% power = no firing) */
        if (power_level == 0)
        {
            HAL_GPIO_WritePin(TRIAC_GATE_1_GPIO_Port, TRIAC_GATE_1_Pin, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(TRIAC_GATE_2_GPIO_Port, TRIAC_GATE_2_Pin, GPIO_PIN_RESET);
            return;
        }

        /* Calculate new delay based on the LIVE measured frequency instead of hardcoded 10ms */
        firing_delay = (uint32_t)(100 - power_level) * (measured_half_cycle / 100);

        /* Apply a slight offset compensation. Optocouplers usually trigger slightly 
         * BEFORE or AFTER the true 0V mark. You can tune ZCD_OFFSET_US at the top 
         * of the file (e.g. +250 or -100) to perfectly align your scope. */
        int32_t tuned_delay = (int32_t)firing_delay + ZCD_OFFSET_US;

        /* 100% power minimum delay. We need a tiny delay so the AC voltage rises 
         * enough to latch the TRIAC. If it's too short, the TRIAC won't turn on. */
        if (tuned_delay < 300) {
            tuned_delay = 300;
        }

        /* 0% power maximum delay. If you want it exactly ON the zero cross, 
         * we allow it to go all the way up to measured_half_cycle. */
        if (tuned_delay > (int32_t)measured_half_cycle) {
            tuned_delay = measured_half_cycle;
        }

        /* Normal case: start TIM3 one-shot with firing_delay */
        __HAL_TIM_SET_COUNTER(&htim3, 0);
        __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, (uint32_t)tuned_delay);
        __HAL_TIM_CLEAR_FLAG(&htim3, TIM_FLAG_CC1);
        HAL_TIM_OC_Start_IT(&htim3, TIM_CHANNEL_1);
    }
}
/**
  * @brief  TIM3 Output Compare callback — fires after firing_delay microseconds
  *         Generates TRIAC gate pulse then stops TIM3.
  */
void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3)
    {
        /* Stop the timer immediately */
        HAL_TIM_OC_Stop_IT(&htim3, TIM_CHANNEL_1);

        /* Select which pin to fire based on the half-cycle state */
        uint16_t active_pin = is_positive_half ? TRIAC_GATE_1_Pin : TRIAC_GATE_2_Pin;
        GPIO_TypeDef* active_port = is_positive_half ? TRIAC_GATE_1_GPIO_Port : TRIAC_GATE_2_GPIO_Port;

        /* Generate gate pulse: HIGH for GATE_PULSE_US microseconds */
        HAL_GPIO_WritePin(active_port, active_pin, GPIO_PIN_SET);

        /* Busy-wait for gate pulse width using TIM2 free-running counter */
        uint32_t start = __HAL_TIM_GET_COUNTER(&htim2);
        while ((__HAL_TIM_GET_COUNTER(&htim2) - start) < GATE_PULSE_US) {}

        HAL_GPIO_WritePin(active_port, active_pin, GPIO_PIN_RESET);
    }
}

/**
  * @brief  UART RX complete callback — accumulates characters into buffer,
  *         triggers command parsing when '\r' or '\n' is received.
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        if (uart_rx_byte == '\r' || uart_rx_byte == '\n')
        {
            if (uart_rx_idx > 0)
            {
                uart_rx_buf[uart_rx_idx] = '\0';
                new_command = 1;
                /* Echo newline to PuTTY */
                UART_Print("\r\n");
            }
            uart_rx_idx = 0;
        }
        else
        {
            if (uart_rx_idx < UART_BUF_SIZE - 1)
            {
                uart_rx_buf[uart_rx_idx++] = (char)uart_rx_byte;
                /* Echo the character back to PuTTY so the user sees what they type */
                HAL_UART_Transmit(&huart2, &uart_rx_byte, 1, 10);
            }
        }

        /* Re-arm UART receive for next byte */
        HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);
    }
}

/**
  * @brief  Parse "P=xx" command and update power level / firing delay
  */
void Process_UART_Command(void)
{
    char msg[80];

    /* Ignore leading spaces or weird hidden characters just in case */
    char *cmd = uart_rx_buf;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    /* If the string is totally empty, just ignore it and don't print anything */
    if (strlen(cmd) == 0) return;

    /* Check if the user is asking for the frequency */
    if (strncmp(cmd, "freq", 4) == 0 || strncmp(cmd, "FREQ", 4) == 0)
    {
        uint32_t now = __HAL_TIM_GET_COUNTER(&htim2);
        /* If no pulse for >100ms, assume disconnected (100ms = 100000us) */
        if (last_capture == 0 || (now - last_capture) > 100000)
        {
            HAL_UART_Transmit(&huart2, (uint8_t*)"AC Status: No Connection\r\n", 26, 100);
            return;
        }

        float freq = 0.0f;
        if (measured_half_cycle > 0)
        {
            /* freq = 1 / period. Period = 2 * half_cycle */
            freq = 1000000.0f / (float)(measured_half_cycle * 2);
        }
        int len = sprintf(msg, "AC Frequency: %.2f Hz\r\n", freq);
        HAL_UART_Transmit(&huart2, (uint8_t*)msg, (uint16_t)len, 100);
        return;
    }

    /* `atoi` will gracefully convert any number string like "80" into 80.
       If someone types something that isn't a number like "hello", atoi returns 0. */
    int val = atoi(cmd);

    /* Clamp to 0-100 range */
    if (val < 0) val = 0;
    if (val > 100) val = 100;

    power_level  = (uint32_t)val;
    firing_delay = (uint32_t)(100 - power_level) * (measured_half_cycle / 100);

    int len = sprintf(msg, "OK | Power: %lu%% | Delay: %lu us\r\n",
                      power_level, firing_delay);
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, (uint16_t)len, 100);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
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
  MX_TIM2_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  MX_TIM3_Init(); /* Added manually because CubeMX wiped it */

  /* Start TIM2 free-running counter (1us per tick, used for timing) */
  HAL_TIM_Base_Start(&htim2);

  /* Start UART receive interrupt (single byte at a time) */
  HAL_UART_Receive_IT(&huart2, &uart_rx_byte, 1);

  /* Startup banner */
  UART_Print("\r\n\r\n");
  UART_Print("=========================================\r\n");
  UART_Print("  Digital Phase Controller (TCA785-like)\r\n");
  UART_Print("  STM32 Nucleo F446RE\r\n");
  UART_Print("  PC0 = ZCD input | PB0 = TRIAC gate\r\n");
  UART_Print("=========================================\r\n");
  UART_Print("  Send: P=0..100  (e.g. P=50)\r\n");
  UART_Print("=========================================\r\n\r\n");

  char msg[80];
  int len = sprintf(msg, "Current: Power=%lu%% | Delay=%lu us\r\n",
                    power_level, firing_delay);
  HAL_UART_Transmit(&huart2, (uint8_t*)msg, (uint16_t)len, 100);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

    /* Process UART command if one is ready */
    if (new_command)
    {
        new_command = 0;
        Process_UART_Command();
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
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

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 89;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
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

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : B1_Pin */
  GPIO_InitStruct.Pin = B1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(B1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : PC0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : LD2_Pin */
  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* Enable TIM3 clock and Interrupt manually (CubeMX wiped it from MSP) */
  __HAL_RCC_TIM3_CLK_ENABLE();
  HAL_NVIC_SetPriority(TIM3_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM3_IRQn);

  /* Enable USART2 RX Interrupt manually */
  HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(USART2_IRQn);

  /* Set TRIAC pin manually just in case */
  HAL_GPIO_WritePin(TRIAC_GATE_1_GPIO_Port, TRIAC_GATE_1_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(TRIAC_GATE_2_GPIO_Port, TRIAC_GATE_2_Pin, GPIO_PIN_RESET);
  
  GPIO_InitStruct.Pin = TRIAC_GATE_1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(TRIAC_GATE_1_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = TRIAC_GATE_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(TRIAC_GATE_2_GPIO_Port, &GPIO_InitStruct);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/**
  * @brief TIM3 Initialization Function — Phase delay one-shot timer
  *        PSC=89 → 90MHz/90 = 1MHz = 1us per tick
  *        ARR=10000 → max 10ms (one AC half-cycle at 50Hz)
  *        OC1 used in Timing mode (no pin output, interrupt only)
  */
static void MX_TIM3_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 89;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 10000;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_OC_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_TIMING;
  sConfigOC.Pulse = 5000;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_OC_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
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
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
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
