/*
 * HwInit.c
 *
 * Copyright(c) 1998 - 2009 Texas Instruments. All rights reserved.      
 * All rights reserved.                                                  
 *                                                                       
 * Redistribution and use in source and binary forms, with or without    
 * modification, are permitted provided that the following conditions    
 * are met:                                                              
 *                                                                       
 *  * Redistributions of source code must retain the above copyright     
 *    notice, this list of conditions and the following disclaimer.      
 *  * Redistributions in binary form must reproduce the above copyright  
 *    notice, this list of conditions and the following disclaimer in    
 *    the documentation and/or other materials provided with the         
 *    distribution.                                                      
 *  * Neither the name Texas Instruments nor the names of its            
 *    contributors may be used to endorse or promote products derived    
 *    from this software without specific prior written permission.      
 *                                                                       
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS   
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT     
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT  
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT      
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT   
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */



/*******************************************************************************/
/*                                                                             */
/*  MODULE:  HwInit.c                                                          */
/*  PURPOSE: HwInit module manages the init process of the TNETW, included     */
/*           firmware download process. It shall perform Hard Reset the chip   */
/*           if possible (this will require a Reset line to be connected to    */
/*           the host); Start InterfaceCtrl; Download NVS and FW               */
/*                                                                             */
/*                                                                             */
/*******************************************************************************/

#define __FILE_ID__  FILE_ID_105
#include "tidef.h"
#include "osApi.h"
#include "report.h"
#include "HwInit_api.h"
#include "FwEvent_api.h"
#include "TwIf.h"
#include "TWDriver.h"
#include "TWDriverInternal.h"
#include "eventMbox_api.h"
#include "CmdBld.h"
#include "CmdMBox_api.h"


/* remove the chipID check when WL6-PG1.0 becomes obsolete (temporary global variable!!) */
TI_BOOL bChipIs1273Pg10 = TI_TRUE;


extern void TWD_FinalizeOnFailure   (TI_HANDLE hTWD);
extern void cmdBld_FinalizeDownload (TI_HANDLE hCmdBld, TBootAttr *pBootAttr, FwStaticData_t *pFwInfo);


/************************************************************************
 * Defines
 ************************************************************************/

/* Download phase partition */
#define PARTITION_DOWN_MEM_ADDR       0                 
#define PARTITION_DOWN_MEM_SIZE       0x177C0           
#define PARTITION_DOWN_REG_ADDR       REGISTERS_BASE	
#define PARTITION_DOWN_REG_SIZE       0x8800            

/* Working phase partition */
#define PARTITION_WORK_MEM_ADDR       0x40000
#define PARTITION_WORK_MEM_SIZE       0x14FC0
#define PARTITION_WORK_REG_ADDR       REGISTERS_BASE    
#define PARTITION_WORK_REG_SIZE       0xB000	        

/* DRPW setting partition */
#define PARTITION_DRPW_MEM_ADDR       0x40000
#define PARTITION_DRPW_MEM_SIZE       0x14FC0
#define PARTITION_DRPW_REG_ADDR       DRPW_BASE         
#define PARTITION_DRPW_REG_SIZE       0x6000	        

/* Total range of bus addresses range */
#define PARTITION_TOTAL_ADDR_RANGE    0x1FFC0

/* Maximal block size in a single SDIO transfer --> Firmware image load chunk size */
#ifdef _VLCT_
#define MAX_SDIO_BLOCK					(4000)	
#else
#define MAX_SDIO_BLOCK					(500)	
#endif

#define ACX_EEPROMLESS_IND_REG        (SCR_PAD4)
#define USE_EEPROM                    (0)
#define SOFT_RESET_MAX_TIME           (1000000)
#define SOFT_RESET_STALL_TIME         (1000)
#define NVS_DATA_BUNDARY_ALIGNMENT    (4)

#define MAX_HW_INIT_CONSECUTIVE_TXN     15

#define WORD_SIZE                       4
#define WORD_ALIGNMENT_MASK             0x3
#define DEF_NVS_SIZE                    ((NVS_PRE_PARAMETERS_LENGTH) + (NVS_TX_TYPE_INDEX) + 4)


/************************************************************************
 * Macros
 ************************************************************************/

#define SET_DEF_NVS(aNVS)     aNVS[0]=0x01; aNVS[1]=0x6d; aNVS[2]=0x54; aNVS[3]=0x56; aNVS[4]=0x34; \
                              aNVS[5]=0x12; aNVS[6]=0x28; aNVS[7]=0x01; aNVS[8]=0x71; aNVS[9]=0x54; \
                              aNVS[10]=0x00; aNVS[11]=0x08; aNVS[12]=0x00; aNVS[13]=0x00; aNVS[14]=0x00; \
                              aNVS[15]=0x00; aNVS[16]=0x00; aNVS[17]=0x00; aNVS[18]=0x00; aNVS[19]=0x00; \
                              aNVS[20]=0x00; aNVS[21]=0x00; aNVS[22]=0x00; aNVS[23]=0x00; aNVS[24]=eNVS_NON_FILE;\
							  aNVS[25]=0x00; aNVS[26]=0x00; aNVS[27]=0x00;



#define HW_INIT_PTXN_SET(pHwInit, pTxn)  pTxn = (TTxnStruct*)&(pHwInit->aHwInitTxn[pHwInit->uTxnIndex].tTxnStruct);

#define BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, uAddr, uVal, uSize, direction, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, direction); \
                              pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData = (TI_UINT32)uVal; \
                              BUILD_TTxnStruct(pTxn, uAddr, &(pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData), uSize, fCB, hCB)

#define BUILD_HW_INIT_FW_STATIC_TXN(pHwInit, pTxn, uAddr, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, TXN_DIRECTION_READ); \
                              BUILD_TTxnStruct(pTxn, uAddr, &(pHwInit->tFwStaticTxn.tFwStaticInfo), sizeof(FwStaticData_t), fCB, hCB)

#define BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, uAddr, uVal, uSize, direction, fCB, hCB)     \
                              HW_INIT_PTXN_SET(pHwInit, pTxn) \
                              TXN_PARAM_SET_DIRECTION(pTxn, direction); \
                              BUILD_TTxnStruct(pTxn, uAddr, uVal, uSize, fCB, hCB)


/* Handle return status inside a state machine */
#define EXCEPT(phwinit,status)                                   \
    switch (status) {                                           \
        case TI_OK:                                             \
        case TXN_STATUS_OK:                                     \
        case TXN_STATUS_COMPLETE:                               \
             break;                                             \
        case TXN_STATUS_PENDING:                                \
             return TXN_STATUS_PENDING;                         \
        default:                                                \
             TWD_FinalizeOnFailure (phwinit->hTWD);             \
             return TXN_STATUS_ERROR;                           \
    }


/* Handle return status inside an init sequence state machine  */
#define EXCEPT_I(phwinit,status)                                \
    switch (status) {                                           \
        case TI_OK:                                             \
        case TXN_STATUS_COMPLETE:                               \
             break;                                             \
        case TXN_STATUS_PENDING:                                \
             phwinit->uInitSeqStatus = status;                  \
             return TXN_STATUS_PENDING;                         \
        default:                                                \
             TWD_FinalizeOnFailure (phwinit->hTWD);             \
             return TXN_STATUS_ERROR;                           \
    }


/* Handle return status inside a load image state machine */
#define EXCEPT_L(phwinit,status)                                \
    switch (status) {                                           \
        case TXN_STATUS_OK:                                     \
        case TXN_STATUS_COMPLETE:                               \
             break;                                             \
        case TXN_STATUS_PENDING:                                \
             phwinit->DownloadStatus = status;                  \
             return TXN_STATUS_PENDING;                         \
        default:                                                \
             phwinit->DownloadStatus = status;                  \
             TWD_FinalizeOnFailure (phwinit->hTWD);             \
             return TXN_STATUS_ERROR;                           \
    }


/************************************************************************
 * Types
 ************************************************************************/

enum
{
    REF_FREQ_19_2                   = 0,
    REF_FREQ_26_0                   = 1,
    REF_FREQ_38_4                   = 2,
    REF_FREQ_40_0                   = 3,
    REF_FREQ_33_6                   = 4,
    REF_FREQ_NUM                    = 5
};

enum
{
    LUT_PARAM_INTEGER_DIVIDER       = 0,
    LUT_PARAM_FRACTIONAL_DIVIDER    = 1,
    LUT_PARAM_ATTN_BB               = 2,
    LUT_PARAM_ALPHA_BB              = 3,
    LUT_PARAM_STOP_TIME_BB          = 4,
    LUT_PARAM_BB_PLL_LOOP_FILTER    = 5,
    LUT_PARAM_NUM                   = 6
};

