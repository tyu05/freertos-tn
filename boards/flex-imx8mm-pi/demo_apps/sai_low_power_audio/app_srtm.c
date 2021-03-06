/*
 * Copyright (c) 2018, NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include "fsl_gpio.h"
#include "fsl_mu.h"
#include "fsl_sdma.h"
#include "fsl_iomuxc.h"
#include "srtm_dispatcher.h"
#include "srtm_peercore.h"
#include "srtm_message.h"
#include "srtm_audio_service.h"
#include "app_srtm.h"
#include "lpm.h"
#include "srtm_sai_sdma_adapter.h"
#include "srtm_rpmsg_endpoint.h"

#if APP_SRTM_CODEC_USED_I2C
#include "fsl_i2c_freertos.h"
#include "srtm_i2c_codec_adapter.h"
#include "fsl_ak4497.h"
#endif

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define APP_MS2TICK(ms) ((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS)
#define BUFFER_LEN (80 * 1024)
uint8_t g_buffer[BUFFER_LEN];
srtm_sai_sdma_local_buf_t g_local_buf = {
    .buf = (uint8_t *)&g_buffer, .bufSize = BUFFER_LEN, .periods = SRTM_SAI_SDMA_MAX_LOCAL_BUF_PERIODS, .threshold = 1,

};

/*******************************************************************************
* Prototypes
******************************************************************************/
#if APP_SRTM_CODEC_USED_I2C
/* Send data to Codec device on I2C Bus. */
static status_t Codec_I2C_SendFunc(
    uint8_t deviceAddress, uint32_t subAddress, uint8_t subAddressSize, const uint8_t *txBuff, uint8_t txBuffSize);

/* Receive data from Codec device on I2C Bus. */
static status_t Codec_I2C_ReceiveFunc(
    uint8_t deviceAddress, uint32_t subAddress, uint8_t subAddressSize, uint8_t *rxBuff, uint8_t rxBuffSize);
#endif
/*******************************************************************************
 * Variables
 ******************************************************************************/
typedef enum
{
    APP_SRTM_StateRun = 0x0U,
    APP_SRTM_StateLinkedUp,
} app_srtm_state_t;

app_rpmsg_monitor_t rpmsgMonitor;
volatile app_srtm_state_t srtmState = APP_SRTM_StateRun;
#if APP_SRTM_CODEC_USED_I2C
static i2c_rtos_handle_t I2cHandle;
static i2c_rtos_handle_t *codecI2cHandle;
static codec_handle_t codecHandle;
static bool powerOnAudioBoard = false;
#endif
static srtm_dispatcher_t disp;
static srtm_peercore_t core;
static srtm_service_t audioService;

SemaphoreHandle_t monSig;
struct rpmsg_lite_instance *rpmsgHandle;

void *rpmsgMonitorParam;
TimerHandle_t linkupTimer;
#if APP_SRTM_CODEC_USED_I2C
static codec_config_t codecConfig = {.I2C_SendFunc = Codec_I2C_SendFunc,
                                     .I2C_ReceiveFunc = Codec_I2C_ReceiveFunc,
                                     .op.Init = AK4497_Init,
                                     .op.Deinit = AK4497_Deinit,
                                     .op.SetFormat = AK4497_ConfigDataFormat,
                                     .op.SetEncoding = AK4497_SetEncoding};
#endif
srtm_sai_adapter_t saiAdapter;
/*******************************************************************************
 * Code
 ******************************************************************************/
