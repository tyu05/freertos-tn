/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "board.h"
#include "fsl_pdm.h"
#include "fsl_pdm_sdma.h"
#include "fsl_sdma.h"
#include "fsl_sai.h"
#include "fsl_sai_sdma.h"
#include "fsl_wm8524.h"
#include "fsl_debug_console.h"
#include "sdma_multi_fifo_script.h"

#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_common.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define DEMO_PDM_DMA SDMAARM3
#define DEMO_SAI_DMA SDMAARM2
#define DEMO_PDM PDM
#define DEMO_PDM_CLK_FREQ (24576000U)
#define DEMO_PDM_FIFO_WATERMARK (4U)
#define DEMO_PDM_QUALITY_MODE kPDM_QualityModeHigh
#define DEMO_PDM_CIC_OVERSAMPLE_RATE (0U)
#define DEMO_PDM_ENABLE_CHANNEL_LEFT (0U)
#define DEMO_PDM_ENABLE_CHANNEL_RIGHT (1U)
#define DEMO_PDM_SAMPLE_CLOCK_RATE (1536000U * 4) /* 6.144MHZ */
#define DEMO_PDM_DMA_REQUEST_SOURCE (24U)
#define DEMO_PDM_DMA_CHANNEL (2U)
#define DEMO_PDM_DMA_CHANNEL_PRIORITY (4U)

#define DEMO_SAI (I2S3)
#define DEMO_SAI_CLK_FREQ (24576000U)
#define DEMO_SAI_DMA_CHANNEL (1)
#define DEMO_SAI_DMA_CHANNEL_PRIORITY (3U)
#define DEMO_SAI_TX_DMA_REQUEST_SOURCE (5)
#define DEMO_SAI_CLOCK_SOURCE (1U)
#define DEMO_SAI_IRQn I2S3_IRQn

#define DEMO_CODEC_WM8524 (1)
#define DEMO_CODEC_BUS_PIN (NULL)
#define DEMO_CODEC_BUS_PIN_NUM (0)
#define DEMO_CODEC_MUTE_PIN (GPIO5)
#define DEMO_CODEC_MUTE_PIN_NUM (21)

#define BUFFER_SIZE (1024)
#define BUFFER_NUMBER (4)
/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void pdmSdmallback(PDM_Type *base, pdm_sdma_handle_t *handle, status_t status, void *userData);
static void saiCallback(I2S_Type *base, sai_sdma_handle_t *handle, status_t status, void *userData);
/*******************************************************************************
 * Variables
 ******************************************************************************/
AT_NONCACHEABLE_SECTION_ALIGN(pdm_sdma_handle_t s_pdmRxHandle, 4);
AT_NONCACHEABLE_SECTION_ALIGN(sdma_handle_t s_pdmDmaHandle, 4);
AT_NONCACHEABLE_SECTION_ALIGN(sdma_handle_t s_saiDmaHandle, 4);
AT_NONCACHEABLE_SECTION_ALIGN(sai_sdma_handle_t s_saiTxHandle, 4);
AT_NONCACHEABLE_SECTION_ALIGN(sdma_context_data_t s_pdmSdmaContext, 4);
AT_NONCACHEABLE_SECTION_ALIGN(sdma_context_data_t s_saiSdmaContext, 4);
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_buffer[BUFFER_SIZE * BUFFER_NUMBER], 4);
static volatile uint32_t s_bufferValidBlock = BUFFER_NUMBER;
static volatile uint32_t s_readIndex = 0U;
static volatile uint32_t s_writeIndex = 0U;
static const pdm_config_t pdmConfig = {
    .enableDoze = false,
    .fifoWatermark = DEMO_PDM_FIFO_WATERMARK,
    .qualityMode = DEMO_PDM_QUALITY_MODE,
    .cicOverSampleRate = DEMO_PDM_CIC_OVERSAMPLE_RATE,
};
static const pdm_channel_config_t channelConfig = {
    .cutOffFreq = kPDM_DcRemoverCutOff152Hz, .gain = kPDM_DfOutputGain4,
};

/*******************************************************************************
 * Code
 ******************************************************************************/
static void pdmSdmallback(PDM_Type *base, pdm_sdma_handle_t *handle, status_t status, void *userData)
{
    s_bufferValidBlock--;
}

