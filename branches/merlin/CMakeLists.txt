project (OSCam C)

#----------------------- minimum version of cmake to use ------------

# if you want to switch to 2.6 min, please look at the escaped definitions
# later in this file (the add_definitions in 2.6 don't need to be escaped),
# and remove the CMP0005 policy.
# the CMP0003 could also be removed for cmake 2.6
cmake_minimum_required (VERSION 2.4)
if(COMMAND cmake_policy)
    cmake_policy(SET CMP0005 OLD)
    cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

#----------------------- detect system ------------------------------

if (CMAKE_CROSSCOMPILING)
    if (OSCAM_SYSTEM_NAME MATCHES "Tuxbox")
        set (OSCamOperatingSystem "Tuxbox")
        set (CS_OS_HW "tuxbox")
        set (CS_OS_SYS "linux")
    elseif (OSCAM_SYSTEM_NAME MATCHES "Fonera2")
        set (OSCamOperatingSystem "Fonera2")
        set (CS_OS_HW "fonera2")
        set (CS_OS_SYS "linux")
    elseif (OSCAM_SYSTEM_NAME MATCHES "WRT54G")
        set (OSCamOperatingSystem "WRT54G")
        set (CS_OS_HW "wrt54g")
        set (CS_OS_SYS "linux")
    elseif (OSCAM_SYSTEM_NAME MATCHES "Amino")
        set (OSCamOperatingSystem "Amino")
        set (CS_OS_HW "amino")
        set (CS_OS_SYS "linux")
    else (OSCAM_SYSTEM_NAME MATCHES "Tuxbox")
        message (STATUS "Unknown cross system name: <${CMAKE_SYSTEM_NAME}>")
        set (OSCamOperatingSystem "Unknown")
        set (CS_OS_HW "unknown")
        set (CS_OS_SYS "unknown")
    endif (OSCAM_SYSTEM_NAME MATCHES "Tuxbox")