bool APP_SRTM_ServiceIdle(void)
{
    srtm_audio_state_t TxState, RxState;

    SRTM_SaiSdmaAdapter_GetAudioServiceState(saiAdapter, &TxState, &RxState);
    if (TxState == SRTM_AudioStateClosed && RxState == SRTM_AudioStateClosed)
    {
        return true;
    }
    else
    {
        return false;
    }
}
#if APP_SRTM_CODEC_USED_I2C
static void i2c_release_bus_delay(void)
{
    uint32_t i = 0;
    for (i = 0; i < APP_SRTM_I2C_DELAY; i++)
    {
        __NOP();
    }
}
void APP_SRTM_I2C_ReleaseBus(void)
{
    uint8_t i = 0;
    gpio_pin_config_t pin_config = {kGPIO_DigitalOutput, 1, kGPIO_NoIntmode};

    IOMUXC_SetPinMux(IOMUXC_I2C3_SCL_GPIO5_IO18, 0U);
    IOMUXC_SetPinConfig(IOMUXC_I2C3_SCL_GPIO5_IO18, IOMUXC_SW_PAD_CTL_PAD_DSE(6U) | IOMUXC_SW_PAD_CTL_PAD_FSEL(2U) |
                                                        IOMUXC_SW_PAD_CTL_PAD_ODE_MASK |
                                                        IOMUXC_SW_PAD_CTL_PAD_HYS_MASK);
    IOMUXC_SetPinMux(IOMUXC_I2C3_SDA_GPIO5_IO19, 0U);
    IOMUXC_SetPinConfig(IOMUXC_I2C3_SDA_GPIO5_IO19, IOMUXC_SW_PAD_CTL_PAD_DSE(6U) | IOMUXC_SW_PAD_CTL_PAD_FSEL(2U) |
                                                        IOMUXC_SW_PAD_CTL_PAD_ODE_MASK |
                                                        IOMUXC_SW_PAD_CTL_PAD_HYS_MASK);
    GPIO_PinInit(APP_AUDIO_I2C_SCL_GPIO, APP_AUDIO_I2C_SCL_PIN, &pin_config);
    GPIO_PinInit(APP_AUDIO_I2C_SDA_GPIO, APP_AUDIO_I2C_SDA_PIN, &pin_config);

    /* Drive SDA low first to simulate a start */
    GPIO_PinWrite(APP_AUDIO_I2C_SDA_GPIO, APP_AUDIO_I2C_SDA_PIN, 0U);
    i2c_release_bus_delay();

    /* Send 9 pulses on SCL and keep SDA high */
    for (i = 0; i < 9; i++)
    {
        GPIO_PinWrite(APP_AUDIO_I2C_SCL_GPIO, APP_AUDIO_I2C_SCL_PIN, 0U);
        i2c_release_bus_delay();

        GPIO_PinWrite(APP_AUDIO_I2C_SDA_GPIO, APP_AUDIO_I2C_SDA_PIN, 1U);
        i2c_release_bus_delay();

        GPIO_PinWrite(APP_AUDIO_I2C_SCL_GPIO, APP_AUDIO_I2C_SCL_PIN, 1U);
        i2c_release_bus_delay();
        i2c_release_bus_delay();
    }

    /* Send stop */
    GPIO_PinWrite(APP_AUDIO_I2C_SCL_GPIO, APP_AUDIO_I2C_SCL_PIN, 0U);
    i2c_release_bus_delay();

    GPIO_PinWrite(APP_AUDIO_I2C_SDA_GPIO, APP_AUDIO_I2C_SDA_PIN, 0U);
    i2c_release_bus_delay();

    GPIO_PinWrite(APP_AUDIO_I2C_SCL_GPIO, APP_AUDIO_I2C_SCL_PIN, 1U);
    i2c_release_bus_delay();

    GPIO_PinWrite(APP_AUDIO_I2C_SDA_GPIO, APP_AUDIO_I2C_SDA_PIN, 1U);
    i2c_release_bus_delay();
}
/*
 * Audio board must be powered after the i.MX8MM FLEX-IMX8MM is powered.
 * To achieve this, the power gate of the audio board power is controled by the
 * PORT0_1 pin of the PCA6416APW device on the FLEX-IMX8MM board.
 * Therefore, the "APP_SRTM_PowerOnAudioBoard()" would be specially added in this case.
 */
