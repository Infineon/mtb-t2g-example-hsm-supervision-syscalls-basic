/**********************************************************************************************************************
 * \file main.c
 * \copyright Copyright (C) Infineon Technologies AG 2019
 *
 * Use of this file is subject to the terms of use agreed between (i) you or the company in which ordinary course of
 * business you are acting and (ii) Infineon Technologies AG or its licensees. If and as long as no such terms of use
 * are agreed, use of this file is subject to following:
 *
 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or organization obtaining a copy of the software and
 * accompanying documentation covered by this license (the "Software") to use, reproduce, display, distribute, execute,
 * and transmit the Software, and to prepare derivative works of the Software, and to permit third-parties to whom the
 * Software is furnished to do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including the above license grant, this restriction
 * and the following disclaimer, must be included in all copies of the Software, in whole or in part, and all
 * derivative works of the Software, unless such copies or derivative works are solely in the form of
 * machine-executable object code generated by a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *********************************************************************************************************************/
/*********************************************************************************************************************/
/*-----------------------------------------------------Includes------------------------------------------------------*/
/*********************************************************************************************************************/
#include "cy_pdl.h"
#include "cybsp.h"
#include "cy_retarget_io.h"

/*********************************************************************************************************************/
/*------------------------------------------------------Macros-------------------------------------------------------*/
/*********************************************************************************************************************/
/* For system calls */
/* Opcode and parameters for EraseSector system call in Blocking mode */
#define ERASE_SECTOR_BLOCKING_COMMAND   0x14000100
/* Opcode and parameters for GenerateHash system call to return Factory Hash */
#define GEN_HASH_FACTORY_HASH_COMMAND   0x1E000100
/* Address of first large sector of work-flash */
#define WORK_FLASH_FIRST_LARGE_SECTOR   0x14000000
/* CM7 triggers the system call, so use IPC1 */
#define USED_IPC_CHANNEL                1
/* IPC Interrupt Structure 0 is used for system calls. This interrupt is handled by notified core (CM0+) */
#define IPC_NOTIFY_INT_NUMBER           0
/* Error code set by HSM (CM0+) when supervision is not approved */
#define HSM_SUPERVISION_ERROR           0x11223344
/* System call success/failure return status bit position in scratch SRAM  */
#define SYS_CALL_STATUS_SHIFT           28
/* System call success status */
#define SYS_CALL_SUCCESS                0xA

/* For user button */
#define USER_BTN_PORT_ADDR              Cy_GPIO_PortToAddr(CYHAL_GET_PORT(CYBSP_USER_BTN))
#define USER_BTN_PIN                    CYHAL_GET_PIN(CYBSP_USER_BTN)
#define USER_BTN_IRQ                    (IRQn_Type)(ioss_interrupts_gpio_dpslp_0_IRQn + CYHAL_GET_PORT(CYBSP_USER_BTN))
#define USER_BTN_IRQ_PRIORITY           7U

/* Shift value for CPU IRQ number ('intSrc' of cy_stc_sysint_t consists of CPU IRQ number and system IRQ number) */
#define CPU_IRQ_NUMBER_SHIFT            16
#define CPU_IRQ_NUM                     (IRQn_Type)NvicMux3_IRQn

/*********************************************************************************************************************/
/*--------------------------------------------Private Variables/Constants--------------------------------------------*/
/*********************************************************************************************************************/
/* Variable to indicate reception of a system call request */
static bool syscallRequest = false;

/* Variable to switch between the two system calls upon each button press */
static uint8_t syscallFlag = 0;

/* Variables for system calls management */
static uint32_t syscallScratchBuffer[8];
static cy_en_ipcdrv_status_t syscallIpcStatus;
static bool syscallLockStatus = true;

/* Configuration structure for user button interrupt */
const cy_stc_sysint_t IRQ_BUTTON_CONFIG =
{
    .intrSrc = ((CPU_IRQ_NUM << CPU_IRQ_NUMBER_SHIFT) | USER_BTN_IRQ),
    .intrPriority = USER_BTN_IRQ_PRIORITY,
};


/*********************************************************************************************************************/
/*------------------------------------------------Function Prototypes------------------------------------------------*/
/*********************************************************************************************************************/
static void handleGPIOInterrupt(void);

/*********************************************************************************************************************/
/*---------------------------------------------Function Implementations----------------------------------------------*/
/*********************************************************************************************************************/
/**********************************************************************************************************************
 * Function Name: handleGPIOInterrupt
 * Summary:
 *  Handles the GPIO interrupt when the user button is pressed.
 * Parameters:
 *  none
 * Return:
 *  none
 **********************************************************************************************************************
 */
static void handleGPIOInterrupt(void)
{
    /* Clear the interrupt */
    Cy_GPIO_ClearInterrupt(USER_BTN_PORT_ADDR, USER_BTN_PIN);

    syscallRequest = true;
    syscallFlag++;
}

