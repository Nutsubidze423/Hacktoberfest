# Every tools/<name>.cpp becomes its own <name>.exe. Tools may use common, host_a and client_b code
# (include "window_picker.h" etc. directly). Static libraries only pull in the objects a tool needs.
file(GLOB TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(tool_source IN LISTS TOOL_SOURCES)
  get_filename_component(tool_name ${tool_source} NAME_WE)
  add_executable(${tool_name} ${tool_source})
  target_link_libraries(${tool_name} PRIVATE kvm_host_lib kvm_client_lib)
endforeach()
