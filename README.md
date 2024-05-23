# Intro.
If the scenario below resonates with you, then '**Process Tamer**' is designed for you.

You've just received the latest computer from your corporate IT, but after a few updates and several restarts, a disappointing realization dawns on you: this new machine is slower than the 386 you used in high school.
 While the hardware specifications are impeccable, the multitude of agents, background services, security tools, and other less obvious processes running on your laptop can slow down even the most powerful systems. 
  
The simplest solution might seem to be terminating and removing all those processes. However, such drastic actions could potentially put you at odds with your IT department, not to mention risking the functionality of your precious laptop.

## What is 'Process Tamer'?

Process Tamer adopts a more subtle approach. Rather than eliminating these processes, it **reduces their priority to the bare minimum** and ensures they remain that way, allowing your essential applications to have the resources they need without completely disrupting the system's operational integrity.

## Service configuration file.

**'Process Tamer**' utilizes a configuration file, specifically an .INI file, which should be placed in your **Windows** directory. Below is an example illustrating the expected format of this file:

    ; This section defines service-level settings
    [Service]
    ; The name displayed in the services management console
    DisplayName="Intel(R) Process Tamer"
    ; Description of what the service does
    Description="Intel(R) service to manage and reduce the priority of specified IT processes to mitigate their impact on system performance."
    ; Interval in milliseconds between checks
    Interval= 1000
    
    ; This section lists the processes to be managed
    [Processes]
    ; Name of the first process and its priority level
    Process1_Name=it-agent.exe 
    Process1_Prio=0

## Building / Installing:

1. Compile the executable and place it in the desired location.
2. Create a configuration file named 'SrvcTame.ini' using Notepad. Follow the provided format and place the file in the Windows directory. Alternatively, for debugging purposes, set **SRVC_TAME_RUN_AS_SERVICE** to false and place the configuration file alongside the executable being debugged.
3. Open a command prompt with **administrative** rights, navigate to the directory where the executable is located, and execute the following command to install the service: `SrvcTame -i` or `SrvcTame -u` to uninstall. 
4. To start the service, either restart your system or use the 'Windows Service Management' tool to start the service named "Process Tamer."
