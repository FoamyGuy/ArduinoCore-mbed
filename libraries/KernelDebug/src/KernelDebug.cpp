/* Copyright 2020 Adam Green (https://github.com/adamgreen/)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
// The GDB compatible debug monitor for kernel debugging of thread and handler mode code.
// Utilizes MRI (Monitor for Remote Inspection) library for core debugging functionality.
#include <MRI.h>
#include "KernelDebug.h"

// Put armv7-m module into handler mode before including its header and source code.
#define MRI_THREAD_MRI 0

extern "C" {
    #include <core/core.h>
    #include <core/platforms.h>
    #include <core/semihost.h>
    #include <architectures/armv7-m/armv7-m.h>
    // Source code for armv7-m module which is included here, configured for supporting kernel mode debugging.
    #include <architectures/armv7-m/armv7-m.x>
    #include <architectures/armv7-m/debug_cm3.h>
}


// Globals that describe the KernelDebug singleton.
static mbed::UnbufferedSerial*  g_pSerial;
static uint32_t                 g_baudRate;
static IRQn_Type                g_irq;
static bool                     g_breakInSetup;


// Run the DebugMonitor and UART interrupts at this priority.
#define DEBUG_ISR_PRIORITY 2


struct SystemHandlerPriorities {
    uint8_t svcallPriority;
    uint8_t pendsvPriority;
    uint8_t systickPriority;
};


// Forward Function Declarations
static void setupStopInSetup();
static int justEnteredSetupCallback(void* pv);
static void initSerial();
static SystemHandlerPriorities getSystemHandlerPrioritiesBeforeMriModifiesThem();
static void restoreSystemHandlerPriorities(const SystemHandlerPriorities* pPriorities);
static void switchFaultHandlersToDebugger();


// Forward declaration of external functions used by KernelDebug.
// Will be setting initial breakpoint on setup() routine.
extern "C" void setup();
// The debugger uses this handler to catch faults, debug events, etc.
extern "C" void mriExceptionHandler(void);


arduino::KernelDebug::KernelDebug(PinName txPin, PinName rxPin, IRQn_Type irq, uint32_t baudRate, bool breakInSetup /*=true*/) :
    _serial(txPin, rxPin, baudRate)
{
    // Just return without doing anything if the singleton has already been initialized.
    // This ends up using the first initialized KernelDebug object.
    if (g_pSerial != NULL) {
        return;
    }
    g_baudRate = baudRate;
    g_irq = irq;
    g_breakInSetup = breakInSetup;
    g_pSerial = &_serial;

    mriInit("");

    setupStopInSetup();
}

static void setupStopInSetup()
{
    mriCore_SetTempBreakpoint((uint32_t)setup, justEnteredSetupCallback, NULL);
}

static int justEnteredSetupCallback(void* pv)
{
    initSerial();

    // Return 0 to indicate that we want to halt execution at the beginning of setup() or 1 to not force a halt.
    return g_breakInSetup ? 0 : 1;
}

static void initSerial()
{
    // Hook communication port ISR to allow debug monitor to be awakened when GDB sends a command.
    g_pSerial->attach(mriExceptionHandler);
    // Override mbed's configuration to set the desired priority and replace the mbed's ISR with a jump directly
    // into the MRI debugger's mriExceptionHandler routine.
    mriCortexMSetPriority(g_irq, DEBUG_ISR_PRIORITY, 0);
    NVIC_SetVector(g_irq, (uint32_t)mriExceptionHandler);
}


arduino::KernelDebug::~KernelDebug()
{
    // IMPORTANT NOTE: You are attempting to destroy the connection to GDB which isn't allowed.
    //                 Don't allow your KernelDebug object to go out of scope like this.
    debugBreak();
    for (;;) {
        // Loop forever.
    }
}




// ---------------------------------------------------------------------------------------------------------------------
// Global Platform_* functions needed by MRI to initialize and communicate with MRI.
// These functions will perform most of their work through the KernelDebug singleton.
// ---------------------------------------------------------------------------------------------------------------------
void Platform_Init(Token* pParameterTokens)
{
    SystemHandlerPriorities origPriorities = getSystemHandlerPrioritiesBeforeMriModifiesThem();

    __try
        mriCortexMInit((Token*)pParameterTokens, DEBUG_ISR_PRIORITY, WAKEUP_PIN_IRQn);
    __catch
        __rethrow;

    // USB defaults to a priority of 1, keep that as highest priority interrupt so that it can respond to PC requests
    // while kernel debugging.
    // Set interrupts used by UART serial comms and DebugMonitor at next highest priority, priority level 2.
    // Set all other external interrupts lower than both serial comms and DebugMonitor.
    restoreSystemHandlerPriorities(&origPriorities);

    switchFaultHandlersToDebugger();
}

