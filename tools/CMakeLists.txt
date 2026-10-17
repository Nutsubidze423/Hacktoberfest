# Every tools/<name>.cpp becomes its own <name>.exe linked against kvm_common.
file(GLOB TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(tool_source IN LISTS TOOL_SOURCES)
  get_filename_component(tool_name ${tool_source} NAME_WE)
  add_executable(${tool_name} ${tool_source})
  target_link_libraries(${tool_name} PRIVATE
    kvm_common
    $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
    d3d11 dxgi windowsapp gdi32)
endforeach()
