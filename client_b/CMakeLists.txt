# kvm_client_lib = every client_b/*.cpp except main.cpp, so tools/ can link client code too.
kvm_add_module_lib(kvm_client_lib ${CMAKE_CURRENT_SOURCE_DIR}
  $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
  gdi32)

add_executable(kvm_client main.cpp)
target_link_libraries(kvm_client PRIVATE kvm_client_lib)
