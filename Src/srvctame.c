
/**
 ******************************************************************************
 * 
 * @file    srvctame.c
 * @brief   Service for process management.
 * 
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
 * 1. Compile the executable and place it in the desired location.
 * 2. Create a configuration file named 'SrvcTame.ini' using Notepad. Follow the 
 *    provided format and place the file in the Windows directory. Alternatively, 
 *    for debugging purposes, set SRVC_TAME_RUN_AS_SERVICE to false and place the 
 *    configuration file alongside the executable being debugged.
 * 3. Open a command prompt with administrative rights, navigate to the directory 
 *    where the executable is located, and execute the following command to install 
 *    the service:
 *    'SrvcTame -i' or 'SrvcTame -u' to uninstall. 
 * 4. To start the service, either restart your system or use the 'Windows Service 
 *    Management' tool to start the service named "Process Tamer."
 *
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/

#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include "llist.h"

/** @addtogroup SRVC_TAME
  * @{
  */

/* Private define ------------------------------------------------------------*/
/** @defgroup SrvcTame_Private_define Service Tamer  Private Define
  * @{
  */

#define SRVC_TAME_RUN_AS_SERVICE       true                             /* Command line / service mode */
#define SRVC_TAME_INI_FILE             "SrvcTame.ini"                   /* Name of the INI file name we're using */
#define SRVC_TAME_SERVICE_NAME         "ProcessTamer"                   /* Service static name */
#define SRVC_TAME_SERVICE_DISPLAY_NAME "Process Tamer"                  /* Service default display name */
#define SRVC_TAME_SERVICE_DESCRIPTION  "Windows process taming service" /* Service default description */
#define SRVC_TAME_INTERVAL             10000                            /* 10 seconds */

/**
  * @}
  */

/* Private typedef -----------------------------------------------------------*/
/** @defgroup SrvcTame_Private_Typedef Service Tamer Private Typedef
  * @{
  */

typedef struct __Tamer_ProcList
{
    char                     procName[128];
    int                      priority;
    struct __Tamer_ProcList *next;

} Tamer_Proc;

typedef struct __Tamer_Config
{
    char        serviceDispalyName[256];
    char        serviceDescription[256];
    char        filePath[MAX_PATH];
    uint32_t    interval;
    uint32_t    crc32;
    Tamer_Proc *procList;

} Tamer_Config;

/*! @brief  Module internal data */
typedef struct __Tamer_GlobalsTypeDef
{
    SERVICE_STATUS        ServiceStatus;
    SERVICE_STATUS_HANDLE hStatus;
    Tamer_Config         *config;
    bool                  serviceMode;
} Tamer_GlobalsTypeDef;

/* Single instance for all globals */
Tamer_GlobalsTypeDef gTamer = {0};

/**
 * @brief Calculate the CRC32 checksum for a given array of data.
 * 
 * This function computes the CRC32 checksum using a non-table approach for the given buffer of data.
 * The CRC calculation uses a standard polynomial widely used in numerous systems to ensure data integrity.
 *
 * @param data Pointer to the data buffer whose CRC is to be calculated.
 * @param length Length of the data buffer in bytes.
 * @return The computed CRC32 checksum as a 32-bit unsigned integer.
 */

