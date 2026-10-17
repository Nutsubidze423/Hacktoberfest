file(GLOB HOST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(kvm_host ${HOST_SOURCES})
target_link_libraries(kvm_host PRIVATE
  kvm_common
  $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
  d3d11 dxgi windowsapp)
