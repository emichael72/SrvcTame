
/**
 ******************************************************************************
 * @file    srvctame.c
 * @brief   Service Tamer Module for Process Management
 ******************************************************************************
 * 
 * This file implements a tool for managing process priorities, capable of
 * operating either as a Windows service or as a standalone application. The
 * tool aims to normalize processes running at unusual priorities, ensuring
 * more stable and controlled operations.
 *
 * Building / Installing:
 * ----------------------
 * 
 * 1. Build the executable and place it in your desired location.
 * 2. Create 'SrvcTame.ini' using notepad with the format provided below, and
 *    place it in your Windows directory.
 * 3. Open a command prompt with administrative rights, navigate to the directory
 *    containing the executable, and run:
 *       SrvcTame -i
 * 4. Access the Windows Service Management tool and start the service named
 *    "Intel(R) Process Tamer."
 *
 * Example of SrvcTame.ini:
 * ------------------------
 * 
 * [Processes]
 * Process1=it-agent.exe
 * Process2=it-autoupdate-service.exe
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include <windows.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

/** @addtogroup SRVC_TAME
  * @{
  */

/* Private define ------------------------------------------------------------*/
/** @defgroup SrvcTame_Private_define Service Tamer  Private Define
  * @{
  */

#define SRVC_TAME_CMD_MODE     false          /* Indicate command / service mode */
#define SRVC_TAME_SERVICE_NAME "ProcessTamer" /* Our service name */
#define SRVC_TAME_INI_FILE     "SrvcTame.ini" /* Name of the INI file we're using */
#define SRVC_TAME_INTERVAL     10000          /* 10 seconds */

/**
  * @}
  */

/* Private typedef -----------------------------------------------------------*/
/** @defgroup SrvcTame_Private_Typedef Service Tamer Private Typedef
  * @{
  */

/*! @brief  Module internal data */
typedef struct __Tamer_GlobalsTypeDef
{
    SERVICE_STATUS        ServiceStatus;
    SERVICE_STATUS_HANDLE hStatus;
    bool                  serviceMode;
} Tamer_GlobalsTypeDef;

/* Single instance for all globals */
Tamer_GlobalsTypeDef gTamer = {0};

/**
  * @}
  */

/**
 * @brief Uninstalls the service.
 * @param serviceName Name of the service to uninstall.
 * @retval int EXIT_SUCCESS on success, EXIT_FAILURE on failure.
 */

static int Tamer_ServiceUninstall(const char *serviceName)
{
    int       retVal     = EXIT_FAILURE;
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if ( hSCManager && serviceName )
    {
        SC_HANDLE hService = OpenService(hSCManager, serviceName, SERVICE_STOP | DELETE);
        if ( hService )
        {
            SERVICE_STATUS status;
            if ( ControlService(hService, SERVICE_CONTROL_STOP, &status) )
            {
                printf("Stopping %s.\n", serviceName);
                Sleep(1000);

                while ( QueryServiceStatus(hService, &status) )
                {
                    if ( status.dwCurrentState == SERVICE_STOP_PENDING )
                    {
                        printf("Waiting for service to stop...\n");
                        Sleep(1000);
                    }
                    else
                    {
                        break;
                    }
                }
                printf("Service stopped.\n");
            }

            if ( DeleteService(hService) )
            {
                printf("Service %s deleted successfully.\n", serviceName);
                retVal = EXIT_SUCCESS;
            }
            else
            {
                printf("Failed to delete service %s: %lu\n", serviceName, GetLastError());
            }

            CloseServiceHandle(hService);
        }
        else
        {
            printf("OpenService failed: %lu\n", GetLastError());
        }

        CloseServiceHandle(hSCManager);
    }
    else
    {
        printf("OpenSCManager failed: %lu\n", GetLastError());
    }

    return retVal;
}

/**
 * @brief Installs the service.
 * @param procName Name of the executable to run as service.
 * @retval int EXIT_SUCCESS on success, EXIT_FAILURE on failure.
 */

static int Tamer_ServiceInstall(char *procName)
{
    int       retVal     = EXIT_FAILURE;
    char      path[512]  = {0};
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);

    if ( hSCManager )
    {
        GetFullPathName(procName, sizeof(path), path, NULL);

        SC_HANDLE hService = CreateService(hSCManager, "ProcessTamer", "Intel(R) Process Tamer", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                           SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);

        if ( hService )
        {
            SERVICE_DESCRIPTION desc = {"Intel(R) service to manage and reduce the priority of specified "
                                        "IT processes to mitigate their impact on system performance."};
            if ( ! ChangeServiceConfig2(hService, SERVICE_CONFIG_DESCRIPTION, &desc) )
            {
                printf("Failed to set service description: %lu\n", GetLastError());
            }

            printf("Service installed successfully\n");
            retVal = EXIT_SUCCESS;
            CloseServiceHandle(hService);
        }
        else
        {
            printf("Failed to install service: %lu\n", GetLastError());
        }

        CloseServiceHandle(hSCManager);
    }
    else
    {
        printf("Service Manager open failed\n");
    }

    return retVal;
}

/**
 * @brief Sets the priority of a process to idle.
 * @param exeName Name of the executable whose priority to adjust.
 */

static bool Tamer_SetProcessPriority(char *exeName)
{
    bool           retVal    = false;
    HANDLE         hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, 0);
    PROCESSENTRY32 pEntry;
    pEntry.dwSize = sizeof(pEntry);
    BOOL hRes     = Process32First(hSnapShot, &pEntry);

    while ( hRes )
    {
        if ( _stricmp(pEntry.szExeFile, exeName) == 0 )
        {
            printf("Found %-48s", pEntry.szExeFile);
            HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pEntry.th32ProcessID);

            if ( hProcess != NULL )
            {
                if ( GetPriorityClass(hProcess) != IDLE_PRIORITY_CLASS )
                {
                    SetPriorityClass(hProcess, IDLE_PRIORITY_CLASS);
                    retVal = true;
                    printf("OK\n");
                }
                else
                    printf("Set\n");

                CloseHandle(hProcess);
            }
            else
            {
                printf("Error\n");
            }
        }

        hRes = Process32Next(hSnapShot, &pEntry);
    }

    CloseHandle(hSnapShot);
    return retVal;
}

