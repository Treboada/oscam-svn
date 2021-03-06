project (OSCam C)

cmake_minimum_required (VERSION 2.6)

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
elseif (OSCamOperatingSystem MATCHES "Amino")
    add_definitions ("-DOS_LINUX")
    set (DEFAULT_CS_CONFDIR "/usr/local/etc")
elseif (OSCamOperatingSystem MATCHES "Windows/Cygwin")
    add_definitions ("-DOS_CYGWIN32")
    set (DEFAULT_CS_CONFDIR "/usr/local/etc")
endif (OSCamOperatingSystem MATCHES "Linux")

set (CS_CONFDIR ${DEFAULT_CS_CONFDIR} CACHE STRING "Default path for the config files")
add_definitions ("-DCS_CONFDIR=\"${CS_CONFDIR}\"")

set (CS_OS_CPU ${CMAKE_SYSTEM_PROCESSOR})
set (CS_OSTYPE ${CS_OS_CPU}-${CS_OS_HW}-${CS_OS_SYS})

add_definitions ("-DUSE_CMAKE")
add_definitions ("-DCS_OSTYPE=\"${CS_OSTYPE}\"")
add_definitions ("-DCS_OS_CPU=\"${CS_OS_CPU}\"")
add_definitions ("-DCS_OS_HW=\"${CS_OS_HW}\"")
add_definitions ("-DCS_OS_SYS=\"${CS_OS_SYS}\"")

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

file (GLOB csmodules_srcs "module-*.c")
file (GLOB csmodules_hdrs "module-*.h")
file (GLOB csreaders_srcs "reader-*.c")
file (GLOB csreaders_hdrs "reader-*.h")
file (GLOB csoscam_srcs "oscam-*.c")
file (GLOB csoscam_hdrs "oscam-*.h")
file (GLOB exe_srcs "oscam.c")
file (GLOB exe_hdrs "globals.h")
file (GLOB all_srcs ${csmodules_srcs} ${csreaders_srcs} ${csoscam_srcs} ${exe_srcs})

#----------------------- modules ------------------------------

set (csmodules "csmodules")
add_library (${csmodules} STATIC ${csmodules_srcs} ${csmodules_hdrs})

#----------------------- readers ------------------------------

set (csreaders "csreaders")
add_library (${csreaders} STATIC ${csreaders_srcs} ${csreaders_hdrs})

#----------------------- other oscam files ------------------------------

set (csoscam "csoscam")
add_library (${csoscam} STATIC ${csoscam_srcs} ${csoscam_hdrs})

#----------------------- the executable ------------------------------

set (exe_name "oscam")
add_executable (${exe_name} ${exe_srcs} ${exe_hdrs})
target_link_libraries (${exe_name} ${csoscam} ${csmodules} ${csreaders} csctapi cscrypt pthread)
if (HAVE_AES)
    target_link_libraries (${exe_name} crypto)
endif (HAVE_AES)
add_dependencies (${exe_name} ${csoscam} ${csreaders} ${csmodules})

#----------------------- specific options ------------------------------

if (OSCamOperatingSystem MATCHES "Linux")
elseif (OSCamOperatingSystem MATCHES "Mac OS X")
elseif (OSCamOperatingSystem MATCHES "Tuxbox")
    target_link_libraries ( ${exe_name} dl)
elseif (OSCamOperatingSystem MATCHES "Fonera2")
elseif (OSCamOperatingSystem MATCHES "Amino")
elseif (OSCamOperatingSystem MATCHES "Windows/cygwin")
endif (OSCamOperatingSystem MATCHES "Linux")

#----------------------- global compile and link options ------------------------------

# some optimisations
add_definitions ("-O3 -Winline -Werror -finline-functions -fomit-frame-pointer")

# we don't want the '-rdynamic' in the link command
set (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")

# we want to strip
set (CMAKE_EXE_LINKER_FLAGS "-s")

#----------------------- installation -----------------------------

file (GLOB config_files "${CMAKE_CURRENT_SOURCE_DIR}/Distribution/oscam.*")
file (GLOB doc_files "${CMAKE_CURRENT_SOURCE_DIR}/Distribution/doc/*.txt")

install (PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${exe_name}${CMAKE_EXECUTABLE_SUFFIX} DESTINATION bin COMPONENT bin)
install (FILES ${config_files} DESTINATION etc COMPONENT config)
install (FILES ${doc_files} DESTINATION share/doc/oscam COMPONENT doc)

#----------------------- we can use CPack to build pacakges --------------------------

# get version from source file
execute_process (COMMAND grep CS_VERSION ${CMAKE_CURRENT_SOURCE_DIR}/globals.h
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