static void APP_SRTM_PowerOnAudioBoard(void)
{
    uint8_t temp = 0, reVal;
    i2c_master_transfer_t masterXfer;

    /* Prepare transfer structure. */
    masterXfer.slaveAddress = 0x20; /* The PCA6416APW IC address */
    masterXfer.direction = kI2C_Write;
    masterXfer.subaddress = 0x6U; /* Configure the PORT0 of the PCA6416APW */
    masterXfer.subaddressSize = 1U;
    temp = 0xFD; /* Set P0_1 to output direction */
    masterXfer.data = &temp;
    masterXfer.dataSize = 1U;
    masterXfer.flags = kI2C_TransferDefaultFlag;

    reVal = I2C_MasterTransferBlocking(I2C3, &masterXfer);

    if (reVal == kStatus_Success)
    {
        i2c_release_bus_delay(); /* Ensure the I2C bus free. */
        temp = 0xFF;
        masterXfer.subaddress = 0x2U;      /* Select the outputregister for port 0 */
        masterXfer.direction = kI2C_Write; /* Make P0_1 output high level */
        masterXfer.subaddressSize = 1U;
        masterXfer.data = &temp;
        I2C_MasterTransferBlocking(I2C3, &masterXfer);
    }
}
static void APP_SRTM_InitI2C(i2c_rtos_handle_t *handle, I2C_Type *base, uint32_t baudrate, uint32_t clockrate)
{
    status_t status;
    i2c_master_config_t masterConfig;

    /*
     * masterConfig->baudRate_Bps = 100000U;
     * masterConfig->enableHighDrive = false;
     * masterConfig->enableStopHold = false;
     * masterConfig->glitchFilterWidth = 0U;
     * masterConfig->enableMaster = true;
     */
    I2C_MasterGetDefaultConfig(&masterConfig);
    masterConfig.baudRate_Bps = baudrate;
    /*  Set I2C Master IRQ Priority. */
    NVIC_SetPriority(I2C3_IRQn, APP_SRTM_I2C3_IRQ_PRIO);
    /* Initialize I2C RTOS driver. */
    status = I2C_RTOS_Init(handle, base, &masterConfig, clockrate);
    assert(status == kStatus_Success);
    (void)status;
}

static void APP_SRTM_DeinitI2C(i2c_rtos_handle_t *handle)
{
    I2C_RTOS_Deinit(handle);
}

static status_t I2C_SendFunc(i2c_rtos_handle_t *handle,
                             uint8_t deviceAddress,
                             uint32_t subAddress,
                             uint8_t subAddressSize,
                             const uint8_t *txBuff,
                             uint8_t txBuffSize)
{
    status_t status = 0U;
    i2c_master_transfer_t masterXfer;

    /* Prepare transfer structure. */
    masterXfer.slaveAddress = deviceAddress;
    masterXfer.direction = kI2C_Write;
    masterXfer.subaddress = subAddress;
    masterXfer.subaddressSize = subAddressSize;
    masterXfer.data = (void *)txBuff;
    masterXfer.dataSize = txBuffSize;
    masterXfer.flags = kI2C_TransferDefaultFlag;

    LPM_IncreseBlockSleepCnt();
    /* Calling I2C Transfer API to start send. */
    status = I2C_RTOS_Transfer(handle, &masterXfer);
    LPM_DecreaseBlockSleepCnt();

    return status;
}

static status_t I2C_ReceiveFunc(i2c_rtos_handle_t *handle,
                                uint8_t deviceAddress,
                                uint32_t subAddress,
                                uint8_t subAddressSize,
                                uint8_t *rxBuff,
                                uint8_t rxBuffSize)
{
    status_t status = 0U;
    i2c_master_transfer_t masterXfer;

    /* Prepare transfer structure. */
    masterXfer.slaveAddress = deviceAddress;
    masterXfer.direction = kI2C_Read;
    masterXfer.subaddress = subAddress;
    masterXfer.subaddressSize = subAddressSize;
    masterXfer.data = rxBuff;
    masterXfer.dataSize = rxBuffSize;
    masterXfer.flags = kI2C_TransferDefaultFlag;

    LPM_IncreseBlockSleepCnt();
    /* Calling I2C Transfer API to start send. */
    status = I2C_RTOS_Transfer(handle, &masterXfer);
    LPM_DecreaseBlockSleepCnt();

    return status;
}
static status_t Codec_I2C_SendFunc(
    uint8_t deviceAddress, uint32_t subAddress, uint8_t subAddressSize, const uint8_t *txBuff, uint8_t txBuffSize)
{
    /* Calling I2C Transfer API to start send. */
    return I2C_SendFunc(codecI2cHandle, deviceAddress, subAddress, subAddressSize, txBuff, txBuffSize);
}