typedef struct 
{
    TTxnStruct              tTxnStruct;
    TI_UINT32               uData; 

} THwInitTxn;

typedef struct 
{
    TTxnStruct              tTxnStruct;
    FwStaticData_t          tFwStaticInfo; 

} TFwStaticTxn;


/* The HW Init module object */
typedef struct 
{
    /* Handles */
    TI_HANDLE               hOs;
    TI_HANDLE               hReport;
    TI_HANDLE               hTWD;
    TI_HANDLE               hBusTxn;
    TI_HANDLE               hTwIf;

    TI_HANDLE 		    hFileInfo;	/* holds parameters of FW Image Portion - for DW Download */
    TEndOfHwInitCb          fInitHwCb;

    /* Firmware image ptr */
    TI_UINT8               *pFwBuf;       
    /* Firmware image length */
    TI_UINT32               uFwLength;
    TI_UINT32               uFwAddress;
    TI_UINT32               bFwBufLast;  
    TI_UINT32               uFwLastAddr;  
    /* EEPROM image ptr */
    TI_UINT8               *pEEPROMBuf;   
    /* EEPROM image length */
    TI_UINT32               uEEPROMLen;   

    TI_UINT8               *pEEPROMCurPtr;
    TI_UINT32               uEEPROMCurLen;
    TBootAttr               tBootAttr;
    TI_HANDLE               hHwCtrl;
    ETxnStatus              DownloadStatus;
    /* Upper module callback for the init stage */
    fnotify_t               fCb;          
    /* Upper module handle for the init stage */
    TI_HANDLE               hCb;          
    /* Init stage */
    TI_UINT32               uInitStage;   
    /* Reset statge */ 
    TI_UINT32               uResetStage;  
    /* EEPROM burst stage */
    TI_UINT32               uEEPROMStage; 
    /* Init state machine temporary data */
    TI_UINT32               uInitData;    
    /* ELP command image */
    TI_UINT32               uElpCmd;      
    /* Chip ID */
    TI_UINT32               uChipId;      
    /* Boot state machine temporary data */
    TI_UINT32               uBootData;    
    TI_UINT32               uSelfClearTime;
    TI_UINT8                uEEPROMBurstLen;
    TI_UINT8                uEEPROMBurstLoop;
    TI_UINT32               uEEPROMRegAddr;
    TI_STATUS               uEEPROMStatus;
    TI_UINT32               uNVSStartAddr;
    TI_UINT32               uNVSNumChar;
    TI_UINT32               uNVSNumByte;
    TI_STATUS               uNVSStatus;
    TI_UINT32               uScrPad6;
    TI_UINT32               uRefFreq; 
    TI_UINT32               uInitSeqStage;
    TI_STATUS               uInitSeqStatus;
    TI_UINT32               uLoadStage;
    TI_UINT32               uBlockReadNum;
    TI_UINT32               uBlockWriteNum;
    TI_UINT32               uPartitionLimit;
    TI_UINT32               uFinStage;
    TI_UINT32               uFinData;
    TI_UINT32               uFinLoop; 
     TI_UINT32               uRegStage;
    TI_UINT32               uRegLoop;
    TI_UINT32               uRegSeqStage;
    TI_UINT32               uRegData;  

    TI_UINT8                auFwTmpBuf [WSPI_PAD_LEN_WRITE + MAX_SDIO_BLOCK];

    TFinalizeCb             fFinalizeDownload;
    TI_HANDLE               hFinalizeDownload;
    /* Size of the Fw image, retrieved from the image itself */         
    TI_UINT32               uFwDataLen; 
    TI_UINT8                aDefaultNVS[DEF_NVS_SIZE];
    TI_UINT8                uTxnIndex;
    THwInitTxn              aHwInitTxn[MAX_HW_INIT_CONSECUTIVE_TXN];
    TFwStaticTxn            tFwStaticTxn;

    TI_UINT32               uSavedDataForWspiHdr;  /* For saving the 4 bytes before the NVS data for WSPI case 
                                                        where they are overrun by the WSPI BusDrv */
} THwInit;


/************************************************************************
 * Local Functions Prototypes
 ************************************************************************/
static void      hwInit_SetPartition                (THwInit   *pHwInit, 
                                                     TI_UINT32 uMemAddr,
                                                     TI_UINT32 uMemSize,
                                                     TI_UINT32 uRegAddr,
                                                     TI_UINT32 uRegSize);
static TI_STATUS hwInit_BootSm                      (TI_HANDLE hHwInit);
static TI_STATUS hwInit_ResetSm                     (TI_HANDLE hHwInit);
static TI_STATUS hwInit_EepromlessStartBurstSm      (TI_HANDLE hHwInit);                                                   
static TI_STATUS hwInit_LoadFwImageSm               (TI_HANDLE hHwInit);
static TI_STATUS hwInit_FinalizeDownloadSm          (TI_HANDLE hHwInit);                                             




/*******************************************************************************
*                       PUBLIC  FUNCTIONS  IMPLEMENTATION                      *
********************************************************************************/


/*************************************************************************
*                        hwInit_Create                                   *
**************************************************************************
* DESCRIPTION:  This function initializes the HwInit module.
*
* INPUT:        hOs - handle to Os Abstraction Layer
*               
* RETURN:       Handle to the allocated HwInit module
*************************************************************************/
TI_HANDLE hwInit_Create (TI_HANDLE hOs)
{
    THwInit *pHwInit;

    /* Allocate HwInit module */
    pHwInit = os_memoryAlloc (hOs, sizeof(THwInit));

    if (pHwInit == NULL)
    {
        WLAN_OS_REPORT(("Error allocating the HwInit Module\n"));
        return NULL;
    }

    /* Reset HwInit module */
    os_memoryZero (hOs, pHwInit, sizeof(THwInit));

    pHwInit->hOs = hOs;

    return (TI_HANDLE)pHwInit;
}


/***************************************************************************
*                           hwInit_Destroy                                 *
****************************************************************************
* DESCRIPTION:  This function unload the HwInit module. 
*
* INPUTS:       hHwInit - the object
*
* OUTPUT:
*
* RETURNS:      TI_OK - Unload succesfull
*               TI_NOK - Unload unsuccesfull
***************************************************************************/
TI_STATUS hwInit_Destroy (TI_HANDLE hHwInit)
{
    THwInit *pHwInit = (THwInit *)hHwInit;

    /* Free HwInit Module */
    os_memoryFree (pHwInit->hOs, pHwInit, sizeof(THwInit));

    return TI_OK;
}


/***************************************************************************
*                           hwInit_Init                                    *
****************************************************************************
* DESCRIPTION:  This function configures the hwInit module
*
* RETURNS:      TI_OK - Configuration successful
*               TI_NOK - Configuration unsuccessful
***************************************************************************/
TI_STATUS hwInit_Init (TI_HANDLE      hHwInit,
                         TI_HANDLE      hReport,
                         TI_HANDLE      hTWD,
                         TI_HANDLE 	hFinalizeDownload, 
			 TFinalizeCb    fFinalizeDownload, 
                         TEndOfHwInitCb fInitHwCb)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TTxnStruct* pTxn;

    /* Configure modules handles */
    pHwInit->hReport    = hReport;
    pHwInit->hTWD       = hTWD;
    pHwInit->hTwIf      = ((TTwd *)hTWD)->hTwIf;
    pHwInit->hOs        = ((TTwd *)hTWD)->hOs;
    pHwInit->fInitHwCb  = fInitHwCb;
    pHwInit->fFinalizeDownload 	= fFinalizeDownload;
    pHwInit->hFinalizeDownload 	= hFinalizeDownload;

    SET_DEF_NVS(pHwInit->aDefaultNVS)

    for (pHwInit->uTxnIndex=0;pHwInit->uTxnIndex<MAX_HW_INIT_CONSECUTIVE_TXN;pHwInit->uTxnIndex++)
    {
        HW_INIT_PTXN_SET(pHwInit, pTxn)
        /* Setting write as default transaction */
        TXN_PARAM_SET(pTxn, TXN_LOW_PRIORITY, TXN_FUNC_ID_WLAN, TXN_DIRECTION_WRITE, TXN_INC_ADDR)
    }

    TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT, ".....HwInit configured successfully\n");
    
    return TI_OK;
}


TI_STATUS hwInit_SetNvsImage (TI_HANDLE hHwInit, TI_UINT8 *pbuf, TI_UINT32 length)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;

    pHwInit->pEEPROMBuf = pbuf;
    pHwInit->uEEPROMLen = length; 

    return TI_OK;
}


