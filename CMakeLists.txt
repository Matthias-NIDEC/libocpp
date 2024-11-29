
#add_library(ocpp)
#add_library(everest::ocpp ALIAS ocpp)

add_subdirectory(../${LIBOCPPCONFIG16})
add_subdirectory(../${LIBOCPPCONFIG201})

add_subdirectory(../${LIBOCPPLIB})

target_link_options(ocpp
    PRIVATE
        -ffunction-sections 
        -fdata-sections
        -Wl,--gc-sections
        -Wl,--strip-all
)

install(TARGETS ocpp
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION libocpp
    INCLUDES DESTINATION include
)
# config and auxillary files
#add_subdirectory(${LIBOCPPCONFIG})