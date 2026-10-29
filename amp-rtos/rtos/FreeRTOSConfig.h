/*
 * FreeRTOSConfig.h —— FreeRTOS 跑在 Linux 上（官方 POSIX 端口）的配置
 *
 * 这个端口是 FreeRTOS 官方维护的，用途就是"没有板子时先跑通逻辑"：
 * 每个任务是一个 pthread，tick 由 setitimer 产生，任务切换靠信号。
 *
 * 所以这里配的东西和真板卡上一样：1 kHz tick、8 个优先级、抢占式调度。
 * 不同的只是"硬件"由 Linux 提供。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configUSE_PREEMPTION			1
#define configUSE_TIME_SLICING			1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION	0
#define configUSE_TICKLESS_IDLE			0

#define configTICK_RATE_HZ			((TickType_t)1000)
#define configMAX_PRIORITIES			8
#define configMINIMAL_STACK_SIZE		((unsigned short)1024)
#define configMAX_TASK_NAME_LEN			16
#define configUSE_16_BIT_TICKS			0
#define configIDLE_SHOULD_YIELD			0

#define configSUPPORT_DYNAMIC_ALLOCATION	1
#define configSUPPORT_STATIC_ALLOCATION		0
#define configTOTAL_HEAP_SIZE			((size_t)(8 * 1024 * 1024))

#define configUSE_MUTEXES			1
#define configUSE_RECURSIVE_MUTEXES		1
#define configUSE_COUNTING_SEMAPHORES		1
#define configUSE_TASK_NOTIFICATIONS		1
#define configQUEUE_REGISTRY_SIZE		8

#define configUSE_TIMERS			1
#define configTIMER_TASK_PRIORITY		(configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH		10
#define configTIMER_TASK_STACK_DEPTH		(configMINIMAL_STACK_SIZE * 2)

/* POSIX 端口用 errno 传递错误码，必须开 */
#define configUSE_POSIX_ERRNO			1

#define configCHECK_FOR_STACK_OVERFLOW		0
#define configUSE_IDLE_HOOK			0
#define configUSE_TICK_HOOK			0
#define configUSE_MALLOC_FAILED_HOOK		0

#define INCLUDE_vTaskDelay			1
#define INCLUDE_vTaskDelayUntil			1
#define INCLUDE_vTaskDelete			1
#define INCLUDE_vTaskSuspend			1
#define INCLUDE_xTaskGetSchedulerState		1
#define INCLUDE_xTaskGetCurrentTaskHandle	1
#define INCLUDE_uxTaskPriorityGet		1
#define INCLUDE_uxTaskGetStackHighWaterMark	1

#endif /* FREERTOS_CONFIG_H */
