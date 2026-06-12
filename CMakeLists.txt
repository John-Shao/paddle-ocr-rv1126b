cmake_minimum_required(VERSION 3.15)

project(ppocr_text_rv1126b)

set(MODEL_ZOO_ROOT "" CACHE PATH "Path to Rockchip rknn_model_zoo")
if (NOT MODEL_ZOO_ROOT)
    set(MODEL_ZOO_ROOT "$ENV{HOME}/rknn_model_zoo")
endif()
if (NOT EXISTS "${MODEL_ZOO_ROOT}/examples/PPOCR/PPOCR-System/cpp")
    message(FATAL_ERROR "MODEL_ZOO_ROOT is invalid: ${MODEL_ZOO_ROOT}")
endif()

add_subdirectory(${MODEL_ZOO_ROOT}/3rdparty 3rdparty.out)
add_subdirectory(${MODEL_ZOO_ROOT}/utils utils.out)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(TARGET_LIB_ARCH lib64)
else()
    set(TARGET_LIB_ARCH lib)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(FATAL_ERROR "Android build is not used for this project")
else()
    if (TARGET_LIB_ARCH STREQUAL "lib")
        set(OpenCV_DIR ${MODEL_ZOO_ROOT}/3rdparty/opencv/opencv-linux-armhf/share/OpenCV)
    else()
        set(OpenCV_DIR ${MODEL_ZOO_ROOT}/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV)
    endif()
endif()
find_package(OpenCV REQUIRED)

set(PPOCR_CPP ${MODEL_ZOO_ROOT}/examples/PPOCR/PPOCR-System/cpp)
set(PPOCR_SOURCES
    src/ppocr_text.cc
    ${PPOCR_CPP}/postprocess.cc
    ${PPOCR_CPP}/clipper.cc
    ${PPOCR_CPP}/rknpu2/ppocr_system.cc
)

foreach(target ppocr_text ppocrd)
    add_executable(${target} ${PPOCR_SOURCES})

    target_include_directories(${target} PRIVATE
        ${PPOCR_CPP}
        ${LIBRKNNRT_INCLUDES}
    )

    target_link_libraries(${target}
        fileutils
        imageutils
        opencv_core
        opencv_imgproc
        ${LIBRKNNRT}
    )

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(THREADS_PREFER_PTHREAD_FLAG ON)
        find_package(Threads REQUIRED)
        target_link_libraries(${target} Threads::Threads)
    endif()
endforeach()

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib:$ORIGIN/../lib:$ORIGIN")

install(TARGETS ppocr_text ppocrd DESTINATION .)
install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/deploy/S96ppocrd DESTINATION service)
install(FILES ${LIBRKNNRT} DESTINATION lib)
install(PROGRAMS ${MODEL_ZOO_ROOT}/3rdparty/librga/Linux/aarch64/librga.so DESTINATION lib)
install(FILES
    ${CMAKE_SYSROOT}/usr/lib/libopencv_core.so
    ${CMAKE_SYSROOT}/usr/lib/libopencv_core.so.409
    ${CMAKE_SYSROOT}/usr/lib/libopencv_core.so.4.9.0
    ${CMAKE_SYSROOT}/usr/lib/libopencv_imgproc.so
    ${CMAKE_SYSROOT}/usr/lib/libopencv_imgproc.so.409
    ${CMAKE_SYSROOT}/usr/lib/libopencv_imgproc.so.4.9.0
    ${CMAKE_SYSROOT}/usr/lib/libgomp.so
    ${CMAKE_SYSROOT}/usr/lib/libgomp.so.1
    ${CMAKE_SYSROOT}/usr/lib/libgomp.so.1.0.0
    DESTINATION lib
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/ppocrv4_det_i8.rknn")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/models/ppocrv4_det_i8.rknn" DESTINATION models)
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/ppocrv4_rec_fp16.rknn")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/models/ppocrv4_rec_fp16.rknn" DESTINATION models)
endif()