static void saiCallback(I2S_Type *base, sai_sdma_handle_t *handle, status_t status, void *userData)
{
    if (kStatus_SAI_TxError == status)
    {
        /* Handle the error. */
    }
    else
    {
        s_bufferValidBlock++;
    }
}

void PDM_ERROR_IRQHandler(void)
{
    uint32_t fifoStatus = 0U;
    if (PDM_GetStatus(DEMO_PDM) & PDM_STAT_LOWFREQF_MASK)
    {
        PDM_ClearStatus(DEMO_PDM, PDM_STAT_LOWFREQF_MASK);
    }

    fifoStatus = PDM_GetFifoStatus(DEMO_PDM);
    if (fifoStatus)
    {
        PDM_ClearFIFOStatus(DEMO_PDM, fifoStatus);
    }
}

void SAI_UserIRQHandler(void)
{
    SAI_TxClearStatusFlags(DEMO_SAI, kSAI_FIFOErrorFlag);
}

void Codec_Init(void)
{
    volatile uint32_t delayCycle = 500000;

    wm8524_config_t codecConfig = {0};
    wm8524_handle_t codecHandle = {0};
    codecConfig.busPinNum = DEMO_CODEC_BUS_PIN_NUM;
    codecConfig.busPin = DEMO_CODEC_BUS_PIN;
    codecConfig.mutePin = DEMO_CODEC_MUTE_PIN;
    codecConfig.mutePinNum = DEMO_CODEC_MUTE_PIN_NUM;
    codecConfig.protocol = kWM8524_ProtocolI2S;
    WM8524_Init(&codecHandle, &codecConfig);

    while (delayCycle)
    {
        __ASM("nop");
        delayCycle--;
    }
}

/*!
 * @brief Main function
 */