else (CMAKE_CROSSCOMPILING)
    if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        set (OSCamOperatingSystem "Linux")
        set (CS_OS_HW "pc")
        set (CS_OS_SYS "linux")
    elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
        set (OSCamOperatingSystem "Mac OS X")
        set (CS_OS_HW "mac")
        set (CS_OS_SYS "macosx")
    elseif (${CYGWIN})
        set (OSCamOperatingSystem "Windows/Cygwin")
        set (CS_OS_HW "pc")
        set (CS_OS_SYS "cygwin")
        set (CMAKE_SYSTEM_PROCESSOR "i386")
    else (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        message (STATUS "Unknown system name: <${CMAKE_SYSTEM_NAME}>")
        set (OSCamOperatingSystem "Unknown")
        set (CS_OS_HW "unknown")
        set (CS_OS_SYS "unknown")
    endif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
endif (CMAKE_CROSSCOMPILING)

#----------------------- some paths ------------------------------

set (OPTIONAL_LINK_DIR "" CACHE STRING "Some optional link directories")
set (OPTIONAL_INCLUDE_DIR "" CACHE STRING "Some optional include directories")

include_directories (
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/csctapi
    ${CMAKE_CURRENT_SOURCE_DIR}/cscrypt
    ${OPTIONAL_INCLUDE_DIR}
    )
link_directories (${OPTIONAL_LINK_DIR})

#----------------------- global options ------------------------------

if (OSCamOperatingSystem MATCHES "Linux")
    add_definitions ("-DOS_LINUX")
    set (DEFAULT_CS_CONFDIR "/usr/local/etc")
elseif (OSCamOperatingSystem MATCHES "Mac OS X")
    add_definitions ("-DOS_MACOSX -DNEED_DAEMON -DCS_NOSHM -DHAVE_PTHREAD_H -DUSE_PTHREAD")
    set (DEFAULT_CS_CONFDIR "/usr/local/etc")
elseif (OSCamOperatingSystem MATCHES "Tuxbox")
    add_definitions ("-DOS_LINUX -DTUXBOX -DPPC")
    set (DEFAULT_CS_CONFDIR "/var/tuxbox/config")
elseif (OSCamOperatingSystem MATCHES "Fonera2")
    add_definitions ("-DOS_LINUX -DMIPSEL -DUCLIBC")
    set (DEFAULT_CS_CONFDIR "/var/etc")
elseif (OSCamOperatingSystem MATCHES "WRT54G")
    add_definitions ("-DOS_LINUX -DMIPSEL -DUCLIBC -DUSE_GPIO")
    set (DEFAULT_CS_CONFDIR "/jffs/etc/config/oscam")
elseif (OSCamOperatingSystem MATCHES "Amino")
    add_definitions ("-DOS_LINUX")
    set (DEFAULT_CS_CONFDIR "/usr/local/etc")
elseif (OSCamOperatingSystem MATCHES "Windows/Cygwin")
    add_definitions ("-DOS_CYGWIN32")
    set (DEFAULT_CS_CONFDIR ".")
endif (OSCamOperatingSystem MATCHES "Linux")

set (CS_CONFDIR ${DEFAULT_CS_CONFDIR} CACHE STRING "Default path for the config files")
add_definitions ("-DCS_CONFDIR=\\\"${CS_CONFDIR}\\\"")

set (CS_OS_CPU ${CMAKE_SYSTEM_PROCESSOR})
set (CS_OSTYPE ${CS_OS_CPU}-${CS_OS_HW}-${CS_OS_SYS})

add_definitions ("-DCS_OSTYPE=\\\"${CS_OSTYPE}\\\"")
add_definitions ("-DCS_OS_CPU=\\\"${CS_OS_CPU}\\\"")
add_definitions ("-DCS_OS_HW=\\\"${CS_OS_HW}\\\"")
add_definitions ("-DCS_OS_SYS=\\\"${CS_OS_SYS}\\\"")

#----------------------- subdirectories ------------------------------

include (CheckIncludeFile)
check_include_file ("openssl/aes.h" HAVE_AES)
if (HAVE_AES)
    add_definitions ("-DHAVE_AES")
endif (HAVE_AES)

#----------------------- subdirectories ------------------------------

add_subdirectory (csctapi)
add_subdirectory (cscrypt)

#----------------------- file groups ------------------------------

file (GLOB cscam_srcs "src/CAM/*.c")
file (GLOB cscam_hdrs "include/CAM/*.h")

file (GLOB csreader_srcs "src/reader/*.c")
file (GLOB csreader_hdrs "include/reader/*.h")

file (GLOB cssharing_srcs "src/sharing/*.c")
file (GLOB cssharing_hdrs "include/sharing/*.h")

file (GLOB csoscam_srcs "src/*.c")
file (GLOB csoscam_hdrs "include/*.h")

file (GLOB exe_srcs "src/oscam.c")
file (GLOB exe_hdrs "include/globals.h")

file (GLOB all_srcs ${cscam_srcs} ${cssharing_srcs} ${csreader_srcs} ${csoscam_srcs} ${exe_srcs})

#----------------------- CAM ----------------------------------

set (cscam "cscam")
add_library (${cscam} STATIC ${cscam_srcs} ${cscam_hdrs})

#----------------------- reader -------------------------------

set (csreader "csreader")
add_library (${csreader} STATIC ${csreader_srcs} ${csreader_hdrs})

#----------------------- sharing ------------------------------

set (cssharing "cssharing")
add_library (${cssharing} STATIC ${cssharing_srcs} ${cssharing_hdrs})

#----------------------- other oscam files ------------------------------

set (csoscam "csoscam")
add_library (${csoscam} STATIC ${csoscam_srcs} ${csoscam_hdrs})

#----------------------- the executable ------------------------------

set (exe_name "oscam")
add_executable (${exe_name} ${exe_srcs} ${exe_hdrs})
target_link_libraries (${exe_name} ${csoscam} ${cscam} ${csreader} ${cssharing} csctapi cscrypt pthread)
if (HAVE_AES)
    target_link_libraries (${exe_name} crypto)
endif (HAVE_AES)
add_dependencies (${exe_name} ${csoscam} ${cscam} ${csreader} ${cssharing})

#----------------------- specific options ------------------------------

if (OSCamOperatingSystem MATCHES "Linux")
elseif (OSCamOperatingSystem MATCHES "Mac OS X")
elseif (OSCamOperatingSystem MATCHES "Tuxbox")
    target_link_libraries ( ${exe_name} dl)
elseif (OSCamOperatingSystem MATCHES "Fonera2")
elseif (OSCamOperatingSystem MATCHES "WRT54G")
elseif (OSCamOperatingSystem MATCHES "Amino")
elseif (OSCamOperatingSystem MATCHES "Windows/cygwin")
endif (OSCamOperatingSystem MATCHES "Linux")

#----------------------- some compile option to better check the code ---------------

set (CMAKE_C_FLAGS_CODECHECK "-Wall -Werror -Wno-strict-aliasing -Wmissing-declarations -Wnested-externs -Wredundant-decls -Wundef -Winline -Waggregate-return -Wbad-function-cast -Wmissing-field-initializers -Wcast-align")

#----------------------- global compile and link options ------------------------------

# some optimisations
add_definitions ("-O3 -Winline -Werror -finline-functions -fomit-frame-pointer")

# we don't want the '-rdynamic' in the link command
set (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

# we want to strip
set (CMAKE_EXE_LINKER_FLAGS "-s")

#----------------------- installation -----------------------------

file (GLOB config_files "${CMAKE_CURRENT_SOURCE_DIR}/Distribution/oscam.*")
file (GLOB doc_files "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/txt/*.txt")
file (GLOB man1_files "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/man/*.1")
file (GLOB man5_files "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/man/*.5")
file (GLOB example_files "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.ac"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.cert"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.conf"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.guess"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.ird"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.server"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.services"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.srvid"
                         "${CMAKE_CURRENT_SOURCE_DIR}/Documentation/example/oscam.user")
file (GLOB monitor_files "${CMAKE_CURRENT_SOURCE_DIR}/monitor/mpcs*.sh"
                         "${CMAKE_CURRENT_SOURCE_DIR}/monitor/mpcs*.tar")

install (PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${exe_name}${CMAKE_EXECUTABLE_SUFFIX} DESTINATION bin COMPONENT bin)
install (FILES ${config_files} DESTINATION etc COMPONENT config)
install (FILES ${doc_files} DESTINATION share/doc/oscam COMPONENT doc)
install (FILES ${man1_files} DESTINATION share/man/man1 COMPONENT doc)
install (FILES ${man5_files} DESTINATION share/man/man5 COMPONENT doc)
install (FILES ${example_files} DESTINATION share/doc/oscam/example COMPONENT doc)
install (FILES ${monitor_files} DESTINATION share/doc/oscam/monitor COMPONENT doc)

#----------------------- we can use CPack to build pacakges --------------------------

# get version from source file
execute_process (COMMAND grep CS_VERSION ${CMAKE_CURRENT_SOURCE_DIR}/include/globals.h
    OUTPUT_VARIABLE CS_VERSION)
string (REGEX REPLACE ".*\"\(.*\)\".*" "\\1" CS_VERSION ${CS_VERSION})
string (REGEX REPLACE "\(.*\)\\..*\\..*" "\\1" CPACK_PACKAGE_VERSION_MAJOR ${CS_VERSION})
string (REGEX REPLACE ".*\\.\(.*\)\\..*" "\\1" CPACK_PACKAGE_VERSION_MINOR ${CS_VERSION})
string (REGEX REPLACE ".*\\..*\\.\(.*\)" "\\1" CPACK_PACKAGE_VERSION_PATCH ${CS_VERSION})
set (CPACK_PACKAGE_VERSION
     "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")

# compute my filename
set (CPACK_PACKAGE_FILE_NAME
     "${PROJECT_NAME}-${CPACK_PACKAGE_VERSION}-${CS_OSTYPE}")

# wich archive we want to generate
set (CPACK_GENERATOR "TGZ")

# what to put in the archive
set (CPACK_STRIP_FILES TRUE)
set (CPACK_COMPONENTS_ALL bin)# config doc)

# ok, we can now use CPack
include (CPack)

#----------------------- printout resume -----------------------------

message (STATUS "")
if (CMAKE_BUILD_TYPE)
    message (STATUS "  build type: ${CMAKE_BUILD_TYPE}")
else (CMAKE_BUILD_TYPE)
    message (STATUS "  build type: unspecified")
endif (CMAKE_BUILD_TYPE)
message (STATUS "  operating system: ${OSCamOperatingSystem}")
message (STATUS "  system type: ${CS_OSTYPE}")
message (STATUS "  system: ${CS_OS_SYS}")
message (STATUS "  cpu: ${CS_OS_CPU}")
message (STATUS "  hw: ${CS_OS_HW}")
if (HAVE_AES)
    message (STATUS "  use system aes functions")
else (HAVE_AES)
    message (STATUS "  use built-in aes functions")
endif (HAVE_AES)
message (STATUS "")