TI_STATUS hwInit_SetFwImage (TI_HANDLE hHwInit, TFileInfo *pFileInfo)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;

    if ((hHwInit == NULL) || (pFileInfo == NULL))
    {
	return TI_NOK;
    }

    pHwInit->pFwBuf 	= pFileInfo->pBuffer;
    pHwInit->uFwLength  = pFileInfo->uLength;
    pHwInit->uFwAddress = pFileInfo->uAddress;
    pHwInit->bFwBufLast = pFileInfo->bLast;

    return TI_OK;
}


/** 
 * \fn     hwInit_SetPartition
 * \brief  Set HW addresses partition
 * 
 * Set the HW address ranges for download or working memory and registers access.
 * Generate and configure the bus access address mapping table.
 * The partition is split between register (fixed partition of 24KB size, exists in all modes), 
 *     and memory (dynamically changed during init and gets constant value in run-time, 104KB size).
 * The TwIf configures the memory mapping table on the device by issuing write transaction to 
 *     table address (note that the TxnQ and bus driver see this as a regular transaction). 
 * 
 * \note In future versions, a specific bus may not support partitioning (as in wUART), 
 *       In this case the HwInit module shall not call this function (will learn the bus 
 *       configuration from the INI file).
 *
 * \param  pHwInit   - The module's object
 * \param  uMemAddr  - The memory partition base address
 * \param  uMemSize  - The memory partition size
 * \param  uRegAddr  - The registers partition base address
 * \param  uRegSize  - The register partition size
 * \return void
 * \sa     
 */ 
static void hwInit_SetPartition (THwInit   *pHwInit, 
                                 TI_UINT32 uMemAddr,
                                 TI_UINT32 uMemSize,
                                 TI_UINT32 uRegAddr,
                                 TI_UINT32 uRegSize)
{
    /* Verify that the total bus address space is not exceeded */
    if (uMemSize + uRegSize  >  PARTITION_TOTAL_ADDR_RANGE)
    {
        TRACE3(pHwInit->hReport, REPORT_SEVERITY_ERROR, "hwInit_SetPartition: Total range exceeded: MemSize=0x%x, RegSize=0x%x MaxSize=0x%x\n", uMemSize, uRegSize, PARTITION_TOTAL_ADDR_RANGE);
        return;
    }

    /* Verify that the memory partition don't overlap the registers partition */
    if ((uMemAddr <= uRegAddr)  &&  (uMemAddr + uMemSize  >  uRegAddr))
    {
        TRACE4(pHwInit->hReport, REPORT_SEVERITY_ERROR, "hwInit_SetPartition: Mem range overlap Reg: MemAddr=0x%x, MemSize=0x%x, RegAddr=0x%x RegSize=0x%x\n", uMemAddr, uMemSize, uRegAddr, uRegSize);
        return;
    }
	if ((uRegAddr <= uMemAddr)  &&  (uRegAddr + uRegSize  >  uMemAddr))
    {
        TRACE4(pHwInit->hReport, REPORT_SEVERITY_ERROR, "hwInit_SetPartition: Reg range overlap Mem: MemAddr=0x%x, MemSize=0x%x, RegAddr=0x%x RegSize=0x%x\n", uMemAddr, uMemSize, uRegAddr, uRegSize);
        return;
    }
    
    TRACE4(pHwInit->hReport, REPORT_SEVERITY_INFORMATION, "hwInit_SetPartition: uMemAddr=0x%x, MemSize=0x%x uRegAddr=0x%x, RegSize=0x%x\n", uMemAddr, uMemSize, uRegAddr, uRegSize);

    /* Prepare partition Txn data and send to HW */
    twIf_SetPartition (pHwInit->hTwIf, uMemAddr, uMemSize, uRegAddr, uRegSize);
}


/****************************************************************************
 *                      hwInit_Boot()
 ****************************************************************************
 * DESCRIPTION: Start HW init sequence which writes and reads some HW registers
 *                  that are needed prior to FW download.
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
TI_STATUS hwInit_Boot (TI_HANDLE hHwInit)
{ 
    THwInit      *pHwInit = (THwInit *)hHwInit;
    TTwd         *pTWD = (TTwd *)pHwInit->hTWD;
    TWlanParams  *pWlanParams = &DB_WLAN(pTWD->hCmdBld);
    TBootAttr     tBootAttr;

    tBootAttr.MacClock = pWlanParams->MacClock;
    tBootAttr.ArmClock = pWlanParams->ArmClock;

    /*
     * Initialize the status of download to  pending 
     * It will be set to TXN_STATUS_COMPLETE at the FinalizeDownload function 
     */
    pHwInit->DownloadStatus = TXN_STATUS_PENDING;

    /* Call the boot sequence state machine */
    pHwInit->uInitStage = 0;

    os_memoryCopy (pHwInit->hOs, &pHwInit->tBootAttr, &tBootAttr, sizeof(TBootAttr));

    hwInit_BootSm (hHwInit);

    /*
     * If it returns the status of the StartInstance only then we can here query for the download status 
     * and then return the status up to the TNETW_Driver.
     * This return value will go back up to the TNETW Driver layer so that the init from OS will know
     * if to wait for the InitComplte or not in case of TXN_STATUS_ERROR.
     * This value will always be pending since the SPI is ASYNC 
     * and in SDIOa timer is set so it will be ASync also in anyway.
     */
    return pHwInit->DownloadStatus;
}


 /****************************************************************************
 * DESCRIPTION: Firmware boot state machine
 * 
 * INPUTS:  
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK 
 ****************************************************************************/