static uint32_t Tamer_CRC2(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for ( size_t i = 0; i < length; i++ )
    {
        uint8_t byte = data[i];
        crc          = crc ^ byte;
        for ( int j = 7; j >= 0; j-- )
        {
            /* Loop for each bit */
            uint32_t mask = (crc & 1) ? 0xFFFFFFFF : 0x00000000;
            crc           = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

/** 
 * 
 * @brief This function opens a specified file, reads its contents into dynamically allocated memory,
 * calculates the CRC32 checksum, and then cleans up by freeing the allocated memory and closing the file.
 * It uses a single cleanup section to manage all cleanup operations. This ensures that all resources
 * are properly released in case of any error.
 * 
 * @param fileName Pointer to a string containing the name of the file.
 * @return CRC32 checksum of the file contents or 0 on error.
 */

static uint32_t Tamer_GetFileCRC(const char *fileName)
{
    FILE    *file     = NULL;
    uint8_t *buffer   = NULL;
    uint32_t crc32    = 0;
    size_t   fileSize = 0;

    do
    {
        /* Open the file in binary mode */
        file = fopen(fileName, "rb");
        if ( file == NULL )
            break;

        /* Seek to the end of the file to determine the file size */
        fseek(file, 0, SEEK_END);
        fileSize = ftell(file);
        rewind(file);

        /* Allocate memory for the entire file */
        buffer = (uint8_t *) malloc(fileSize);
        if ( buffer == NULL )
            break;

        /* Read the file into the buffer */
        if ( fread(buffer, 1, fileSize, file) != fileSize )
            break;

        /* Calculate CRC32 of the buffer */
        crc32 = Tamer_CRC2(buffer, fileSize);

    } while ( 0 );

    /* Cleanup section */
    if ( buffer != NULL )
        free(buffer);

    if ( file != NULL )
        fclose(file);

    return crc32;
}

/** 
 * @brief reads the .INI file into the session configuration global.
 * After the first read the function will do nothing if the 
 * configuration crc32 is the same as the previous run crc.
 * @return number of items in the process list or 0 on error.
 */

static int Tamer_ReadConfig(void)
{

    int         retVal            = 0;
    char        iniFile[MAX_PATH] = {0};
    uint32_t    crc32;
    Tamer_Proc *el, *tmp;

    do
    {
        if ( gTamer.config == NULL )
        {
            /* First run, allocate the thing */
            gTamer.config = (Tamer_Config *) malloc(sizeof(Tamer_Config));
            if ( gTamer.config == 0 )
                return 0;

            memset(gTamer.config, 0, sizeof(Tamer_Config));
        }

        /* Figure the configuration file name and path */
        if ( gTamer.config->filePath[0] == 0 )
        {
            if ( gTamer.serviceMode == true )
            {
                /* When running as a service we expect the .ini to be in the  \Windsows directory. */
                if ( GetWindowsDirectory(iniFile, MAX_PATH) == 0 )
                    break;
            }
            else
            {
                /* When not running as a service we expect the .ini to be in the local path. */
                if ( GetCurrentDirectory(MAX_PATH, iniFile) == 0 )
                    break;
            }

            snprintf(gTamer.config->filePath, MAX_PATH, "%s\\%s", iniFile, SRVC_TAME_INI_FILE);
        }

        /* Get the configuration file CRC to see if we have to read it again */
        crc32 = Tamer_GetFileCRC(gTamer.config->filePath);
        if ( crc32 == 0 )
            break;

        /* If we got a crc that is different from the previous one invalidate the processes list */
        if ( crc32 != gTamer.config->crc32 )
        {

            /* Get the service display name, description and interval */

            gTamer.config->serviceDispalyName[0] = 0;
            GetPrivateProfileString("Service", "DisplayName", SRVC_TAME_SERVICE_DISPLAY_NAME, gTamer.config->serviceDispalyName,
                                    sizeof(((Tamer_Config *) 0)->serviceDispalyName) - 1, gTamer.config->filePath);

            gTamer.config->serviceDescription[0] = 0;
            GetPrivateProfileString("Service", "Description", SRVC_TAME_SERVICE_DESCRIPTION, gTamer.config->serviceDescription,
                                    sizeof(((Tamer_Config *) 0)->serviceDescription) - 1, gTamer.config->filePath);

            gTamer.config->interval = GetPrivateProfileInt("Service", "Interval", SRVC_TAME_INTERVAL, gTamer.config->filePath);

            /* Release the process list */
            LL_FOREACH_SAFE(gTamer.config->procList, el, tmp)
            {
                free(el); /* Release the node */
            }

            gTamer.config->procList = NULL;
            gTamer.config->crc32    = crc32; /* Update our session the current crc32 */

            /* Construct a new list based on the configuration file */
            char  configEntry[256];
            int   processIndex = 1;
            DWORD bufferSize   = sizeof(((Tamer_Proc *) 0)->procName);

            while ( 1 )
            {
                /* Build the list */
                el = (Tamer_Proc *) malloc(sizeof(Tamer_Proc));
                if ( el != NULL )
                {
                    /* Get the process name */
                    memset(el, 0, sizeof(Tamer_Proc));
                    configEntry[0] = 0;
                    snprintf(configEntry, sizeof(configEntry), "Process%d_Name", processIndex);
                    if ( GetPrivateProfileString("Processes", configEntry, "", el->procName, bufferSize - 1, gTamer.config->filePath) == 0 )
                    {
                        free(el);
                        el = NULL;
                        break;
                    }
                    /* Get the process tamed priority */
                    configEntry[0] = 0;
                    snprintf(configEntry, sizeof(configEntry), "Process%d_Prio", processIndex);
                    el->priority = GetPrivateProfileInt("Processes", configEntry, 0, gTamer.config->filePath);
                }

                if ( el == NULL )
                    break;

                /* Add to the list */
                LL_APPEND(gTamer.config->procList, el);

                processIndex++;
            }
        }

        /* Return the items we have in the process list */
        LL_COUNT(gTamer.config->procList, el, retVal);

    } while ( 0 );

    return retVal;
}

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

                Sleep(1000);
                while ( QueryServiceStatus(hService, &status) )
                {
                    if ( status.dwCurrentState == SERVICE_STOP_PENDING )
                        Sleep(500);
                    else
                        break;
                }
            }

            if ( DeleteService(hService) )
                retVal = EXIT_SUCCESS;

            CloseServiceHandle(hService);
        }

        CloseServiceHandle(hSCManager);
    }

    return retVal;
}

