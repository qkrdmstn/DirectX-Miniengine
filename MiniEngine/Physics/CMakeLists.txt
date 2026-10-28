cmake_minimum_required(VERSION 3.20)
project(MiniPhysics LANGUAGES CXX)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# 엔진과 같은 DLL CRT를 사용하고 최초 도입은 SSE2 CPU 경로로 고정한다.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
foreach(option IN ITEMS USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512
    USE_LZCNT USE_TZCNT USE_F16C USE_FMADD DOUBLE_PRECISION
    JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
    DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE
    ENABLE_OBJECT_STREAM FLOATING_POINT_EXCEPTIONS_ENABLED ENABLE_INSTALL)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(OBJECT_LAYER_BITS 16 CACHE STRING "" FORCE)

# 공개 커밋의 archive를 해시 검증한 뒤 공식 빌드 설정을 그대로 포함한다.
include(FetchContent)
set(JOLT_ARCHIVE "" CACHE FILEPATH "Verified Jolt archive supplied by BuildJolt.ps1")
if(NOT EXISTS "${JOLT_ARCHIVE}")
    message(FATAL_ERROR "Run Tools/BuildJolt.ps1 to download the pinned Jolt archive")
endif()
FetchContent_Declare(JoltSource
    URL "${JOLT_ARCHIVE}"
    URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
    SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltSource)

# Jolt와 무관한 순수 C++ 충돌 기하 라이브러리다.
add_library(MiniCollision STATIC Collision/ConvexElem.cpp Collision/AggregateGeom.cpp)
target_include_directories(MiniCollision PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(MiniCollision PRIVATE /utf-8 /W4)

add_library(MiniPhysics STATIC PhysicsWorld.cpp PhysicsWorldBodies.cpp PhysicsWorldStep.cpp PhysicsWorldInternal.h)
# MiniPhysics가 AggregateGeom을 쓰도록 CMake 의존으로 연결한다.
# 정적 라이브러리는 의존 대상을 포함하지 않으므로 MiniPhysics.lib에는 MiniCollision 객체가 없다.
# ModelViewer는 Physics.props로 MiniPhysics.lib와 Jolt.lib만 링크하고 Model.lib가 이미 같은 소스를 컴파일하므로,
# MiniCollision.lib까지 링크하면 중복 심볼이 생기니 viewer 링크에 추가하지 않는다.
target_link_libraries(MiniPhysics PUBLIC Jolt MiniCollision)
target_include_directories(MiniPhysics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(MiniPhysics PRIVATE /utf-8 /W4)

enable_testing()
add_executable(PhysicsQueryTests tests/PhysicsQueryTests.cpp)
target_link_libraries(PhysicsQueryTests PRIVATE MiniPhysics)
target_compile_options(PhysicsQueryTests PRIVATE /utf-8 /W4)
add_test(NAME PhysicsQueryTests COMMAND PhysicsQueryTests)

add_executable(ConvexElemTests tests/ConvexElemTests.cpp)
target_link_libraries(ConvexElemTests PRIVATE MiniCollision)
target_compile_options(ConvexElemTests PRIVATE /utf-8 /W4)
add_test(NAME ConvexElemTests COMMAND ConvexElemTests)

add_executable(AggregateGeomTests tests/AggregateGeomTests.cpp)
target_link_libraries(AggregateGeomTests PRIVATE MiniCollision)
target_compile_options(AggregateGeomTests PRIVATE /utf-8 /W4)
add_test(NAME AggregateGeomTests COMMAND AggregateGeomTests)

add_executable(AggregateSliceTests tests/AggregateSliceTests.cpp)
target_link_libraries(AggregateSliceTests PRIVATE MiniCollision)
target_compile_options(AggregateSliceTests PRIVATE /utf-8 /W4)
add_test(NAME AggregateSliceTests COMMAND AggregateSliceTests)

add_executable(BodyMassTests tests/BodyMassTests.cpp)
target_link_libraries(BodyMassTests PRIVATE MiniCollision)
target_compile_options(BodyMassTests PRIVATE /utf-8 /W4)
add_test(NAME BodyMassTests COMMAND BodyMassTests)

add_executable(PhysicsBodyTests tests/PhysicsBodyTests.cpp)
target_link_libraries(PhysicsBodyTests PRIVATE MiniPhysics)
target_compile_options(PhysicsBodyTests PRIVATE /utf-8 /W4)
add_test(NAME PhysicsBodyTests COMMAND PhysicsBodyTests)

add_executable(PhysicsStepTests tests/PhysicsStepTests.cpp)
target_link_libraries(PhysicsStepTests PRIVATE MiniPhysics)
target_compile_options(PhysicsStepTests PRIVATE /utf-8 /W4)
add_test(NAME PhysicsStepTests COMMAND PhysicsStepTests)

add_executable(PhysicsTransformTests tests/PhysicsTransformTests.cpp)
target_link_libraries(PhysicsTransformTests PRIVATE MiniPhysics)
target_compile_options(PhysicsTransformTests PRIVATE /utf-8 /W4)
add_test(NAME PhysicsTransformTests COMMAND PhysicsTransformTests)

add_executable(HullRefitTests tests/HullRefitTests.cpp)
target_link_libraries(HullRefitTests PRIVATE MiniCollision)
target_compile_options(HullRefitTests PRIVATE /utf-8 /W4)
add_test(NAME HullRefitTests COMMAND HullRefitTests)