static SystemHandlerPriorities getSystemHandlerPrioritiesBeforeMriModifiesThem()
{
    SystemHandlerPriorities priorities;

    priorities.svcallPriority = NVIC_GetPriority(SVCall_IRQn);
    priorities.pendsvPriority = NVIC_GetPriority(PendSV_IRQn);
    priorities.systickPriority = NVIC_GetPriority(SysTick_IRQn);
    return priorities;
}

static void restoreSystemHandlerPriorities(const SystemHandlerPriorities* pPriorities)
{
    // Restore the system handlers used by the RTOS to their original values which already use the 2 lowest priority
    // levels on the device.
    NVIC_SetPriority(SVCall_IRQn, pPriorities->svcallPriority);
    NVIC_SetPriority(PendSV_IRQn, pPriorities->pendsvPriority);
    NVIC_SetPriority(SysTick_IRQn, pPriorities->systickPriority);
}

static void switchFaultHandlersToDebugger(void)
{
    NVIC_SetVector(HardFault_IRQn,        (uint32_t)&mriExceptionHandler);
    NVIC_SetVector(MemoryManagement_IRQn, (uint32_t)&mriExceptionHandler);
    NVIC_SetVector(BusFault_IRQn,         (uint32_t)&mriExceptionHandler);
    NVIC_SetVector(UsageFault_IRQn,       (uint32_t)&mriExceptionHandler);
}




uint32_t Platform_CommHasReceiveData(void)
{
    return g_pSerial->readable();
}

uint32_t Platform_CommHasTransmitCompleted(void)
{
    // This function is called by MRI to make sure that last GDB command has been ACKed before it executes a reset
    // request. We will busy wait in here until that has happened and then always return true.
    while (!g_pSerial->writable()) {
        // Wait until transmit data register is empty.
    }

    // Might still have one byte in output shift register so wait 10 bit times to be safe.
    uint32_t microsecondsForOneByte = (10  * 1000000) / g_baudRate;
    delayMicroseconds(microsecondsForOneByte);

    return 1;
}

int Platform_CommReceiveChar(void)
{
    while (!Platform_CommHasReceiveData()) {
    }
    uint8_t byte;
    g_pSerial->read(&byte, 1);
    return byte;
}

void Platform_CommSendChar(int character)
{
    g_pSerial->write(&character, 1);
}




static const char g_memoryMapXml[] = "<?xml version=\"1.0\"?>"
                                     "<!DOCTYPE memory-map PUBLIC \"+//IDN gnu.org//DTD GDB Memory Map V1.0//EN\" \"http://sourceware.org/gdb/gdb-memory-map.dtd\">"
                                     "<memory-map>"
                                     "<memory type=\"ram\" start=\"0x00000000\" length=\"0x10000\"> </memory>"
                                     "<memory type=\"flash\" start=\"0x08000000\" length=\"0x200000\"> <property name=\"blocksize\">0x20000</property></memory>"
                                     "<memory type=\"ram\" start=\"0x10000000\" length=\"0x48000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x1ff00000\" length=\"0x20000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x20000000\" length=\"0x20000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x24000000\" length=\"0x80000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x30000000\" length=\"0x48000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x38000000\" length=\"0x10000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x38800000\" length=\"0x1000\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x58020000\" length=\"0x2c00\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x58024400\" length=\"0xc00\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x58025400\" length=\"0x800\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x58026000\" length=\"0x800\"> </memory>"
                                     "<memory type=\"ram\" start=\"0x58027000\" length=\"0x400\"> </memory>"
                                     "<memory type=\"flash\" start=\"0x90000000\" length=\"0x10000000\"> <property name=\"blocksize\">0x200</property></memory>"
                                     "<memory type=\"ram\" start=\"0xc0000000\" length=\"0x800000\"> </memory>"
                                     "</memory-map>";

extern "C" uint32_t Platform_GetDeviceMemoryMapXmlSize(void)
{
    return sizeof(g_memoryMapXml) - 1;
}

extern "C" const char* Platform_GetDeviceMemoryMapXml(void)
{
    return g_memoryMapXml;
}




extern "C" const uint8_t* Platform_GetUid(void)
{
    return NULL;
}

extern "C" uint32_t Platform_GetUidSize(void)
{
    return 0;
}

extern "C" int Semihost_IsDebuggeeMakingSemihostCall(void)
{
    return 0;
}

int Semihost_HandleSemihostRequest(void)
{
    return 0;
}
