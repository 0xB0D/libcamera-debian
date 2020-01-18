v4l2_compat_sources = files([
    'v4l2_camera.cpp',
    'v4l2_camera_proxy.cpp',
    'v4l2_compat.cpp',
    'v4l2_compat_manager.cpp',
])

v4l2_compat_includes = [
    libcamera_includes,
    libcamera_internal_includes,
]

v4l2_compat_cpp_args = [
    # Meson enables large file support unconditionally, which redirect file
    # operations to 64-bit versions. This results in some symbols being
    # renamed, for instance open() being renamed to open64(). As the V4L2
    # adaptation wrapper needs to provide both 32-bit and 64-bit versions of
    # file operations, disable transparent large file support.
    '-U_FILE_OFFSET_BITS',
    '-D_FILE_OFFSET_BITS=32',
    '-fvisibility=hidden',
]

v4l2_compat = shared_library('v4l2-compat',
                             v4l2_compat_sources,
                             name_prefix : '',
                             install : true,
                             link_with : libcamera,
                             include_directories : v4l2_compat_includes,
                             dependencies : libcamera_deps,
                             cpp_args : v4l2_compat_cpp_args)