static TI_STATUS hwInit_BootSm (TI_HANDLE hHwInit)
{
    THwInit    *pHwInit = (THwInit *)hHwInit;
    TI_STATUS   status = 0;
    TTxnStruct  *pTxn;
    TI_UINT32   uData;
    TTwd        *pTWD        = (TTwd *) pHwInit->hTWD;
    IniFileGeneralParam  *pGenParams = &DB_GEN(pTWD->hCmdBld);
    TI_UINT32   clkVal = 0x3;

    switch (pHwInit->uInitStage)
    {
    case 0:
        pHwInit->uInitStage++;
        pHwInit->uTxnIndex = 0;

        /* Set the bus addresses partition to its "running" mode */
        hwInit_SetPartition (pHwInit, 
                             PARTITION_WORK_MEM_ADDR, 
                             PARTITION_WORK_MEM_SIZE, 
                             PARTITION_WORK_REG_ADDR, 
                             PARTITION_WORK_REG_SIZE);

#ifdef _VLCT_
         /* Set FW to test mode */    
         BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD8, 0xBABABABE, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
         twIf_Transact(pHwInit->hTwIf, pTxn);
         pHwInit->uTxnIndex++;
#endif

           if (( 0 == pGenParams->RefClk) || (2 == pGenParams->RefClk))
        {/* ref clk: 19.2/38.4 */
            clkVal = 0x3;
        }
          if (( 1 == pGenParams->RefClk) || (3 == pGenParams->RefClk))
        {/* ref clk: 26/52 */
            clkVal = 0x5;
        }

        WLAN_OS_REPORT(("CHIP VERSION... set 1273 chip top registers\n"));

        /* set the reference clock freq' to be used (pll_selinpfref field) */        
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, PLL_PARAMETERS, clkVal,
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        pHwInit->uTxnIndex++;

        /* read the PAUSE value to highest threshold */        
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, PLL_PARAMETERS, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);

        EXCEPT (pHwInit, status)

    case 1:
        pHwInit->uInitStage ++;
        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
        uData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
        uData &= ~(0x3ff);

        /* Now we can zero the index */
        pHwInit->uTxnIndex = 0;

        /* set the the PAUSE value to highest threshold */        
        uData |= WU_COUNTER_PAUSE_VAL;
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, WU_COUNTER_PAUSE, uData, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        pHwInit->uTxnIndex++;

        /* Continue the ELP wake up sequence */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, WELP_ARM_COMMAND, WELP_ARM_COMMAND_VAL, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        /* Wait 500uS */
        os_StalluSec (pHwInit->hOs, 500);

        /* Set the bus addresses partition to DRPw registers region */
        hwInit_SetPartition (pHwInit, 
                             PARTITION_DRPW_MEM_ADDR, 
                             PARTITION_DRPW_MEM_SIZE, 
                             PARTITION_DRPW_REG_ADDR, 
                             PARTITION_DRPW_REG_SIZE);

        pHwInit->uTxnIndex++;

        /* Read-modify-write DRPW_SCRATCH_START register (see next state) to be used by DRPw FW. 
           The RTRIM value will be added  by the FW before taking DRPw out of reset */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, DRPW_SCRATCH_START, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);

        EXCEPT (pHwInit, status)

    case 2:
        pHwInit->uInitStage ++;

        /* multiply fref value by 2, so that {0,1,2,3} values will become {0,2,4,6} */
        /* Then, move it 4 places to the right, to alter Fref relevant bits in register 0x2c */
        clkVal = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
        pHwInit->uTxnIndex = 0; /* Reset index only after getting the last read value! */
        clkVal |= (pGenParams->RefClk << 1) << 4;
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, DRPW_SCRATCH_START, clkVal, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        pHwInit->uTxnIndex++;

        /* Set the bus addresses partition back to its "running" mode */
        hwInit_SetPartition (pHwInit, 
                             PARTITION_WORK_MEM_ADDR, 
                             PARTITION_WORK_MEM_SIZE, 
                             PARTITION_WORK_REG_ADDR, 
                             PARTITION_WORK_REG_SIZE);

        /* 
         * end of CHIP init seq.
         */

        /* Disable interrupts */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_MASK, ACX_INTR_ALL, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
        twIf_Transact(pHwInit->hTwIf, pTxn);

        pHwInit->uTxnIndex++;

        /* Read the CHIP ID to get an indication that the bus is TI_OK */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, CHIP_ID, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);

        EXCEPT (pHwInit, status)
        
    case 3:
        pHwInit->uInitStage ++;

        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
         pHwInit->uChipId = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;

        /* This is only sanity check that the HW exists, we can continue and fail on FwLoad */
		if (pHwInit->uChipId == CHIP_ID_1273_PG10)
        {
            WLAN_OS_REPORT(("Working on a 1273 PG 1.0 board.\n"));
            bChipIs1273Pg10 = TI_TRUE;
        }
		else if (pHwInit->uChipId == CHIP_ID_1273_PG20)
        {
            WLAN_OS_REPORT(("Working on a 1273 PG 2.0 board.\n"));
            bChipIs1273Pg10 = TI_FALSE;
        }
        else 
        {
            WLAN_OS_REPORT (("Error!! Found unknown Chip Id = 0x%x\n", pHwInit->uChipId));
            bChipIs1273Pg10 = TI_FALSE;

            /*
             * NOTE: no exception because of forward compatibility
             */
        }
    
        /*
         * Soft reset 
         */
        pHwInit->uResetStage = 0;
        pHwInit->uSelfClearTime = 0;
        pHwInit->uBootData = 0;
        status = hwInit_ResetSm (pHwInit);    

        EXCEPT (pHwInit, status)

    case 4:
        pHwInit->uInitStage ++;

        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "TNET SOFT-RESET\n");

        WLAN_OS_REPORT(("Starting to process NVS...\n"));

        /*
         * Start EEPROM/NVS burst
         */

        if (pHwInit->pEEPROMBuf) 
        {
            /* NVS file exists (EEPROM-less support) */
            pHwInit->uEEPROMCurLen = pHwInit->uEEPROMLen;

            TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "EEPROM Image addr=0x%x, EEPROM Len=0x0x%x\n", pHwInit->pEEPROMBuf, pHwInit->uEEPROMLen);
            WLAN_OS_REPORT (("NVS found, EEPROM Image addr=0x%x, EEPROM Len=0x0x%x\n", 
                              pHwInit->pEEPROMBuf, pHwInit->uEEPROMLen));
        }
        else
        {
            WLAN_OS_REPORT (("No Nvs, Setting default MAC address\n"));
            pHwInit->uEEPROMCurLen = DEF_NVS_SIZE;
            pHwInit->pEEPROMBuf = (TI_UINT8*)(&pHwInit->aDefaultNVS[0]);
            WLAN_OS_REPORT (("pHwInit->uEEPROMCurLen: %x\n", pHwInit->uEEPROMCurLen));

        }

        pHwInit->pEEPROMCurPtr = pHwInit->pEEPROMBuf;
        pHwInit->uEEPROMStage = 0;
        status = hwInit_EepromlessStartBurstSm (hHwInit);

        EXCEPT (pHwInit, status)
        
    case 5: 
        pHwInit->uInitStage ++;
        pHwInit->uTxnIndex = 0;

        if (pHwInit->pEEPROMBuf) 
        {
            /* Signal FW that we are eeprom less */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_EEPROMLESS_IND_REG, ACX_EEPROMLESS_IND_REG, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "DRIVER NVS BURST-READ\n");
        }
        else
        {
	    /* 1273 - EEPROM is not support by FPGA yet */ 
            /*
             * Start ACX EEPROM
             */     
            /*pHwInit->uRegister = START_EEPROM_MGR;
            TXN_PARAM_SET(pTxn, TXN_LOW_PRIORITY, TXN_FUNC_ID_WLAN, TXN_DIRECTION_WRITE, TXN_INC_ADDR)
            BUILD_TTxnStruct(pTxn, ACX_REG_EE_START, &pHwInit->uRegister, REGISTER_SIZE, 0, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);*/

            /*
             * The stall is needed so the EEPROM NVS burst read will complete
             */     
            os_StalluSec (pHwInit->hOs, 40000);

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_EEPROMLESS_IND_REG, USE_EEPROM, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "STARTING EEPROM NVS BURST-READ\n");
        }

        pHwInit->uTxnIndex++;

        /* Read Chip ID */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn,  CHIP_ID, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);

        EXCEPT (pHwInit, status)

    case 6:
        pHwInit->uInitStage ++;
        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
        pHwInit->uBootData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
        /* Now we can zero the index */
        pHwInit->uTxnIndex = 0;

        WLAN_OS_REPORT(("Chip ID is 0x%X.\n", pHwInit->uBootData));
        /* if the WLAN_EN is ON but MainClock is problamtic the chip-id will be zero*/
        if (pHwInit->uBootData == 0)
        {
         WLAN_OS_REPORT(("Cannot read ChipID stopping\n", pHwInit->uBootData));
         TWD_FinalizeOnFailure (pHwInit->hTWD); 
         return TXN_STATUS_ERROR; 
        }


		
        /* Read Scr2 to verify that the HW is ready */
        BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SCR_PAD2, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ,(TTxnDoneCb)hwInit_BootSm, hHwInit)
        status = twIf_Transact(pHwInit->hTwIf, pTxn);
        EXCEPT (pHwInit, status)

    case 7:
        pHwInit->uInitStage ++;
        /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
        pHwInit->uBootData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;

        if (pHwInit->uBootData == 0xffffffff)
        {
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_FATAL_ERROR , "Error in SCR_PAD2 register\n");
            EXCEPT (pHwInit, TXN_STATUS_ERROR)
        }

        /* Call the restart sequence */
        pHwInit->uInitSeqStage = 0;
        pHwInit->uInitSeqStatus = TXN_STATUS_COMPLETE;

        EXCEPT (pHwInit, status)

    case 8:
        pHwInit->uInitStage = 0;
        
        /* Set the Download Status to COMPLETE */
        pHwInit->DownloadStatus = TXN_STATUS_COMPLETE;

        /* Call upper layer callback */
        if (pHwInit->fInitHwCb)
        {
            (*pHwInit->fInitHwCb) (pHwInit->hTWD);
        }

        return TI_OK;
    }

    return TI_OK;
}


TI_STATUS hwInit_LoadFw (TI_HANDLE hHwInit)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TI_STATUS  status;

    /* check parameters */
    if (hHwInit == NULL)
    {
        EXCEPT (pHwInit, TXN_STATUS_ERROR)
    }

    if (pHwInit->pFwBuf)
    {
        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "CPU halt -> download code\n");

        /* Load firmware image */ 
        pHwInit->uLoadStage = 0;
        status = hwInit_LoadFwImageSm (pHwInit);

        switch (status)
        {
        case TXN_STATUS_COMPLETE:
            WLAN_OS_REPORT (("Firmware successfully downloaded.\n"));
            break;
        case TXN_STATUS_PENDING:
            WLAN_OS_REPORT (("Starting to download firmware...\n"));
            break;
        default:
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Firmware download failed!\n");
            break;
        }

        EXCEPT (pHwInit, status);
    }   
    else
    {
        TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Firmware not downloaded...\n");

        EXCEPT (pHwInit, TXN_STATUS_ERROR)
    }
            
    WLAN_OS_REPORT (("FW download OK...\n"));
    return TI_OK;
}                                                  
    