static status_t Codec_I2C_ReceiveFunc(
    uint8_t deviceAddress, uint32_t subAddress, uint8_t subAddressSize, uint8_t *rxBuff, uint8_t rxBuffSize)
{
    /* Calling I2C Transfer API to start receive. */
    return I2C_ReceiveFunc(codecI2cHandle, deviceAddress, subAddress, subAddressSize, rxBuff, rxBuffSize);
}

static status_t APP_SRTM_ReadCodecRegMap(void *handle, uint32_t reg, uint32_t *val)
{
    return AK4497_ReadReg((codec_handle_t *)handle, reg, (uint8_t *)val);
}

static status_t APP_SRTM_WriteCodecRegMap(void *handle, uint32_t reg, uint32_t val)
{
    return AK4497_WriteReg((codec_handle_t *)handle, reg, val);
}
#endif
static void APP_SRTM_PollLinkup(srtm_dispatcher_t dispatcher, void *param1, void *param2)
{
    if (srtmState == APP_SRTM_StateRun)
    {
        if (rpmsg_lite_is_link_up(rpmsgHandle))
        {
            srtmState = APP_SRTM_StateLinkedUp;
            xSemaphoreGive(monSig);
        }
        else
        {
            /* Start timer to poll linkup status. */
            xTimerStart(linkupTimer, portMAX_DELAY);
        }
    }
}

static void APP_LinkupTimerCallback(TimerHandle_t xTimer)
{
    srtm_procedure_t proc = SRTM_Procedure_Create(APP_SRTM_PollLinkup, NULL, NULL);

    if (proc)
    {
        SRTM_Dispatcher_PostProc(disp, proc);
    }
}

static void APP_SRTM_NotifyPeerCoreReady(struct rpmsg_lite_instance *rpmsgHandle, bool ready)
{
    if (rpmsgMonitor)
    {
        rpmsgMonitor(rpmsgHandle, ready, rpmsgMonitorParam);
    }
}

static void APP_SRTM_Linkup(void)
{
    srtm_channel_t chan;
    srtm_rpmsg_endpoint_config_t rpmsgConfig;

    APP_SRTM_NotifyPeerCoreReady(rpmsgHandle, true);

    /* Create SRTM peer core */
    core = SRTM_PeerCore_Create(1U); /* Assign CA53 core ID to 1U */

    SRTM_PeerCore_SetState(core, SRTM_PeerCore_State_Activated);

    /* Common RPMsg channel config */
    rpmsgConfig.localAddr = RL_ADDR_ANY;
    rpmsgConfig.peerAddr = RL_ADDR_ANY;

    rpmsgConfig.rpmsgHandle = rpmsgHandle;
    rpmsgConfig.epName = APP_SRTM_AUDIO_CHANNEL_NAME;
    chan = SRTM_RPMsgEndpoint_Create(&rpmsgConfig);
    SRTM_PeerCore_AddChannel(core, chan);

    SRTM_Dispatcher_AddPeerCore(disp, core);
}

static void APP_SRTM_InitPeerCore(void)
{
    rpmsgHandle = rpmsg_lite_remote_init((void *)RPMSG_LITE_SRTM_SHMEM_BASE, RPMSG_LITE_SRTM_LINK_ID, RL_NO_FLAGS);
    assert(rpmsgHandle);
    if (rpmsg_lite_is_link_up(rpmsgHandle))
    {
        /* If resume context has already linked up, don't need to announce channel again. */
        APP_SRTM_Linkup();
    }
    else
    {
        /* Start timer to poll linkup status. */
        xTimerStart(linkupTimer, portMAX_DELAY);
    }
}

