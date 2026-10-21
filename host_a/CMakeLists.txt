# kvm_host_lib = every host_a/*.cpp except main.cpp, so tools/ can link host code too.
kvm_add_module_lib(kvm_host_lib ${CMAKE_CURRENT_SOURCE_DIR}
  $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
  d3d11 dxgi windowsapp mfplat mfuuid)

# Every feature works in both directions (protocol v2), so each exe links both module libraries:
# A can show B's window (viewer from client_b) and B can stream its own (capture from host_a).
add_executable(kvm_host main.cpp)
target_link_libraries(kvm_host PRIVATE kvm_host_lib kvm_client_lib)