/****************************************************************************
 *                      hwInit_FinalizeDownloadSm()
 ****************************************************************************
 * DESCRIPTION: Run the Hardware firmware
 *              Wait for Init Complete
 *              Configure the Bus Access with Addresses available on the scratch pad register 
 *              Change the SDIO/SPI partitions to be able to see all the memory addresses
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: None
 ****************************************************************************/
static TI_STATUS hwInit_FinalizeDownloadSm (TI_HANDLE hHwInit)
{
    THwInit  *pHwInit = (THwInit *)hHwInit;
    TTwd     *pTWD = (TTwd *)pHwInit->hTWD;
    TI_STATUS status = TI_OK;
    TI_UINT32   uIntVect;
    TTxnStruct* pTxn;

#ifdef _VLCT_
    #define FIN_LOOP 10
#else
    #define FIN_LOOP 20000
#endif

    while (TI_TRUE)
    {
        switch (pHwInit->uFinStage)
        {
        case 0:
            pHwInit->uFinStage = 1;
            pHwInit->uTxnIndex = 0;
            /*
             * Run the firmware (I) - Read current value from ECPU Control Reg.
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_ECPU_CONTROL, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

        case 1:
            pHwInit->uFinStage ++;
            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            pHwInit->uFinData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;

            /*
             * Run the firmware (II) - Take HW out of reset (write ECPU_CONTROL_HALT to ECPU Control Reg.)
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_ECPU_CONTROL, (pHwInit->uFinData | ECPU_CONTROL_HALT), 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            WLAN_OS_REPORT (("Firmware running.\n"));

            /* 
             * CHIP ID Debug
             */     

            pHwInit->uTxnIndex++;                  

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, CHIP_ID, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

        case 2:
            pHwInit->uFinStage ++;
            pHwInit->uFinLoop = 0;

            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            pHwInit->uFinData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
                               
            TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT , "CHIP ID IS %x\n", pHwInit->uFinData);

            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Wait init complete\n");

        case 3:
            pHwInit->uTxnIndex = 0;

            /* 
             * Wait for init complete 
             */
            if (pHwInit->uFinLoop < FIN_LOOP)
            {           
                pHwInit->uFinStage = 4;

                os_StalluSec (pHwInit->hOs, 50);

                /* Read interrupt status register */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_NO_CLEAR, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);

                EXCEPT (pHwInit, status)
            }
            else
			{
				pHwInit->uFinStage = 5;
			}                
            continue;

        case 4:
            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            pHwInit->uFinData = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;
            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;
            
            if (pHwInit->uFinData == 0xffffffff) /* error */
            {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Error reading hardware complete init indication\n");

                pHwInit->DownloadStatus = TXN_STATUS_ERROR;
                EXCEPT (pHwInit, TXN_STATUS_ERROR)
            }

            if (IS_MASK_ON (pHwInit->uFinData, ACX_INTR_INIT_COMPLETE))
            {
                pHwInit->uFinStage = 5;

                /* Interrupt ACK */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_ACK, ACX_INTR_INIT_COMPLETE, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
                twIf_Transact(pHwInit->hTwIf, pTxn);

                break;
            }
            else
            {
                pHwInit->uFinStage = 3;
                pHwInit->uFinLoop ++;
            }
            continue;

        case 5:  
            pHwInit->uFinStage++;

            if (pHwInit->uFinLoop >= FIN_LOOP)
            {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for the hardware to complete initialization\n");

                pHwInit->DownloadStatus = TXN_STATUS_ERROR;
                EXCEPT (pHwInit, TXN_STATUS_ERROR);
            }
        
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Firmware init complete...\n");

            /* 
             * There are valid addresses of the command and event mailbox 
             * on the scratch pad registers 
             */
            /* Hardware config command mail box */
            status = cmdMbox_ConfigHw (pTWD->hCmdMbox,
                                       (fnotify_t)hwInit_FinalizeDownloadSm, 
                                       hHwInit);
            EXCEPT (pHwInit, status)
            
        case 6:  
            pHwInit->uFinStage++;

            /* Hardware config event mail box */
            status = eventMbox_InitMboxAddr (pTWD->hEventMbox,
                                         (fnotify_t)hwInit_FinalizeDownloadSm, 
                                         hHwInit);
            EXCEPT (pHwInit, status);
            
        case 7: 
            pHwInit->uFinStage++;
            pHwInit->uTxnIndex = 0;

            /* Set the bus addresses partition to its "running" mode */
            hwInit_SetPartition (pHwInit, 
                                 PARTITION_WORK_MEM_ADDR, 
                                 PARTITION_WORK_MEM_SIZE, 
                                 PARTITION_WORK_REG_ADDR, 
                                 PARTITION_WORK_REG_SIZE);
            /* 
             * In case of full asynchronous mode the firmware event must be ready 
             * to receive event from the command mailbox
             */

            uIntVect = fwEvent_GetInitMask (pTWD->hFwEvent);

                /* Clearing all the interrupt status register sources */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, ACX_REG_INTERRUPT_MASK, (~uIntVect), 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);


            pHwInit->uTxnIndex++;

            BUILD_HW_INIT_FW_STATIC_TXN(pHwInit, pTxn, cmdMbox_GetMboxAddress (pTWD->hCmdMbox),
                                        (TTxnDoneCb)hwInit_FinalizeDownloadSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status);
            continue;

        case 8:
            
            pHwInit->uFinStage = 0;

            cmdBld_FinalizeDownload (pTWD->hCmdBld, &pHwInit->tBootAttr, &(pHwInit->tFwStaticTxn.tFwStaticInfo));

            /* Set the Download Status to COMPLETE */
            pHwInit->DownloadStatus = TXN_STATUS_COMPLETE;

            return TXN_STATUS_COMPLETE;

        } /* End switch */

    } /* End while */

}


/****************************************************************************
 *                      hwInit_ResetSm()
 ****************************************************************************
 * DESCRIPTION: Reset hardware state machine
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
static TI_STATUS hwInit_ResetSm (TI_HANDLE hHwInit)
{
    THwInit *pHwInit = (THwInit *)hHwInit;
    TI_STATUS status = TI_OK;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

        /* Disable Rx/Tx */
    BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, REG_ENABLE_TX_RX, 0x0, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
    twIf_Transact(pHwInit->hTwIf, pTxn);

    pHwInit->uTxnIndex++;

        /* Disable auto calibration on start */
    BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, SPARE_A2, 0xFFFF, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE,(TTxnDoneCb)hwInit_BootSm, hHwInit)
    status = twIf_Transact(pHwInit->hTwIf, pTxn);

    return status;
}


/****************************************************************************
 *                      hwInit_EepromlessStartBurstSm()
 ****************************************************************************
 * DESCRIPTION: prepare eepromless configuration before boot
 * 
 * INPUTS:  
 * 
 * OUTPUT:  
 * 
 * RETURNS: 
 ****************************************************************************/