/**
 * @brief Controls the service based on the request code.
 * @param request Control code for the service.
 */
static void Tamer_ServiceControl(DWORD request)
{
    /* Intended only when running as a service */
    if ( gTamer.serviceMode == false )
        return;

    switch ( request )
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            gTamer.ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(gTamer.hStatus, &gTamer.ServiceStatus);
            break;
        default:
            SetServiceStatus(gTamer.hStatus, &gTamer.ServiceStatus);
            break;
    }
}

/**
 * @brief Initializes the service settings.
 * @retval bool true if initialization succeeds, false otherwise.
 */

static bool Tamer_ServiceInit(void)
{
    /* Intended only when running as a service */
    if ( gTamer.serviceMode == false )
        return false;

    /* Placeholder */
    return true;
}

/**
 * @brief Main processing loop for the service.
 * @retval bool true to continue processing, false to indicate a stop condition.
 */

static bool Tamer_ServiceProcess(void)
{
    char iniFile[MAX_PATH] = {0};
    char iniPath[MAX_PATH] = {0};

    if ( gTamer.serviceMode == true )
    {
        /* When running as a service we expect the .ini to be in the  \Windsows directory. */
        if ( GetWindowsDirectory(iniPath, sizeof(iniPath)) == 0 )
        {
            printf("Failed to get system directory\n");
            return false;
        }
    }
    else
    {
        /* When not running as a service we expect the .ini to be in the local path. */
        if ( GetCurrentDirectory(sizeof(iniPath), iniPath) == 0 )
        {
            printf("Failed to get local directory\n");
            return false;
        }
    }

    snprintf(iniFile, sizeof(iniFile), "%s\\%s", iniPath, SRVC_TAME_INI_FILE);

    char  processName[256] = {0};
    char  exeName[256]     = {0};
    DWORD bufferSize       = sizeof(exeName);
    int   processIndex     = 1;

    while ( 1 )
    {
        processName[0] = 0;
        snprintf(processName, sizeof(processName), "Process%d", processIndex);
        if ( GetPrivateProfileString("Processes", processName, "", exeName, bufferSize, iniFile) == 0 )
            break;

        Tamer_SetProcessPriority(exeName);
        processIndex++;
    }

    printf("\n");
    return true;
}

/**
 * @brief The main function for the service.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @retval bool true on successful execution, false otherwise.
 */

bool Tamer_ServiceMain(int argc, char **argv)
{
    /* Intended only when running as a service */
    if ( gTamer.serviceMode == false )
        return false;

    /* Note: assuming ServiceStatus was initialized to 0 at startup */
    gTamer.ServiceStatus.dwServiceType      = SERVICE_WIN32;
    gTamer.ServiceStatus.dwCurrentState     = SERVICE_START_PENDING;
    gTamer.ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    gTamer.hStatus = RegisterServiceCtrlHandler(SRVC_TAME_SERVICE_NAME, (LPHANDLER_FUNCTION) Tamer_ServiceControl);

    if ( gTamer.hStatus == (SERVICE_STATUS_HANDLE) 0 )
    {
        return false;
    }

    if ( Tamer_ServiceInit() == false )
    {
        gTamer.ServiceStatus.dwCurrentState  = SERVICE_STOPPED;
        gTamer.ServiceStatus.dwWin32ExitCode = -1;
        SetServiceStatus(gTamer.hStatus, &gTamer.ServiceStatus);
        return false;
    }

    gTamer.ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(gTamer.hStatus, &gTamer.ServiceStatus);

    while ( gTamer.ServiceStatus.dwCurrentState == SERVICE_RUNNING )
    {
        Tamer_ServiceProcess();
        Sleep(SRVC_TAME_INTERVAL);
    }

    return true;
}

/**
 * @brief Entry point for the application.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @retval int EXIT_SUCCESS on success, EXIT_FAILURE otherwise.
 */

int main(int argc, char *argv[])
{
    int retVal = EXIT_FAILURE;

    gTamer.serviceMode = ! SRVC_TAME_CMD_MODE;

    if ( argc == 2 )
    {
        if ( _stricmp(argv[1], "-i") == 0 )
            return Tamer_ServiceInstall(argv[0]);
        else if ( _stricmp(argv[1], "-u") == 0 )
            return Tamer_ServiceUninstall(SRVC_TAME_SERVICE_NAME);
        else
        {
            printf("Unknown command line option provided.\n");
            return EXIT_FAILURE;
        }
    }

    if ( gTamer.serviceMode )
    {
        SERVICE_TABLE_ENTRY ServiceTable[2] = {{0}};

        ServiceTable[0].lpServiceName = SRVC_TAME_SERVICE_NAME;
        ServiceTable[0].lpServiceProc = (LPSERVICE_MAIN_FUNCTION) Tamer_ServiceMain;
        ServiceTable[1].lpServiceName = NULL;
        ServiceTable[1].lpServiceProc = NULL;

        StartServiceCtrlDispatcher(ServiceTable);
    }
    else
    {
        /* Running as a stand alone console process */
        while ( 1 )
        {
            Tamer_ServiceProcess();
            Sleep(SRVC_TAME_INTERVAL);
        }
    }

    return EXIT_SUCCESS;
}

/**
  * @}
  */