/**********************************************************************************************************************
 * Function Name: main
 * Summary:
 *  This is the main function.
 * Parameters:
 *  none
 * Return:
 *  int
 **********************************************************************************************************************
*/
int main(void)
{
    cy_rslt_t result;

    /* Initialize the device and board peripherals */
    result = cybsp_init() ;
    if (result != CY_RSLT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Disable CM7 Data Cache for easy system call handling */
    SCB_DisableDCache();

    /* ------------------------------------------------------------------------------------------
     *                               GPIO and Interrupt Initialization
     * -----------------------------------------------------------------------------------------*/

    /* Initialize user button 1 */
    Cy_GPIO_Pin_FastInit(USER_BTN_PORT_ADDR, USER_BTN_PIN, CY_GPIO_DM_PULLUP, 1U, HSIOM_SEL_GPIO);

    /* Set the interrupt edge */
    Cy_GPIO_SetInterruptEdge(USER_BTN_PORT_ADDR, USER_BTN_PIN, CY_GPIO_INTR_FALLING);

    /* Set the interrupt mask */
    Cy_GPIO_SetInterruptMask(USER_BTN_PORT_ADDR, USER_BTN_PIN, 1);

    /*Initialize the  interrupt with its interrupt handler */
    if (Cy_SysInt_Init(&IRQ_BUTTON_CONFIG, &handleGPIOInterrupt) != CY_SYSINT_SUCCESS)
    {
        CY_ASSERT(0);
    }

    /* Enable CPU interrupt. */
    NVIC_EnableIRQ(CPU_IRQ_NUM);

    /* Initialize retarget-io to use the debug UART port */
    result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX,
             CY_RETARGET_IO_BAUDRATE);

    /* retarget-io init failed. Stop program execution */
    if (result != CY_RSLT_SUCCESS)
    {
       CY_ASSERT(0);
    }

    /* Enable global interrupts */
    __enable_irq();

    printf("\n Press user switch USER1 to invoke system call from Host \r\n");

    for (;;)
    {
        /* Check if user button/switch is pressed. Each switch press event alternately requests one of the 
         * two system calls */
        if (syscallRequest)
        {
            syscallRequest = false;
            if ((syscallFlag % 2) == 1)
            {
                printf("\r\n System Call request to erase a work-flash sector \r\n");

                /* Enable Work-flash erase/write */
                Cy_Flashc_WorkWriteEnable();

                /* Set the scratch buffer with required parameters */
                syscallScratchBuffer[0] = ERASE_SECTOR_BLOCKING_COMMAND;
                syscallScratchBuffer[1] = WORK_FLASH_FIRST_LARGE_SECTOR;

                /* Trigger the system call via IPC channel */
                syscallIpcStatus = Cy_IPC_Drv_SendMsgWord(Cy_IPC_Drv_GetIpcBaseAddress(USED_IPC_CHANNEL), 
                         (1u << IPC_NOTIFY_INT_NUMBER), (uint32_t) &syscallScratchBuffer[0]);

                CY_ASSERT(CY_IPC_DRV_SUCCESS == syscallIpcStatus);
                do
                {
                    /* Wait till the IPC channel is released either by the system call/SROM or by HSM */
                    syscallLockStatus = Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(USED_IPC_CHANNEL));
                } while (syscallLockStatus);

                /* Disable Work-flash erase/write */
                Cy_Flashc_WorkWriteDisable();

                /* Check if the system call execution is successful */
                if((syscallScratchBuffer[0] >> SYS_CALL_STATUS_SHIFT) == SYS_CALL_SUCCESS)
                {
                    printf("\r\nWork-flash erase sector system call successfully executed\r\n");
                }

                /* Check if the system call was rejected by HSM */
                else if((syscallScratchBuffer[0]) == HSM_SUPERVISION_ERROR)
                {
                    printf("\r\nHSM did not approve the system call\r\n");
                }

                /* Check if the system call returned other errors */
                else
                {
                    printf("\r\nWork-flash erase sector system call failed with error code %08lX\r\n", 
                       syscallScratchBuffer[0]);
                }
            }
            else
            {
                printf("\r\nSystem Call request to Generate Hash (Factory Hash)\r\n");

                /* Set the scratch buffer with required parameters */
                syscallScratchBuffer[0] = GEN_HASH_FACTORY_HASH_COMMAND;

                /* Trigger the system call via IPC channel */
                syscallIpcStatus = Cy_IPC_Drv_SendMsgWord(Cy_IPC_Drv_GetIpcBaseAddress(USED_IPC_CHANNEL),
                        (1u << IPC_NOTIFY_INT_NUMBER), (uint32_t)(&syscallScratchBuffer[0]));
                CY_ASSERT(CY_IPC_DRV_SUCCESS == syscallIpcStatus);

                do
                {
                    /* Wait till the IPC channel is released either by the system call/SROM or by HSM */
                    syscallLockStatus = Cy_IPC_Drv_IsLockAcquired(Cy_IPC_Drv_GetIpcBaseAddress(USED_IPC_CHANNEL));
                } while (syscallLockStatus);

                /* Check if the system call execution is successful */
                if ((syscallScratchBuffer[0] >> SYS_CALL_STATUS_SHIFT) == SYS_CALL_SUCCESS)
                {
                    printf("\r\nGenerate Hash system call successfully executed.\r\n");
                    printf("\r\nFactory Hash word 0: %08lX\r\n", *(syscallScratchBuffer + 1));
                    printf("\r\nFactory Hash word 1: %08lX\r\n", *(syscallScratchBuffer + 2));
                    printf("\r\nFactory Hash word 2: %08lX\r\n", *(syscallScratchBuffer + 3));
                    printf("\r\nFactory Hash word 3: %08lX\r\n", *(syscallScratchBuffer + 4));
                    printf("\r\nFactory Hash zeros:  %08lX\r\n", *(syscallScratchBuffer + 5));
                }
                /* Check if the system call was rejected by HSM */
                else if ((syscallScratchBuffer[0]) == HSM_SUPERVISION_ERROR)
                {
                    printf("\r\nHSM did not approve the system call\r\n");
                }
                /* Check if the system call returned other errors */
                else
                {
                    printf("\r\nGenerate Hash system call failed with error code %08lX\r\n", syscallScratchBuffer[0]);
                }
            }
            printf("\r\nPress user switch USER1 to invoke system call from Host\r\n");
        }
    }
}

/* [] END OF FILE */