/**
 * @brief Installs the service.
 * @param procName Name of the executable to run as service.
 * @retval int EXIT_SUCCESS on success, EXIT_FAILURE on failure.
 */

static int Tamer_ServiceInstall(char *procName, const char *serviceName, const char *serviceDisplayName, const char *serviceDescription)
{
    int       retVal     = EXIT_FAILURE;
    char      path[512]  = {0};
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);

    if ( hSCManager )
    {
        GetFullPathName(procName, sizeof(path), path, NULL);

        SC_HANDLE hService = CreateService(hSCManager, serviceName, serviceDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                           SERVICE_ERROR_NORMAL, path, NULL, NULL, NULL, NULL, NULL);

        if ( hService )
        {
            SERVICE_DESCRIPTION desc;
            desc.lpDescription = (LPSTR) serviceDescription;

            if ( ChangeServiceConfig2(hService, SERVICE_CONFIG_DESCRIPTION, &desc) )
                retVal = EXIT_SUCCESS;
            CloseServiceHandle(hService);
        }

        CloseServiceHandle(hSCManager);
    }

    return retVal;
}

/**
 * @brief Sets the priority of a process to idle.
 * @param exeName Name of the executable whose priority to adjust.
 */

static void Tamer_SetProcessPriority(Tamer_Proc *proc)
{

    HANDLE         hProcess;
    HANDLE         hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, 0);
    PROCESSENTRY32 pEntry;
    pEntry.dwSize = sizeof(pEntry);
    BOOL hRes     = Process32First(hSnapShot, &pEntry);

    while ( hRes )
    {
        if ( _stricmp(pEntry.szExeFile, proc->procName) == 0 )
        {

            hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pEntry.th32ProcessID);

            if ( hProcess != NULL )
            {
                if ( GetPriorityClass(hProcess) != IDLE_PRIORITY_CLASS )
                    SetPriorityClass(hProcess, IDLE_PRIORITY_CLASS);

                CloseHandle(hProcess);
            }
        }

        hRes = Process32Next(hSnapShot, &pEntry);
    }

    CloseHandle(hSnapShot);
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
    Tamer_Proc *el;

    /* Update configuration as needed */
    if ( Tamer_ReadConfig() == 0 )
        return false;

    if ( gTamer.config == NULL || gTamer.config->procList == NULL )
        return false;

    LL_FOREACH(gTamer.config->procList, el)
    {
        Tamer_SetProcessPriority(el);
    }
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
        Sleep(gTamer.config->interval);
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

    gTamer.serviceMode = SRVC_TAME_RUN_AS_SERVICE;

    /* Read the configuration (.ini) file */
    if ( Tamer_ReadConfig() == 0 )
    {
        if ( gTamer.config && gTamer.config->filePath )
            printf("Error while reading configuration from %s.\n", gTamer.config->filePath);
        else
            printf("Error while looking for %s.\n", SRVC_TAME_INI_FILE);

        return EXIT_FAILURE;
    }

    /* Handle service install/ uninstall from the command line */
    if ( argc == 2 )
    {
        if ( _stricmp(argv[1], "-i") == 0 )
        {
            retVal = Tamer_ServiceInstall(argv[0], SRVC_TAME_SERVICE_NAME, gTamer.config->serviceDispalyName, gTamer.config->serviceDescription);
        }
        else if ( _stricmp(argv[1], "-u") == 0 )
        {
            retVal = Tamer_ServiceUninstall(SRVC_TAME_SERVICE_NAME);
        }
        else
        {
            printf("Unknown command line option provided.\n");
            return EXIT_FAILURE;
        }

        if ( retVal == EXIT_SUCCESS )
            printf("Operation completed successfully.\n");
        else
            printf("Operation was not completed successfully.\n");

        return retVal;
    }

    /* Service endless loop. */
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
            Sleep(gTamer.config->interval);
        }
    }

    return EXIT_SUCCESS;
}

/**
  * @}
  */
