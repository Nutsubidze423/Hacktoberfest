file(GLOB CLIENT_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(kvm_client ${CLIENT_SOURCES})
target_link_libraries(kvm_client PRIVATE
  kvm_common
  $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
  gdi32)