static TI_STATUS hwInit_EepromlessStartBurstSm (TI_HANDLE hHwInit)
{
    THwInit   *pHwInit = (THwInit *)hHwInit;
    TI_STATUS  status = TI_OK;
    TI_UINT8   *uAddr;
    TI_UINT32  uDeltaLength;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

    while (TI_TRUE)
    {
        switch (pHwInit->uEEPROMStage)
        {
        /* 
         * Stages 0, 1 handles the eeprom format parameters: 
         * ------------------------------------------------
         * Length  - 8bit       --> The length is counted in 32bit words
         * Address - 16bit
         * Data    - (Length * 4) bytes
         * 
         * Note: The nvs is in big endian format and we need to change it to little endian
         */
        case 0: 
            /* Check if address LSB = 1 --> Register address */
            if ((pHwInit->uEEPROMRegAddr = pHwInit->pEEPROMCurPtr[1]) & 1)
            {
                /* Mask the register's address LSB before writing to it */
                pHwInit->uEEPROMRegAddr &= 0xfe;
                /* Change the address's endian */
                pHwInit->uEEPROMRegAddr |= (TI_UINT32)pHwInit->pEEPROMCurPtr[2] << 8;
                /* Length of burst data */
                pHwInit->uEEPROMBurstLen = pHwInit->pEEPROMCurPtr[0];
                pHwInit->pEEPROMCurPtr += 3;
                pHwInit->uEEPROMBurstLoop = 0; 
                /* 
                 * We've finished reading the burst information.
                 * Go to stage 1 in order to write it 
                 */
                pHwInit->uEEPROMStage = 1;
            }
            /* If address LSB = 0 --> We're not in the burst section */
            else
            {
                /* End of Burst transaction: we should see 7 zeroed bytes */
                if (pHwInit->pEEPROMCurPtr[0] == 0)
                {
                    pHwInit->pEEPROMCurPtr += 7;
                }
                pHwInit->uEEPROMCurLen -= (pHwInit->pEEPROMCurPtr - pHwInit->pEEPROMBuf + 1);
                pHwInit->uEEPROMCurLen = (pHwInit->uEEPROMCurLen + NVS_DATA_BUNDARY_ALIGNMENT - 1) & 0xfffffffc;
                /* End of Burst transaction, go to TLV section */
                pHwInit->uEEPROMStage = 2;
            }
            continue;            

        case 1: 
            if (pHwInit->uEEPROMBurstLoop < pHwInit->uEEPROMBurstLen)
            {
                /* Change the data's endian */
                TI_UINT32 val = (pHwInit->pEEPROMCurPtr[0] | 
                                (pHwInit->pEEPROMCurPtr[1] << 8) | 
                                (pHwInit->pEEPROMCurPtr[2] << 16) | 
                                (pHwInit->pEEPROMCurPtr[3] << 24));

                TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "NVS::BurstRead: *(%08x) = %x\n", pHwInit->uEEPROMRegAddr, val);

                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, (REGISTERS_BASE+pHwInit->uEEPROMRegAddr), val, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_EepromlessStartBurstSm, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);
 
                pHwInit->uEEPROMStatus = status;
                pHwInit->uEEPROMRegAddr += WORD_SIZE;
                pHwInit->pEEPROMCurPtr +=  WORD_SIZE;
                /* While not end of burst, we stay in stage 1 */
                pHwInit->uEEPROMStage = 1;
                pHwInit->uEEPROMBurstLoop ++;

                EXCEPT (pHwInit, status);
            }
            else
            {
                /* If end of burst return to stage 0 to read the next one */
                pHwInit->uEEPROMStage = 0;
            }
             
            continue;

        case 2:


            pHwInit->uEEPROMStage = 3;
    
            /* Set the bus addresses partition to its "running" mode */
            hwInit_SetPartition (pHwInit, 
                                 PARTITION_WORK_MEM_ADDR, 
                                 PARTITION_WORK_MEM_SIZE, 
                                 PARTITION_WORK_REG_ADDR, 
                                 PARTITION_WORK_REG_SIZE);
            continue;
 
        case 3:
            TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Reached TLV section\n");

            /* Align the host address */
            if (((TI_UINT32)pHwInit->pEEPROMCurPtr & WORD_ALIGNMENT_MASK) && (pHwInit->uEEPROMCurLen > 0) )
            {
                uAddr = (TI_UINT8*)(((TI_UINT32)pHwInit->pEEPROMCurPtr & 0xFFFFFFFC)+WORD_SIZE);
                uDeltaLength = uAddr - pHwInit->pEEPROMCurPtr + 1;

                pHwInit->pEEPROMCurPtr = uAddr;
                pHwInit->uEEPROMCurLen-= uDeltaLength;
            }

            TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "NVS::WriteTLV: pEEPROMCurPtr= %x, Length=%d\n", pHwInit->pEEPROMCurPtr, pHwInit->uEEPROMCurLen);

            if (pHwInit->uEEPROMCurLen)
            {
                /* Save the 4 bytes before the NVS data for WSPI case where they are overrun by the WSPI BusDrv */
                pHwInit->uSavedDataForWspiHdr = *(TI_UINT32 *)(pHwInit->pEEPROMCurPtr - WSPI_PAD_LEN_WRITE);

                /* Prepare the Txn structure for the NVS transaction to the CMD_MBOX */
                HW_INIT_PTXN_SET(pHwInit, pTxn)
                TXN_PARAM_SET_DIRECTION(pTxn, TXN_DIRECTION_WRITE);
                BUILD_TTxnStruct(pTxn, CMD_MBOX_ADDRESS, pHwInit->pEEPROMCurPtr, pHwInit->uEEPROMCurLen, 
                                 (TTxnDoneCb)hwInit_EepromlessStartBurstSm, hHwInit)

                /* Transact the NVS data to the CMD_MBOX */
                status = twIf_Transact(pHwInit->hTwIf, pTxn);
                
                pHwInit->uEEPROMCurLen = 0;
                pHwInit->uNVSStatus = status;

                EXCEPT (pHwInit, status); 
            }
            else
            {
                /* Restore the 4 bytes before the NVS data for WSPI case were they are overrun by the WSPI BusDrv */
                *(TI_UINT32 *)(pHwInit->pEEPROMCurPtr - WSPI_PAD_LEN_WRITE) = pHwInit->uSavedDataForWspiHdr;

                /* Call the upper level state machine */
                if (pHwInit->uEEPROMStatus == TXN_STATUS_PENDING || 
                    pHwInit->uNVSStatus == TXN_STATUS_PENDING)
                {
                    hwInit_BootSm (hHwInit);
                }

                return TXN_STATUS_COMPLETE;
            }
        } /* End switch */
 
    } /* End while */
}

/****************************************************************************
 *                      hwInit_LoadFwImageSm()
 ****************************************************************************
 * DESCRIPTION: Load image from the host and download into the hardware 
 * 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/


#define ADDRESS_SIZE		(sizeof(TI_INT32))

static TI_STATUS hwInit_LoadFwImageSm (TI_HANDLE hHwInit)
{
    THwInit *pHwInit 			= (THwInit *)hHwInit;
    TI_STATUS status 			= TI_OK;
	ETxnStatus	TxnStatus;
	TI_UINT32 uMaxPartitionSize	= PARTITION_DOWN_MEM_SIZE;
    TTxnStruct* pTxn;

    pHwInit->uTxnIndex = 0;

    while (TI_TRUE)
    {
        switch (pHwInit->uLoadStage)
        {
		case 0:
            pHwInit->uLoadStage = 1; 

			/* Check the Downloaded FW alignment */
			if ((pHwInit->uFwLength % ADDRESS_SIZE) != 0)
			{
				TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Length of downloaded Portion (%d) is not aligned\n",pHwInit->uFwLength);
				EXCEPT_L (pHwInit, TXN_STATUS_ERROR);
			}

			TRACE2(pHwInit->hReport, REPORT_SEVERITY_INIT , "Image addr=0x%x, Len=0x%x\n", pHwInit->pFwBuf, pHwInit->uFwLength);

			/* Set bus memory partition to current download area */
            hwInit_SetPartition (pHwInit, 
                                 pHwInit->uFwAddress, 
                                 PARTITION_DOWN_MEM_SIZE, 
                                 PARTITION_DOWN_REG_ADDR, 
                                 PARTITION_DOWN_REG_SIZE);
            status = TI_OK;
			break;

        case 1:

			pHwInit->uLoadStage = 2;
			/* if initial size is smaller than MAX_SDIO_BLOCK - go strait to stage 4 to write partial block */
			if (pHwInit->uFwLength < MAX_SDIO_BLOCK)
			{
				pHwInit->uLoadStage = 4; 
			}

			pHwInit->uBlockReadNum 		= 0;
			pHwInit->uBlockWriteNum 	= 0;
			pHwInit->uPartitionLimit 	= pHwInit->uFwAddress + uMaxPartitionSize;

            continue;
                    
        case 2:

            /* Load firmware by blocks */
 			if (pHwInit->uBlockReadNum < (pHwInit->uFwLength / MAX_SDIO_BLOCK))
            {            
                pHwInit->uLoadStage = 3;

                /* Change partition */
				/* The +2 is for the last block and the block remainder */  
				if ( ((pHwInit->uBlockWriteNum + 2) * MAX_SDIO_BLOCK + pHwInit->uFwAddress) > pHwInit->uPartitionLimit)
                {                					
					pHwInit->uFwAddress += pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK;
					/* update uPartitionLimit */
					pHwInit->uPartitionLimit = pHwInit->uFwAddress + uMaxPartitionSize;
                    /* Set bus memory partition to current download area */
                    hwInit_SetPartition (pHwInit, 
                                         pHwInit->uFwAddress, 
                                         PARTITION_DOWN_MEM_SIZE, 
                                         PARTITION_DOWN_REG_ADDR, 
                                         PARTITION_DOWN_REG_SIZE);
                    TxnStatus = TXN_STATUS_OK;
					pHwInit->uBlockWriteNum = 0;
                    TRACE1(pHwInit->hReport, REPORT_SEVERITY_INIT , "Change partition to address offset = 0x%x\n", 									   pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK);
                    EXCEPT_L (pHwInit, TxnStatus);                                                     
                }
            }
            else
            {
                pHwInit->uLoadStage = 4;
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INIT , "Load firmware with Portions\n");
            }
            continue;

        case 3:        
            pHwInit->uLoadStage = 2;

            pHwInit->uTxnIndex = 0;

            /* Copy image block to temporary buffer */
            os_memoryCopy (pHwInit->hOs,
                           (void *)&pHwInit->auFwTmpBuf[WSPI_PAD_LEN_WRITE],
						   (void *)(pHwInit->pFwBuf + pHwInit->uBlockReadNum * MAX_SDIO_BLOCK),
						   MAX_SDIO_BLOCK);

            /* Load the block. Save WSPI_PAD_LEN_WRITE space for WSPI bus command */
             BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, (pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK),
                                     (pHwInit->auFwTmpBuf + WSPI_PAD_LEN_WRITE), MAX_SDIO_BLOCK, TXN_DIRECTION_WRITE,
                                     (TTxnDoneCb)hwInit_LoadFwImageSm, hHwInit)
            TxnStatus = twIf_Transact(pHwInit->hTwIf, pTxn);

            /* Log ERROR if the transaction returned ERROR */
            if (TxnStatus == TXN_STATUS_ERROR)
            {
                TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "hwInit_LoadFwImageSm: twIf_Transact retruned status=0x%x\n", TxnStatus);
            } 

			pHwInit->uBlockWriteNum ++;
			pHwInit->uBlockReadNum ++;
            EXCEPT_L (pHwInit, TxnStatus);
            continue;

        case 4:    
			pHwInit->uLoadStage 	= 5;

            pHwInit->uTxnIndex = 0;

			/* If No Last block to write */
			if ( pHwInit->uFwLength % MAX_SDIO_BLOCK == 0 )
			{
				continue;
			}


            /* Copy the last image block */
             os_memoryCopy (pHwInit->hOs,
                           (void *)&pHwInit->auFwTmpBuf[WSPI_PAD_LEN_WRITE],
						   (void *)(pHwInit->pFwBuf + pHwInit->uBlockReadNum * MAX_SDIO_BLOCK),
						   pHwInit->uFwLength % MAX_SDIO_BLOCK);

            /* Load the last block */
             BUILD_HW_INIT_FW_DL_TXN(pHwInit, pTxn, (pHwInit->uFwAddress + pHwInit->uBlockWriteNum * MAX_SDIO_BLOCK),
                                     (pHwInit->auFwTmpBuf + WSPI_PAD_LEN_WRITE), (pHwInit->uFwLength % MAX_SDIO_BLOCK), TXN_DIRECTION_WRITE,
                                     (TTxnDoneCb)hwInit_LoadFwImageSm, hHwInit)
            TxnStatus = twIf_Transact(pHwInit->hTwIf, pTxn);

            if (TxnStatus == TXN_STATUS_ERROR)
			{
                TRACE1(pHwInit->hReport, REPORT_SEVERITY_ERROR , "hwInit_LoadFwImageSm: last block retruned status=0x%x\n", TxnStatus);
			}

            EXCEPT_L (pHwInit, TxnStatus);
            continue;

        case 5:
            pHwInit->uLoadStage = 0;

			/*If end of overall FW Download Process: Finalize download (run firmware)*/
			if ( pHwInit->bFwBufLast == TI_TRUE )
			{			
				/* The download has completed */ 
				WLAN_OS_REPORT (("Finished downloading firmware.\n"));
				status = hwInit_FinalizeDownloadSm (hHwInit);
			}
			/* Have to wait to more FW Portions */
			else
			{
				/* Call the upper layer callback */
				if ( pHwInit->fFinalizeDownload != NULL )
				{
					(pHwInit->fFinalizeDownload) (pHwInit->hFinalizeDownload);
				}

				status = TI_OK;
			}
            return status;

        } /* End switch */

    } /* End while */

} /* hwInit_LoadFwImageSm() */

