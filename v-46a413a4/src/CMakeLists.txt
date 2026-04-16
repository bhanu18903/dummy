###############################################################################
## NAME            : src/CMakeLists.txt
## PURPOSE         : Defines the executable target with all components.
## AUTO-GENERATED  : Adaptive AUTOSAR Code Generation Pipeline
###############################################################################

set(SOURCES
    main.cpp
    CanConsumer/CanConsumer.cpp
    ProbeApp/ProbeApp.cpp
    ProbeComm/ProbeComm.cpp
    ProbeCommVariant/ProbeCommVariant.cpp
    Probe_Main/Probe_Main.cpp
)

add_executable(swe3test__uid__2
    ${SOURCES}
)

target_include_directories(swe3test__uid__2
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/CanConsumer
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeApp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeComm
    ${CMAKE_CURRENT_SOURCE_DIR}/ProbeCommVariant
    ${CMAKE_CURRENT_SOURCE_DIR}/Probe_Main
)

target_link_libraries(swe3test__uid__2
    PRIVATE
    ${ARA_LIBRARIES}
    ${ARAExec_LIBRARIES}
    ${ara-com_LIBRARIES}
    ${ara-log_LIBRARIES}
    ${ara-per_LIBRARIES}
    Threads::Threads
)

target_compile_options(swe3test__uid__2
    PRIVATE
    -std=c++14
)

install(
    TARGETS swe3test__uid__2
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