static void APP_SRTM_InitAudioDevice(void)
{
    sdma_config_t dmaConfig = {0};

    /* Create SDMA handle */
    SDMA_GetDefaultConfig(&dmaConfig);
    dmaConfig.ratio = kSDMA_ARMClockFreq; /* SDMA2 & SDMA3 must set the clock ratio to 1:1. */
    SDMA_Init(SDMAARM3, &dmaConfig);

#if APP_SRTM_CODEC_USED_I2C

    APP_SRTM_InitI2C(&I2cHandle, I2C3, APP_SRTM_I2C_BAUDRATE, APP_SRTM_I2C_CLOCK_FREQ);
    if (!powerOnAudioBoard)
    {
        APP_SRTM_PowerOnAudioBoard();
        powerOnAudioBoard = true;
    }
    codecI2cHandle = &I2cHandle;
#endif
}

static void APP_SRTM_DeinitAudioDevice(void)
{
    APP_SRTM_DeinitI2C(&I2cHandle);
    SDMA_Deinit(SDMAARM3);
}
static void APP_SRTM_InitAudioService(void)
{
    srtm_sai_sdma_config_t saiTxConfig;
    srtm_sai_sdma_config_t saiRxConfig;
#if APP_SRTM_CODEC_USED_I2C
    srtm_i2c_codec_config_t i2cCodecConfig;

    ak4497_config_t ak4497Config;
#endif
    srtm_codec_adapter_t codecAdapter;
    APP_SRTM_InitAudioDevice();

    /* Create SAI SDMA adapter */
    /* Init SAI module */
    /*
     * config.masterSlave = kSAI_Master;
     * config.mclkSource = kSAI_MclkSourceSysclk;
     * config.protocol = kSAI_BusLeftJustified;
     * config.syncMode = kSAI_ModeAsync;
     * config.mclkOutputEnable = true;
     */
    SAI_TxGetDefaultConfig(&saiTxConfig.config);
    saiTxConfig.config.protocol = kSAI_BusI2S; /* Tx in async mode */
    saiTxConfig.dataLine1 = 0;
    saiTxConfig.dataLine2 = 4; /* Channel 4 is used as the another channel for the DSD music stream. */
    saiTxConfig.watermark = FSL_FEATURE_SAI_FIFO_COUNT - 1;
    saiTxConfig.mclk = APP_SAI_CLK_FREQ;
    saiTxConfig.bclk = saiTxConfig.mclk;
    saiTxConfig.stopOnSuspend = false; /* Audio data is in DRAM which is not accessable in A53 suspend. */
    saiTxConfig.guardTime =
        1000; /* Unit:ms. This is a lower limit that M4 should reserve such time data to wakeup A core. */
    saiTxConfig.threshold = 1; /* Under the threshold value would trigger periodDone message to A53. */
    saiTxConfig.dmaChannel = APP_SAI_TX_DMA_CHANNEL;
    saiTxConfig.ChannelPriority = APP_SAI_TX_DMA_CHANNEL_PRIORITY;
    saiTxConfig.eventSource = APP_SAI_TX_DMA_SOURCE;

    SAI_RxGetDefaultConfig(&saiRxConfig.config);
    saiRxConfig.config.syncMode = kSAI_ModeSync; /* Rx in sync mode */
    saiRxConfig.config.protocol = kSAI_BusI2S;
    saiRxConfig.dataLine1 = 0;
    saiRxConfig.watermark = 1;
    saiRxConfig.mclk = APP_SAI_CLK_FREQ;
    saiRxConfig.bclk = saiTxConfig.mclk;
    saiRxConfig.threshold = UINT32_MAX; /* Under the threshold value would trigger periodDone message to A53. */
    saiRxConfig.dmaChannel = APP_SAI_RX_DMA_CHANNEL;
    saiRxConfig.ChannelPriority = APP_SAI_RX_DMA_CHANNEL_PRIORITY;
    saiRxConfig.eventSource = APP_SAI_RX_DMA_SOURCE;

    saiAdapter = SRTM_SaiSdmaAdapter_Create(APP_SRTM_SAI, APP_SRTM_DMA, &saiTxConfig, &saiRxConfig);
    assert(saiAdapter);
#if APP_SRTM_CODEC_USED_I2C
    AK4497_DefaultConfig(&ak4497Config);
    codecConfig.codecConfig = &ak4497Config;

    CODEC_Init(&codecHandle, &codecConfig);
    /* Create I2C Codec adaptor */
    i2cCodecConfig.addrType = kCODEC_RegAddr8Bit;
    i2cCodecConfig.regWidth = kCODEC_RegWidth8Bit;
    i2cCodecConfig.writeRegMap = APP_SRTM_WriteCodecRegMap;
    i2cCodecConfig.readRegMap = APP_SRTM_ReadCodecRegMap;
    codecAdapter = SRTM_I2CCodecAdapter_Create(&codecHandle, &i2cCodecConfig);
    assert(codecAdapter);
#endif

    /*  Set SAI DMA IRQ Priority. */
    NVIC_SetPriority(APP_SRTM_DMA_IRQn, APP_SAI_TX_DMA_IRQ_PRIO);
    NVIC_SetPriority(APP_SRTM_SAI_IRQn, APP_SAI_IRQ_PRIO);

    /* Create and register audio service */
    SRTM_SaiSdmaAdapter_SetTxLocalBuf(saiAdapter, &g_local_buf);
    audioService = SRTM_AudioService_Create(saiAdapter, codecAdapter);
    SRTM_Dispatcher_RegisterService(disp, audioService);
}