#define READ_TOP_REG_LOOP  32

/****************************************************************************
 *                      hwInit_ReadRadioParamsSm ()
 ****************************************************************************
 * DESCRIPTION: hwInit_ReadRadioParamsSm 
 * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
TI_STATUS hwInit_ReadRadioParamsSm (TI_HANDLE hHwInit)
{ 
    THwInit      *pHwInit = (THwInit *)hHwInit;
    TTwd         *pTWD = (TTwd *)pHwInit->hTWD;
   IniFileGeneralParam *pGenParams = &DB_GEN(pTWD->hCmdBld);
    TI_UINT32  val= 0, value;
    TI_UINT32  add = FUNC7_SEL;
	TI_UINT32  retAddress;
    TTxnStruct  *pTxn;
    TI_STATUS   status = 0;
    
             
    while (TI_TRUE)
    {
       switch (pHwInit->uRegStage)
        {
        case 0:
            pHwInit->uRegStage = 1;
            pHwInit->uTxnIndex++;

            /*
             * Select GPIO over Debug for BT_FUNC7 clear bit 17
             */
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, GPIO_SELECT, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_ReadRadioParamsSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

        case 1:
            pHwInit->uRegStage ++;
            pHwInit->uRegLoop = 0;

            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            val = (pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData);                
            val &= 0xFFFDFFFF; /*clear bit 17*/
            /* Now we can zero the index */
            pHwInit->uTxnIndex = 0;

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, GPIO_SELECT, val, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)

            twIf_Transact(pHwInit->hTwIf, pTxn);

            pHwInit->uTxnIndex++; 

            pHwInit->uRegData = FUNC7_SEL;

            continue;

        case 2:

            pHwInit->uRegStage ++;
            add = pHwInit->uRegData;

     
            /* Select GPIO over Debug for BT_FUNC7*/
            retAddress = (TI_UINT32)(add / 2);
	        val = (retAddress & 0x7FF);
        	val |= BIT_16 | BIT_17;

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, val, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            pHwInit->uTxnIndex++;  

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x2, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            continue;

        case 3:

            pHwInit->uRegStage ++;
            pHwInit->uTxnIndex++; 

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_DATA_RD, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_ReadRadioParamsSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

           
        case 4:

            val = (pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData);
            
            pHwInit->uTxnIndex = 0;
            if (val & BIT_18)
            {
              if ((val & BIT_16) && (!(val & BIT_17)))
              {
                  pHwInit->uRegStage ++;
                  pHwInit->uRegLoop = 0;

              }
              else 
              {
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "can't writing bt_func7_sel\n");
                               
                TWD_FinalizeFEMRead(pHwInit->hTWD);

                return TI_NOK;
              }
            }
            else
            {
              if (pHwInit->uRegLoop < READ_TOP_REG_LOOP)
              {
                 pHwInit->uRegStage = 3;
                 pHwInit->uRegLoop++;
              }
              else 
              {

                TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for writing bt_func7_sel\n");
               
                TWD_FinalizeFEMRead(pHwInit->hTWD);

                return TI_NOK;

              }
            }

            continue;

        case 5:
               pHwInit->uRegStage ++;
               add = pHwInit->uRegData;
               retAddress = (TI_UINT32)(add / 2);
	           value = (retAddress & 0x7FF);
               value |= BIT_16 | BIT_17;

               BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, value, 
                                  REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
               twIf_Transact(pHwInit->hTwIf, pTxn);

               pHwInit->uTxnIndex++;  

              if (pHwInit->uRegSeqStage == 0)
              {
                  if (pHwInit->uRegData == FUNC7_SEL)
                    value = (val | 0x600);
                  else
                    value = (val | 0x1000);
              }
              else
              {
                  if (pHwInit->uRegData == FUNC7_SEL)
                    value = (val & 0xF8FF);
                  else
                    value = (val & 0xCFFF);

              }

	      value &= 0xFFFF;
          
               BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_WDATA, value, 
                                  REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
               twIf_Transact(pHwInit->hTwIf, pTxn);

               pHwInit->uTxnIndex++; 

               BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x1, 
                                  REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_ReadRadioParamsSm, hHwInit)

               /*BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, INDIRECT_REG5, 0x1, 
                                  REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL) */

               status = twIf_Transact(pHwInit->hTwIf, pTxn);

               pHwInit->uTxnIndex++;                

               if ((pHwInit->uRegData == FUNC7_SEL)&& (pHwInit->uRegSeqStage == 0))
               {
                 pHwInit->uRegData = FUNC7_PULL;
                 pHwInit->uRegStage = 2;
               }
               else
               {
                  if ((pHwInit->uRegData == FUNC7_PULL)&& (pHwInit->uRegSeqStage == 1))
                   {
                     pHwInit->uRegData = FUNC7_SEL;
                     pHwInit->uRegStage = 2;
                   }
               }

               EXCEPT (pHwInit, status)                 
               continue;

        case 6:

              if (pHwInit->uRegSeqStage == 1)
              {
                  pHwInit->uRegStage = 8;
              }
              else
              {
                pHwInit->uRegStage ++;
                pHwInit->uTxnIndex++; 

                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, GPIO_OE_RADIO, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_ReadRadioParamsSm, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);
                EXCEPT (pHwInit, status)
              }
              continue;

        case 7:
            pHwInit->uRegStage ++;

            /* We don't zero pHwInit->uTxnIndex at the begining because we need it's value to the next transaction */
            val = (pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData);                
            val |= 0x00020000;

            pHwInit->uTxnIndex = 0; 
            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, GPIO_OE_RADIO, val, 
                               REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
            twIf_Transact(pHwInit->hTwIf, pTxn);

            pHwInit->uTxnIndex++;  

            BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, GPIO_IN, 0, 
                               REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_ReadRadioParamsSm, hHwInit)
            status = twIf_Transact(pHwInit->hTwIf, pTxn);

            EXCEPT (pHwInit, status)

            
        case 8:
            if (pHwInit->uRegSeqStage == 0)
             {
	       val = (pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData);                  
	       val &= 0x20000;
	       if(val)
	      {
		   pGenParams->TXBiPFEMManufacturer = FEM_TRIQUINT_TYPE_E;
	      }
	      else
	      {
	  	   pGenParams->TXBiPFEMManufacturer = FEM_RFMD_TYPE_E;
	      }
               WLAN_OS_REPORT (("FEM Type %d \n",pGenParams->TXBiPFEMManufacturer));
			   pHwInit->uTxnIndex = 0;
               pHwInit->uRegSeqStage = 1;
               pHwInit->uRegStage = 2;
               pHwInit->uRegData = FUNC7_PULL;
               continue;
             }
             else
             {
              TRACE0(pHwInit->hReport, REPORT_SEVERITY_INFORMATION, "hwInit_ReadRadioParamsSm Ended Successfully\n");
              
              TWD_FinalizeFEMRead(pHwInit->hTWD);

              return TI_OK;

             }

        } /* End switch */

    } /* End while */

}


