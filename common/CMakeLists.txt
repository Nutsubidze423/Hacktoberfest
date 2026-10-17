file(GLOB COMMON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(kvm_common STATIC ${COMMON_SOURCES})
target_include_directories(kvm_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvm_common PUBLIC nlohmann_json::nlohmann_json ws2_32 user32 shcore)