static void APP_SRTM_InitServices(void)
{
    APP_SRTM_InitAudioService();
}

static void SRTM_MonitorTask(void *pvParameters)
{
    /* Initialize services and add to dispatcher */
    APP_SRTM_InitServices();

    /* Start SRTM dispatcher */
    SRTM_Dispatcher_Start(disp);

    xSemaphoreGive(monSig);
    while (true)
    {
        xSemaphoreTake(monSig, portMAX_DELAY);
        if (srtmState == APP_SRTM_StateRun)
        {
            SRTM_Dispatcher_Stop(disp);
            APP_SRTM_InitPeerCore();
            SRTM_Dispatcher_Start(disp);
        }
        else
        {
            SRTM_Dispatcher_Stop(disp);
            /* Need to announce channel as we just linked up. */
            APP_SRTM_Linkup();
            SRTM_Dispatcher_Start(disp);
        }
    }
}

static void SRTM_DispatcherTask(void *pvParameters)
{
    SRTM_Dispatcher_Run(disp);
}

void APP_SRTM_Init(void)
{
    MU_Init(MUB);

    monSig = xSemaphoreCreateBinary();
    assert(monSig);
    linkupTimer =
        xTimerCreate("Linkup", APP_MS2TICK(APP_LINKUP_TIMER_PERIOD_MS), pdFALSE, NULL, APP_LinkupTimerCallback);
    assert(linkupTimer);
    /* Create SRTM dispatcher */
    disp = SRTM_Dispatcher_Create();

    xTaskCreate(SRTM_MonitorTask, "SRTM monitor", 256U, NULL, APP_SRTM_MONITOR_TASK_PRIO, NULL);
    xTaskCreate(SRTM_DispatcherTask, "SRTM dispatcher", 512U, NULL, APP_SRTM_DISPATCHER_TASK_PRIO, NULL);
}
void APP_SRTM_Suspend(void)
{
    APP_SRTM_DeinitAudioDevice();
}

void APP_SRTM_Resume(void)
{
    APP_SRTM_InitAudioDevice();
}