/****************************************************************************
 *                      hwInit_ReadRadioParams()
 ****************************************************************************
 * DESCRIPTION: hwInit_ReadRadioParamsSm 
 * initalizie hwInit_ReadRadioParamsSm parmaeters
  ****************************************************************************/
   
TI_STATUS hwInit_ReadRadioParams (TI_HANDLE hHwInit)
{
  THwInit      *pHwInit = (THwInit *)hHwInit;

  pHwInit->uRegStage = 0;
  pHwInit->uRegSeqStage = 0;
 
  return hwInit_ReadRadioParamsSm (hHwInit);
}

/****************************************************************************
 *                      hwInit_InitPoalrity()
 ****************************************************************************
 * DESCRIPTION: hwInit_ReadRadioParamsSm 
 * initalizie hwInit_ReadRadioParamsSm parmaeters
  ****************************************************************************/
   
TI_STATUS hwInit_InitPolarity(TI_HANDLE hHwInit)
{
  THwInit      *pHwInit = (THwInit *)hHwInit;

  pHwInit->uRegStage = 0;
  pHwInit->uRegSeqStage = 0;
 
  return hwInit_WriteIRQPolarity (hHwInit);
}



/****************************************************************************
 *                      hwInit_WriteIRQPolarity ()
 ****************************************************************************
 * DESCRIPTION: hwInit_WriteIRQPolarity
  * INPUTS:  None    
 * 
 * OUTPUT:  None
 * 
 * RETURNS: TI_OK or TI_NOK
 ****************************************************************************/
 TI_STATUS hwInit_WriteIRQPolarity(TI_HANDLE hHwInit)
 {
     THwInit     *pHwInit = (THwInit *)hHwInit;
     TI_UINT32   Address,value;
     TI_UINT32   val=0;
     TTxnStruct  *pTxn;
     TI_STATUS   status = 0;

   /*  To write to a top level address from the WLAN IP:
       Write the top level address to the OCP_POR_CTR register. 
       Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600
       Write the data to the OCP_POR_WDATA register
       Write 0x1 to the OCP_CMD register. 

      To read from a top level address:
      Write the top level address to the OCP_POR_CTR register.
      Divide the top address by 2, and add 0x30000 to the result � for example for top address 0xC00, write to the OCP_POR_CTR 0x30600 
      Write 0x2 to the OCP_CMD register. 
      Poll bit [18] of OCP_DATA_RD for data valid indication
      Check bits 17:16 of OCP_DATA_RD:
      00 � no response
      01 � data valid / accept
      10 � request failed
      11 � response error
      Read the data from the OCP_DATA_RD register
   */
      
     while (TI_TRUE)
     {
         switch (pHwInit->uRegStage)
         {
         case 0:

             pHwInit->uRegStage = 1;
             pHwInit->uTxnIndex++;
             pHwInit->uRegLoop = 0;

             /* first read the IRQ Polarity register*/
             Address = (TI_UINT32)(FN0_CCCR_REG_32 / 2);
             val = (Address & 0x7FF);
             val |= BIT_16 | BIT_17;

             /* Write IRQ Polarity address register to OCP_POR_CTR*/
             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, val, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)

             twIf_Transact(pHwInit->hTwIf, pTxn);

             pHwInit->uTxnIndex++;  

             /* Write read (2)command to the OCP_CMD register. */

             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x2, 
                                REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
             twIf_Transact(pHwInit->hTwIf, pTxn);

             continue;

         case 1:
             
             pHwInit->uRegStage ++;
             pHwInit->uTxnIndex++; 

             BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_DATA_RD, 0, 
                                REGISTER_SIZE, TXN_DIRECTION_READ, (TTxnDoneCb)hwInit_WriteIRQPolarity, hHwInit)
             status = twIf_Transact(pHwInit->hTwIf, pTxn);

             EXCEPT (pHwInit, status)


         case 2:
             /* get the value from  IRQ Polarity register*/
             val = pHwInit->aHwInitTxn[pHwInit->uTxnIndex].uData;

             pHwInit->uTxnIndex = 0;

             /*Poll bit 18 of OCP_DATA_RD for data valid indication*/
             if (val & BIT_18)
             {
               if ((val & BIT_16) && (!(val & BIT_17)))
               {
                   pHwInit->uRegStage ++;
                   pHwInit->uRegLoop = 0;

               }
               else 
               {
                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "can't writing bt_func7_sel\n");
                 TWD_FinalizePolarityRead(pHwInit->hTWD);

                return TI_NOK;
               }
             }
             else
             {
               if (pHwInit->uRegLoop < READ_TOP_REG_LOOP)
               {
                  pHwInit->uRegStage = 1;
                  pHwInit->uRegLoop++;
               }
               else 
               {

                 TRACE0(pHwInit->hReport, REPORT_SEVERITY_ERROR , "Timeout waiting for writing bt_func7_sel\n");
                 TWD_FinalizePolarityRead(pHwInit->hTWD);

                return TI_NOK;

               }
             }

             continue;


         case 3:
               /* second, write new value of IRQ polarity due to complation flag 1 - active low, 0 - active high*/
                pHwInit->uRegStage ++;
                Address = (TI_UINT32)(FN0_CCCR_REG_32 / 2);
                value = (Address & 0x7FF);
                value |= BIT_16 | BIT_17;

                /* Write IRQ Polarity address register to OCP_POR_CTR*/
               
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_CTR, value, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)

                twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++;  

#ifdef USE_IRQ_ACTIVE_HIGH
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INFORMATION , "Hwinit IRQ polarity active high\n");
                val |= 0x0<<1;
                    
#else
                TRACE0(pHwInit->hReport, REPORT_SEVERITY_INFORMATION , "Hwinit IRQ polarity active low\n");
                val |= 0x01<<1;
#endif

              /* Write the new IRQ polarity value to the OCP_POR_WDATA register */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_POR_WDATA, val, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, NULL, NULL)
                twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++; 

               /* Write write (1)command to the OCP_CMD register. */
                BUILD_HW_INIT_TXN_DATA(pHwInit, pTxn, OCP_CMD, 0x1, 
                                   REGISTER_SIZE, TXN_DIRECTION_WRITE, (TTxnDoneCb)hwInit_WriteIRQPolarity, hHwInit)
                status = twIf_Transact(pHwInit->hTwIf, pTxn);

                pHwInit->uTxnIndex++; 

                EXCEPT (pHwInit, status)              
                continue;

         case 4:

               TWD_FinalizePolarityRead(pHwInit->hTWD);

              return TI_OK;

          
         } /* End switch */

     } /* End while */

 }