int main(void)
{
    pdm_transfer_t pdmXfer;
    sdma_config_t dmaConfig = {0};
    sai_config_t config;
    sai_transfer_format_t format;
    sai_transfer_t saiXfer;
    uint32_t mclkSourceClockHz = 0U;

    /* Board specific RDC settings */
    BOARD_RdcInit();

    BOARD_InitPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();
    BOARD_InitMemory();

    CLOCK_SetRootMux(kCLOCK_RootPdm, kCLOCK_PdmRootmuxAudioPll1); /* Set PDM source from OSC 25MHZ */
    CLOCK_SetRootDivider(kCLOCK_RootPdm, 1U, 32U);                /* Set root clock to 25MHZ */

    CLOCK_SetRootMux(kCLOCK_RootSai3, kCLOCK_SaiRootmuxAudioPll1); /* Set SAI source to Audio PLL1 Div6 786432000HZ */
    CLOCK_SetRootDivider(kCLOCK_RootSai3, 1U, 32U);                /* Set root clock to 786432000HZ / 32 = 24576000HZ */

    memset(&format, 0U, sizeof(sai_transfer_format_t));

    PRINTF("PDM SAI sdma example started!\n\r");

    /* Create SDMA handle */
    SDMA_GetDefaultConfig(&dmaConfig);
    dmaConfig.ratio = kSDMA_ARMClockFreq;

    SDMA_Init(DEMO_PDM_DMA, &dmaConfig);
    SDMA_Init(DEMO_SAI_DMA, &dmaConfig);
    SDMA_CreateHandle(&s_pdmDmaHandle, DEMO_PDM_DMA, DEMO_PDM_DMA_CHANNEL, &s_pdmSdmaContext);
    SDMA_SetChannelPriority(DEMO_PDM_DMA, DEMO_PDM_DMA_CHANNEL, DEMO_PDM_DMA_CHANNEL_PRIORITY);
    SDMA_LoadScript(DEMO_PDM_DMA, SCRIPT_CODE_START_ADDR, (void *)sdma_multi_fifo_script,SCRIPT_CODE_SIZE * sizeof(short));

    SDMA_CreateHandle(&s_saiDmaHandle, DEMO_SAI_DMA, DEMO_SAI_DMA_CHANNEL, &s_saiSdmaContext);
    SDMA_SetChannelPriority(DEMO_SAI_DMA, DEMO_SAI_DMA_CHANNEL, DEMO_SAI_DMA_CHANNEL_PRIORITY);

    SAI_TxGetDefaultConfig(&config);
    config.bclkSource = (sai_bclk_source_t)DEMO_SAI_CLOCK_SOURCE;
    config.protocol = kSAI_BusI2S;
    SAI_TxInit(DEMO_SAI, &config);

    Codec_Init();

    /* Configure the audio format */
    format.bitWidth = kSAI_WordWidth16bits;
    format.channel = 0U;
    format.sampleRate_Hz = kSAI_SampleRate48KHz;
#if (defined FSL_FEATURE_SAI_HAS_MCLKDIV_REGISTER && FSL_FEATURE_SAI_HAS_MCLKDIV_REGISTER) || \
    (defined FSL_FEATURE_PCC_HAS_SAI_DIVIDER && FSL_FEATURE_PCC_HAS_SAI_DIVIDER)
    format.masterClockHz = OVER_SAMPLE_RATE * format.sampleRate_Hz;
#else
    format.masterClockHz = DEMO_SAI_CLK_FREQ;
#endif
    format.protocol = config.protocol;
    format.stereo = kSAI_Stereo;
    format.isFrameSyncCompact = true;
#if defined(FSL_FEATURE_SAI_FIFO_COUNT) && (FSL_FEATURE_SAI_FIFO_COUNT > 1)
    format.watermark = FSL_FEATURE_SAI_FIFO_COUNT / 2U;
#endif

    SAI_TransferTxCreateHandleSDMA(DEMO_SAI, &s_saiTxHandle, saiCallback, NULL, &s_saiDmaHandle,
                                   DEMO_SAI_TX_DMA_REQUEST_SOURCE);
    mclkSourceClockHz = DEMO_SAI_CLK_FREQ;
    SAI_TransferTxSetFormatSDMA(DEMO_SAI, &s_saiTxHandle, &format, mclkSourceClockHz, format.masterClockHz);

    /* Setup pdm */
    PDM_Init(DEMO_PDM, &pdmConfig);
    PDM_TransferCreateHandleSDMA(DEMO_PDM, &s_pdmRxHandle, pdmSdmallback, NULL, &s_pdmDmaHandle,
                                 DEMO_PDM_DMA_REQUEST_SOURCE);
    PDM_SetChannelConfigSDMA(DEMO_PDM, &s_pdmRxHandle, DEMO_PDM_ENABLE_CHANNEL_LEFT, &channelConfig);
    PDM_SetChannelConfigSDMA(DEMO_PDM, &s_pdmRxHandle, DEMO_PDM_ENABLE_CHANNEL_RIGHT, &channelConfig);
    PDM_SetSampleRate(DEMO_PDM, (1U << DEMO_PDM_ENABLE_CHANNEL_LEFT) | (1U << DEMO_PDM_ENABLE_CHANNEL_RIGHT),
                      pdmConfig.qualityMode, pdmConfig.cicOverSampleRate,
                      DEMO_PDM_CLK_FREQ / DEMO_PDM_SAMPLE_CLOCK_RATE);
    PDM_Reset(DEMO_PDM);

    while (1)
    {
        /* wait one buffer idle to recieve data */
        if (s_bufferValidBlock > 0)
        {
            pdmXfer.data = (uint8_t *)((uint32_t)s_buffer + s_readIndex * BUFFER_SIZE);
            pdmXfer.dataSize = BUFFER_SIZE;
            if (kStatus_Success == PDM_TransferReceiveSDMA(DEMO_PDM, &s_pdmRxHandle, &pdmXfer))
            {
                s_readIndex++;
            }
            if (s_readIndex == BUFFER_NUMBER)
            {
                s_readIndex = 0U;
            }
        }
        /* wait one buffer busy to send data */
        if (s_bufferValidBlock < BUFFER_NUMBER)
        {
            saiXfer.data = (uint8_t *)((uint32_t)s_buffer + s_writeIndex * BUFFER_SIZE);
            saiXfer.dataSize = BUFFER_SIZE;
            if (kStatus_Success == SAI_TransferSendSDMA(DEMO_SAI, &s_saiTxHandle, &saiXfer))
            {
                s_writeIndex++;
            }
            if (s_writeIndex == BUFFER_NUMBER)
            {
                s_writeIndex = 0U;
            }
        }
    }
}
