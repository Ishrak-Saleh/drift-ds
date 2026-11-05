#!/bin/sh
#Keeps two promises the README makes about the log:
# 1. details of the machine are read in ONE place only (runtime/sysinfo.c), so they are easy to review
# 2. the game has no network code at all, so nothing can be sent anywhere
#Where it fits: run it before every commit.
#Allowed exceptions: GetMonitorInfoW in window.c (sizes the fullscreen window, never logged),
#glGetString in window.c (the graphics card name for the log, printed to the console and log file only).
cd "$(dirname "$0")/.." || exit 1
status=0

#1. machine details outside sysinfo.c
MACHINE='GetSystemInfo|GetNativeSystemInfo|GlobalMemoryStatus|GetSystemPowerStatus|RegGetValue|RegOpenKey|RegQueryValue|EnumDisplaySettings|EnumDisplayDevices|GetComputerName|GetUserName|GetSystemTimes|RtlGetVersion|GetVersionEx|GetLogicalProcessorInformation|__cpuid|SM_CMONITORS|GetAdaptersInfo|GetAdaptersAddresses'
found=$(grep -nE "$MACHINE" runtime/*.c runtime/*.h | grep -v '^runtime/sysinfo\.c:' | grep -v '^runtime/sysinfo\.h:')
if [ -n "$found" ]; then
    echo "$found"
    echo "FAILED: machine details are read outside runtime/sysinfo.c"
    status=1
fi
found=$(grep -nE 'glGetString' runtime/*.c runtime/*.h | grep -v '^runtime/window\.c:')
if [ -n "$found" ]; then
    echo "$found"
    echo "FAILED: glGetString is used outside window.c"
    status=1
fi

#2. network code anywhere in the runtime
NETWORK='winsock|WSAStartup|socket\(|connect\(|sendto|recvfrom|WinHttp|InternetOpen|InternetConnect|URLDownload|getaddrinfo|gethostbyname|curl_|wininet|ws2_32|#include <winhttp'
found=$(grep -nE "$NETWORK" runtime/*.c runtime/*.h build.sh)
if [ -n "$found" ]; then
    echo "$found"
    echo "FAILED: network code found, the game must not talk to the internet"
    status=1
fi

[ "$status" -eq 0 ] && echo "privacy check passed: machine details only in sysinfo.c, no network code"
exit "$status"
