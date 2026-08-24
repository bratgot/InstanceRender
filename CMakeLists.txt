cmake_minimum_required(VERSION 3.20)
project(NukeInstanceRender CXX)

# ---------------------------------------------------------------------------
# InstanceRender - instancing renderer for Nuke, 14.1 to 17.
#   cmake -G "Visual Studio 17 2022" -A x64 -DNUKE_ROOT="C:/Program Files/Nuke17.0v4" -B build17.0
#   cmake --build build17.0 --config Release
# Dependencies: Nuke (NDK, and its bundled pxr USD libs where it has them),
# Embree 4 (EMBREE_ROOT), optional CUDA 12 + OptiX 9 for the GPU back-end.
#
# One build per Nuke minor version: the NDK is not compatible across them, and
# each Nuke carries its own USD.  Nuke 14.1 ships the USD libraries but NOT their
# headers, so the USD front end cannot be compiled there - that build gets the
# classic-3D front end only, which is why the USD half is optional here.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Defaults per platform. Nuke installs under /usr/local on Linux, and Foundry
# documents the compiler to use: gcc 9 for Nuke 14, gcc 11 for 15-17.
#   cmake -S . -B build17.1 -DCMAKE_BUILD_TYPE=Release #         -DNUKE_ROOT=/usr/local/Nuke17.1v1 -DEMBREE_ROOT=/usr/local/embree4
if(WIN32)
    set(_nuke_default   "C:/Program Files/Nuke17.0v4")
    set(_embree_default "C:/dev/embree-4")
    set(_cuda_default   "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6")
    set(_optix_default  "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0")
else()
    set(_nuke_default   "/usr/local/Nuke17.0v4")
    set(_embree_default "/usr/local/embree4")
    set(_cuda_default   "/usr/local/cuda")
    set(_optix_default  "/usr/local/optix")
endif()
set(NUKE_ROOT "${_nuke_default}" CACHE PATH "Nuke install root (contains DDImage, include/, FnUSD/)")
set(EMBREE_ROOT "${_embree_default}" CACHE PATH "Embree 4 install root")
set(CUDA_TOOLKIT_ROOT "${_cuda_default}" CACHE PATH "CUDA toolkit")
set(OPTIX_ROOT "${_optix_default}" CACHE PATH "OptiX SDK")
option(IR_WITH_OPTIX "Build the OptiX GPU back-end" OFF)

if(WIN32)
    set(_ddimage_probe "${NUKE_ROOT}/DDImage.lib")
else()
    set(_ddimage_probe "${NUKE_ROOT}/libDDImage.so")
endif()
if(NOT EXISTS "${_ddimage_probe}")
    message(FATAL_ERROR "Nuke not found: no ${_ddimage_probe}. Set -DNUKE_ROOT=<install>")
endif()
string(REGEX MATCH "Nuke([0-9]+)\\.([0-9]+)" _nk "${NUKE_ROOT}")
set(NUKE_MAJOR "${CMAKE_MATCH_1}")
set(NUKE_MINOR "${CMAKE_MATCH_2}")
if(NUKE_MAJOR STREQUAL "" OR NUKE_MINOR STREQUAL "")
    message(FATAL_ERROR "could not read a Nuke version out of NUKE_ROOT: ${NUKE_ROOT}")
endif()
set(NUKE_VER "${NUKE_MAJOR}.${NUKE_MINOR}")
math(EXPR NUKE_VER_INT "${NUKE_MAJOR} * 100 + ${NUKE_MINOR}")

# The USD front end needs two things Nuke only has from 16.0 on:
#
#   pxr headers          Nuke 14.1 ships the USD libraries but no headers at all.
#   GeometryProviderI    the entry point that hands a plugin a composed stage.
#                        Before 16.0 the only route is the static
#                        GeomOp::buildStage(), and calling that from a plugin
#                        SEGFAULTS in 15.2 - unless Nuke's own ScanlineRender2
#                        has already rendered the same graph once, which
#                        pins it on initialisation only Foundry's renderer does.
#                        Rather than ship a crash, 15.x gets the classic front
#                        end, which works there like everywhere else.
if(EXISTS "${NUKE_ROOT}/FnUSD/include/pxr/pxr.h" AND NUKE_VER_INT GREATER_EQUAL 1600)
    set(HAS_USD TRUE)
elseif(NUKE_VER_INT LESS 1600)
    set(HAS_USD FALSE)
    message(STATUS "Nuke ${NUKE_VER}: no USD front end (needs GeometryProviderI, 16.0+) - classic 3D only")
else()
    set(HAS_USD FALSE)
    message(STATUS "Nuke ${NUKE_VER} ships no pxr headers - USD front end off, classic 3D only")
endif()

# ---- Nuke + pxr -----------------------------------------------------------
find_library(LIB_DDIMAGE   DDImage          PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_NDK       Ndk              PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH)
find_library(LIB_USDABS    FnUsdAbstraction PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDENGINE FnUsdEngine      PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH)
find_library(LIB_TBB       tbb              PATHS "${NUKE_ROOT}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_TBB12     tbb12            PATHS "${NUKE_ROOT}" "${EMBREE_ROOT}/lib" NO_DEFAULT_PATH)
if(WIN32)
    file(GLOB _fdk "${NUKE_ROOT}/FdkBase*.lib")
else()
    file(GLOB _fdk "${NUKE_ROOT}/libFdkBase*.so")
endif()
list(GET _fdk 0 LIB_FDKBASE)
# Volumes need pxr's usdVol schema, which not every Nuke that has USD ships:
# 16.0 has the pxr headers but no usdVol at all, so the volume front end is gated
# separately from the USD one rather than breaking that build.
set(HAS_USDVOL FALSE)
if(HAS_USD AND EXISTS "${NUKE_ROOT}/FnUSD/include/pxr/usd/usdVol/volume.h"
   AND (EXISTS "${NUKE_ROOT}/FnUSD/lib/usd_usdVol.lib"
        OR EXISTS "${NUKE_ROOT}/FnUSD/lib/libusd_usdVol.so"))
    set(HAS_USDVOL TRUE)
else()
    message(STATUS "Nuke ${NUKE_VER}: no usdVol - volumes off, everything else unchanged")
endif()

set(USD_LIB_NAMES usd_tf usd_gf usd_vt usd_arch usd_js usd_trace usd_work usd_plug usd_sdf usd_pcp usd_ar usd_kind
                  usd_usd usd_usdGeom usd_usdShade usd_usdLux usd_hio usd_python)
set(USD_LIBS)
if(HAS_USDVOL)
    list(APPEND USD_LIB_NAMES usd_usdVol)
endif()
if(HAS_USD)
    foreach(_lib ${USD_LIB_NAMES})
        find_library(_found_${_lib}_${NUKE_VER_INT} ${_lib} PATHS "${NUKE_ROOT}/FnUSD/lib" NO_DEFAULT_PATH)
        if(_found_${_lib}_${NUKE_VER_INT})
            list(APPEND USD_LIBS ${_found_${_lib}_${NUKE_VER_INT}})
        else()
            message(WARNING "USD lib not found: ${_lib}")
        endif()
    endforeach()
endif()
# Nuke ships pythonXY.dll but no import library for it, and which XY depends on
# the version (3.10 for Nuke 15, 3.11 for 16 and 17), so pick the one that
# matches this install.  third_party/mkpythonlib.ps1 makes new ones.
# WINDOWS ONLY. An import library is a Windows idea: on Linux the plugin links
# Nuke's shipped libpython .so directly, and there is nothing to synthesise.
set(LIB_PYTHON "")
if(HAS_USD AND WIN32)
    file(GLOB _pydll "${NUKE_ROOT}/python3*.dll")
    foreach(_dll ${_pydll})
        get_filename_component(_pyname "${_dll}" NAME_WE)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_pyname}.lib")
            set(LIB_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_pyname}.lib")
        endif()
    endforeach()
    if(LIB_PYTHON STREQUAL "")
        message(FATAL_ERROR "no python import library for ${NUKE_ROOT} - run third_party/mkimportlibs.ps1")
    endif()
    message(STATUS "python import library: ${LIB_PYTHON}")
elseif(HAS_USD)
    file(GLOB _pyso "${NUKE_ROOT}/libpython3*.so*")
    if(_pyso)
        list(GET _pyso 0 LIB_PYTHON)
        message(STATUS "python: ${LIB_PYTHON}")
    endif()
endif()

# ---- TBB headers ------------------------------------------------------------
# pxr headers include tbb/..., and Nuke ships the DLL and the import library but
# no headers.  WHICH TBB matters: Nuke 15 and 16 carry a USD built against the
# old TBB (tbb::task, removed in oneTBB), Nuke 17 one built against oneTBB.
# Picking the wrong one fails to compile inside pxr/base/work/dispatcher.h.
set(TBB_NEEDS_OLD FALSE)
if(HAS_USD AND EXISTS "${NUKE_ROOT}/FnUSD/include/pxr/base/work/dispatcher.h")
    file(READ "${NUKE_ROOT}/FnUSD/include/pxr/base/work/dispatcher.h" _disp)
    if(_disp MATCHES "tbb::task[^_]" OR _disp MATCHES "tbb/task.h")
        set(TBB_NEEDS_OLD TRUE)
    endif()
endif()
set(TBB_INCLUDE_DIR "" CACHE PATH "TBB headers matching this Nuke's USD")
if(TBB_INCLUDE_DIR STREQUAL "")
    if(TBB_NEEDS_OLD)
        set(_tbb_cands "C:/dev/tbb/include" "C:/dev/External/tbb/include")
        set(_tbb_probe "tbb/task.h")
    else()
        set(_tbb_cands "C:/vcpkg/installed/x64-windows/include" "C:/dev/External/oneTBB/oneTBB-2021.5.0/include" "${EMBREE_ROOT}/include")
        set(_tbb_probe "tbb/concurrent_queue.h")
    endif()
    foreach(_cand ${_tbb_cands})
        if(EXISTS "${_cand}/${_tbb_probe}")
            set(TBB_INCLUDE_DIR "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(TBB_NEEDS_OLD)
    message(STATUS "Nuke ${NUKE_VER}'s USD wants the pre-oneTBB API")
endif()
if(TBB_INCLUDE_DIR STREQUAL "" AND HAS_USD)
    message(FATAL_ERROR "TBB headers not found - set TBB_INCLUDE_DIR")
endif()
message(STATUS "TBB headers: ${TBB_INCLUDE_DIR}")

# ---- boost (only for the Nukes whose USD headers were built against it) ------
set(NEEDS_BOOST FALSE)
if(HAS_USD AND EXISTS "${NUKE_ROOT}/FnUSD/include/pxr/base/tf/pyObjWrapper.h")
    file(READ "${NUKE_ROOT}/FnUSD/include/pxr/base/tf/pyObjWrapper.h" _pyobj)
    if(_pyobj MATCHES "include <boost/python")
        set(NEEDS_BOOST TRUE)
    endif()
endif()
set(BOOST_INCLUDE_DIR "" CACHE PATH "boost headers (only needed for Nuke 15/16)")
if(NEEDS_BOOST AND BOOST_INCLUDE_DIR STREQUAL "")
    file(GLOB _boost_cands "C:/dev/boost_*" "C:/local/boost_*" "C:/boost_*")
    list(APPEND _boost_cands "C:/vcpkg/installed/x64-windows/include")
    foreach(_cand ${_boost_cands})
        if(EXISTS "${_cand}/boost/python/object_fwd.hpp")
            set(BOOST_INCLUDE_DIR "${_cand}")
            break()
        elseif(EXISTS "${_cand}/python/object_fwd.hpp")
            get_filename_component(BOOST_INCLUDE_DIR "${_cand}/.." ABSOLUTE)
            break()
        endif()
    endforeach()
endif()

# ---- Embree ------------------------------------------------------------------
find_library(LIB_EMBREE embree4 PATHS "${EMBREE_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
set(EMBREE_INCLUDE_DIR "${EMBREE_ROOT}/include")

# ---- OptiX / CUDA (optional) -----------------------------------------------------
set(HAS_OPTIX FALSE)
if(WIN32)
    set(_nvcc "${CUDA_TOOLKIT_ROOT}/bin/nvcc.exe")
else()
    set(_nvcc "${CUDA_TOOLKIT_ROOT}/bin/nvcc")
endif()
if(IR_WITH_OPTIX AND EXISTS "${OPTIX_ROOT}/include/optix.h" AND EXISTS "${_nvcc}")
    set(HAS_OPTIX TRUE)
    # lib/x64 on Windows, lib64 on Linux
    find_library(LIB_CUDA cuda PATHS "${CUDA_TOOLKIT_ROOT}/lib/x64" "${CUDA_TOOLKIT_ROOT}/lib64"
                 "${CUDA_TOOLKIT_ROOT}/lib64/stubs" NO_DEFAULT_PATH)
    find_library(LIB_CUDART cudart PATHS "${CUDA_TOOLKIT_ROOT}/lib/x64" "${CUDA_TOOLKIT_ROOT}/lib64"
                 NO_DEFAULT_PATH)
    # The custom command has no VS environment, so on Windows nvcc has to be
    # pointed at cl.exe by hand. On Linux it finds gcc the ordinary way and
    # -ccbin would only get in the way.
    set(_ccbin "")
    if(MSVC)
        get_filename_component(_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_ccbin -ccbin "${_MSVC_BIN_DIR}")
    endif()
    set(IR_PTX "${CMAKE_CURRENT_BINARY_DIR}/InstanceRenderKernel.ptx")
    add_custom_command(
        OUTPUT "${IR_PTX}"
        COMMAND "${_nvcc}" -ptx -arch=sm_75 -std=c++17 -O3 --use_fast_math -lineinfo
                ${_ccbin}
                -I "${OPTIX_ROOT}/include" -I "${CUDA_TOOLKIT_ROOT}/include" -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
                -DIR_OPTIX_KERNEL=1
                -o "${IR_PTX}" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/OptixKernel.cu"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/OptixKernel.cu" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/Kernel.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/Math.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/Scene.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ir/OptixShared.h"
        COMMENT "nvcc: InstanceRender OptiX kernel -> PTX" VERBATIM)
    # embed the PTX as a C string
    set(IR_PTX_CPP "${CMAKE_CURRENT_BINARY_DIR}/InstanceRenderKernel_ptx.cpp")
    add_custom_command(
        OUTPUT "${IR_PTX_CPP}"
        COMMAND ${CMAKE_COMMAND} -DPTX_IN=${IR_PTX} -DCPP_OUT=${IR_PTX_CPP} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedPTX.cmake"
        DEPENDS "${IR_PTX}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedPTX.cmake"
        COMMENT "embedding PTX" VERBATIM)
    message(STATUS "OptiX found: ${OPTIX_ROOT}")
else()
    message(STATUS "OptiX/CUDA not found or disabled - GPU back-end off")
endif()

# ---- plugin ----------------------------------------------------------------------
set(IR_SOURCES src/InstanceRender.cpp src/ir/GeoLoader.cpp src/ir/EmbreeBackend.cpp src/ir/NukeOpImage.cpp src/ir/Watchdog.cpp)
if(HAS_USD)
    list(APPEND IR_SOURCES src/ir/StageLoader.cpp src/ir/Texture.cpp)
endif()
if(HAS_OPTIX)
    list(APPEND IR_SOURCES src/ir/OptixBackend.cpp src/ir/Denoise.cpp "${IR_PTX_CPP}")
endif()
add_library(InstanceRender MODULE ${IR_SOURCES})

# Debug symbols in the RELEASE build.  They change no generated code - /Zi only
# writes a .pdb beside the .dll, and /OPT:REF,ICF put back the folding that
# /DEBUG would otherwise turn off - but without them the freeze reports written
# by src/ir/Watchdog.cpp name our own frames "(no symbol)", which is precisely
# the half of the stack we are in a position to fix.
if(MSVC)
    target_compile_options(InstanceRender PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(InstanceRender PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()
set_target_properties(InstanceRender PROPERTIES PREFIX "" OUTPUT_NAME "InstanceRender")
target_include_directories(InstanceRender PRIVATE
    "${NUKE_ROOT}/include" "${NUKE_ROOT}/include/include" "${NUKE_ROOT}/FnUSD/include"
    "${EMBREE_INCLUDE_DIR}" "${TBB_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Nuke 15 and 16 ship pxr headers that were built against boost.python, and no
# boost comes with Nuke.  (Nuke 17's USD vendors its own copy under
# pxr/external/boost, so it needs none of this.)  Only headers are needed - the
# plugin never calls into boost - so any boost distribution will do.
if(HAS_USD AND NEEDS_BOOST)
    if(BOOST_INCLUDE_DIR STREQUAL "")
        message(FATAL_ERROR "Nuke ${NUKE_VER}'s USD headers need boost headers - set BOOST_INCLUDE_DIR")
    endif()
    message(STATUS "boost headers: ${BOOST_INCLUDE_DIR}")
    target_include_directories(InstanceRender PRIVATE "${BOOST_INCLUDE_DIR}")
    # Foundry builds boost with the namespace renamed to foundryboost (the usual
    # -Dboost=<name> trick), so the pxr headers say boost:: but the DLLs export
    # foundryboost::.  Compiling with the same rename makes the few symbols the
    # USD headers instantiate - boost.python's converter registry - resolve
    # against the boost Nuke actually ships.  BOOST_ALL_NO_LIB switches off
    # MSVC's auto-linking, which would ask for a boost_python*.lib that no Nuke
    # has.
    target_compile_definitions(InstanceRender PRIVATE BOOST_ALL_NO_LIB boost=foundryboost)
    # Renaming the namespace with a macro also rewrites boost's own MACRO-form
    # includes (#include BOOST_USER_CONFIG -> <foundryboost/config/user.hpp>),
    # so boost has to be reachable under that name too.  A link costs nothing;
    # where the filesystem refuses one, CMake copies the tree instead.
    set(_boostns "${CMAKE_CURRENT_BINARY_DIR}/boostns")
    if(NOT EXISTS "${_boostns}/foundryboost/config.hpp")
        file(MAKE_DIRECTORY "${_boostns}")
        file(CREATE_LINK "${BOOST_INCLUDE_DIR}/boost" "${_boostns}/foundryboost" COPY_ON_ERROR SYMBOLIC)
    endif()
    target_include_directories(InstanceRender PRIVATE "${_boostns}")
    # WINDOWS ONLY, again because an import library is a Windows idea: on Linux
    # the shipped libfoundryboost_python.so is linked directly.
    if(WIN32)
        file(GLOB _fbdll "${NUKE_ROOT}/foundryboost_python*.dll")
        set(LIB_FOUNDRYBOOST "")
        foreach(_dll ${_fbdll})
            get_filename_component(_fbname "${_dll}" NAME_WE)
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_fbname}.lib")
                set(LIB_FOUNDRYBOOST "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_fbname}.lib")
            endif()
        endforeach()
        if(LIB_FOUNDRYBOOST STREQUAL "")
            message(FATAL_ERROR "no import library for Nuke ${NUKE_VER}'s boost.python - run third_party/mkimportlibs.ps1")
        endif()
        message(STATUS "boost.python import library: ${LIB_FOUNDRYBOOST}")
    else()
        file(GLOB _fbso "${NUKE_ROOT}/libfoundryboost_python*.so*")
        set(LIB_FOUNDRYBOOST "")
        if(_fbso)
            list(GET _fbso 0 LIB_FOUNDRYBOOST)
            message(STATUS "boost.python: ${LIB_FOUNDRYBOOST}")
        endif()
    endif()
    if(NOT LIB_FOUNDRYBOOST STREQUAL "")
        target_link_libraries(InstanceRender PRIVATE "${LIB_FOUNDRYBOOST}")
    endif()
endif()
target_compile_definitions(InstanceRender PRIVATE
    $<$<BOOL:${WIN32}>:NOMINMAX>
    $<$<BOOL:${WIN32}>:_USE_MATH_DEFINES>
    $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
    $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS>
    $<$<BOOL:${HAS_USDVOL}>:IR_HAS_VOLUMES=1>
    # Nuke ships an older msvcp140.dll; meaningless outside MSVC
    $<$<BOOL:${MSVC}>:_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR>
    __TBB_NO_IMPLICIT_LINKAGE=1
    IR_NUKE_MAJOR=${NUKE_MAJOR}
    IR_NUKE_MINOR=${NUKE_MINOR}
    IR_NUKE_VER=${NUKE_VER_INT}
    IR_HAVE_USD=$<IF:$<BOOL:${HAS_USD}>,1,0>
    IR_HAVE_OPTIX=$<IF:$<BOOL:${HAS_OPTIX}>,1,0>)
if(MSVC)
    target_compile_options(InstanceRender PRIVATE /W3 /wd4251 /wd4275 /wd4267 /wd4244 /wd4305 /wd4996 /MP /Zc:__cplusplus /utf-8 /EHsc /MD /bigobj)
else()
    # -Wno-deprecated: Nuke's own headers use deprecated declarations and there
    # is nothing to be done about them from here.
    target_compile_options(InstanceRender PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated -fPIC)
endif()
target_link_libraries(InstanceRender PRIVATE
    "${LIB_DDIMAGE}" "${LIB_FDKBASE}" "${LIB_TBB}" "${LIB_USDABS}" ${LIB_NDK} ${LIB_USDENGINE}
    ${LIB_PYTHON} ${USD_LIBS} "${LIB_EMBREE}" ${LIB_TBB12})
# embree4.dll sits next to the plugin, which is not on the DLL search path: delay-load it and
# pre-load it from the plugin directory (see loadSideBySideDlls in InstanceRender.cpp)
# On Linux the loader does this itself: RPATH $ORIGIN makes the plugin look for
# its neighbours in its own directory, which is what the delay-load plus the
# hand preload in InstanceRender.cpp achieve on Windows.
if(WIN32)
    target_link_options(InstanceRender PRIVATE "/DELAYLOAD:embree4.dll")
    if(HAS_USD)
        target_link_options(InstanceRender PRIVATE "/DELAYLOAD:usd_hio.dll")
    endif()
    target_link_libraries(InstanceRender PRIVATE delayimp)
else()
    set_target_properties(InstanceRender PROPERTIES
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../:${NUKE_ROOT}:${NUKE_ROOT}/FnUSD/lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()
if(HAS_OPTIX)
    target_include_directories(InstanceRender PRIVATE "${OPTIX_ROOT}/include" "${CUDA_TOOLKIT_ROOT}/include")
    target_link_libraries(InstanceRender PRIVATE "${LIB_CUDA}" "${LIB_CUDART}")
endif()

# Embree's DLLs must sit next to the plugin (or on PATH).  tbb12 goes with it:
# Embree 4 needs oneTBB and only Nuke 17 ships one.
#
# WINDOWS ONLY. On Linux the RPATH set above finds the .so files where they are,
# so nothing has to be copied and there is no equivalent step.
if(WIN32)
set(IR_RUNTIME_DLLS "${EMBREE_ROOT}/bin/embree4.dll")
if(EXISTS "${EMBREE_ROOT}/bin/tbb12.dll")
    list(APPEND IR_RUNTIME_DLLS "${EMBREE_ROOT}/bin/tbb12.dll")
endif()
# the CUDA runtime too: only a machine with the toolkit installed has it on PATH,
# and the plugin loads it from its own folder (see SideBySideLoader)
if(HAS_OPTIX AND EXISTS "${CUDA_TOOLKIT_ROOT}/bin/cudart64_12.dll")
    list(APPEND IR_RUNTIME_DLLS "${CUDA_TOOLKIT_ROOT}/bin/cudart64_12.dll")
endif()
foreach(_dll ${IR_RUNTIME_DLLS})
    get_filename_component(_dllname "${_dll}" NAME)
    add_custom_command(TARGET InstanceRender POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:InstanceRender>/${_dllname}"
        COMMENT "copy ${_dllname} next to the plugin")
endforeach()
endif()

# per MINOR version: the NDK is not compatible across 16.0 and 16.1 either
install(TARGETS InstanceRender DESTINATION InstanceRender/nuke${NUKE_VER})
# the .pdb travels with the .dll or the freeze reports cannot name our frames.
# Both of these are Windows-shaped: there is no .pdb elsewhere, and the runtime
# list is empty because the RPATH finds the .so files where they already are.
if(MSVC)
    install(FILES $<TARGET_PDB_FILE:InstanceRender> DESTINATION InstanceRender/nuke${NUKE_VER} OPTIONAL)
endif()
if(IR_RUNTIME_DLLS)
    install(FILES ${IR_RUNTIME_DLLS} DESTINATION InstanceRender/nuke${NUKE_VER})
endif()
install(FILES nuke/init.py nuke/menu.py nuke/hydra_launch.bat nuke/hydra_debug.bat DESTINATION InstanceRender)
# the licence travels with the plugin, not just with the repository
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION InstanceRender)
# the node and menu icons: one folder, not a copy per Nuke version
install(DIRECTORY nuke/icons/ DESTINATION InstanceRender/icons FILES_MATCHING PATTERN "*.png")
# Nuke scans <prefix>/ToolSets itself, so the AOV recipe shows up under
# ToolSets > InstanceRender without any menu.py work
install(FILES nuke/ToolSets/InstanceRender_AOVs.nk DESTINATION ToolSets/InstanceRender)
# add the idempotent nuke.pluginAddPath('./InstanceRender') line to ~/.nuke/init.py
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_user_init.cmake")

# ---- the Hydra render delegate (Nuke 17.1's Hydra 2.0) --------------------------
# Nuke finds renderers through USD's own plugin registry, so this is an ordinary
# HdRendererPlugin: a library plus a plugInfo.json on PXR_PLUGINPATH_NAME.  It
# needs the same pxr the host runs, which is Nuke's own.
option(IR_WITH_HYDRA "Build the Hydra render delegate (needs Nuke 17.0+)" ON)
set(HAS_HYDRA FALSE)
if(IR_WITH_HYDRA AND HAS_USD AND NUKE_VER_INT GREATER_EQUAL 1700
   AND EXISTS "${NUKE_ROOT}/FnUSD/include/pxr/imaging/hd/rendererPlugin.h")
    set(HAS_HYDRA TRUE)
endif()

if(HAS_HYDRA)
    set(HYDRA_LIB_NAMES usd_hd usd_hf usd_hgi usd_cameraUtil usd_pxOsd)
    set(HYDRA_LIBS)
    foreach(_lib ${HYDRA_LIB_NAMES})
        find_library(_hydra_${_lib}_${NUKE_VER_INT} ${_lib} PATHS "${NUKE_ROOT}/FnUSD/lib" NO_DEFAULT_PATH)
        if(_hydra_${_lib}_${NUKE_VER_INT})
            list(APPEND HYDRA_LIBS ${_hydra_${_lib}_${NUKE_VER_INT}})
        endif()
    endforeach()

    set(IR_HYDRA_SOURCES
        src/hydra/HdInstanceRender.cpp
        src/hydra/HdIrMesh.cpp
        src/hydra/HdIrInstancer.cpp
        src/hydra/HdIrLight.cpp
        src/hydra/HdIrPoints.cpp
        src/hydra/HdIrVolume.cpp
        src/ir/Texture.cpp
        src/ir/EmbreeBackend.cpp
        # a texture wired from a Nuke node is the node: reading it needs DDImage,
        # which is already in the process - the delegate only ever runs in Nuke
        src/ir/NukeOpImage.cpp
        # GeoLoader marks watchdog phases, so the delegate needs the watchdog too
        src/ir/Watchdog.cpp
        src/ir/GeoLoader.cpp)
    if(HAS_OPTIX)
        # the same GPU back-end the node uses, and the same embedded PTX
        list(APPEND IR_HYDRA_SOURCES src/ir/OptixBackend.cpp src/ir/Denoise.cpp "${IR_PTX_CPP}")
    endif()
    add_library(hdInstanceRender MODULE ${IR_HYDRA_SOURCES})
    set_target_properties(hdInstanceRender PROPERTIES PREFIX "" OUTPUT_NAME "hdInstanceRender")
    target_include_directories(hdInstanceRender PRIVATE
        "${NUKE_ROOT}/include" "${NUKE_ROOT}/include/include" "${NUKE_ROOT}/FnUSD/include"
        "${TBB_INCLUDE_DIR}" "${EMBREE_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/hydra")
    target_compile_definitions(hdInstanceRender PRIVATE
        $<$<BOOL:${WIN32}>:NOMINMAX>
        $<$<BOOL:${WIN32}>:_USE_MATH_DEFINES>
        $<$<BOOL:${WIN32}>:WIN32_LEAN_AND_MEAN>
        $<$<BOOL:${WIN32}>:_CRT_SECURE_NO_WARNINGS>
        $<$<BOOL:${MSVC}>:_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR>
        __TBB_NO_IMPLICIT_LINKAGE=1
        IR_NUKE_VER=${NUKE_VER_INT}
        IR_HAVE_OPTIX=$<IF:$<BOOL:${HAS_OPTIX}>,1,0>)
    if(HAS_OPTIX)
        target_include_directories(hdInstanceRender PRIVATE "${OPTIX_ROOT}/include" "${CUDA_TOOLKIT_ROOT}/include")
        target_link_libraries(hdInstanceRender PRIVATE "${LIB_CUDA}" "${LIB_CUDART}")
    endif()
    if(MSVC)
        target_compile_options(hdInstanceRender PRIVATE /W3 /wd4251 /wd4275 /wd4267 /wd4244 /wd4305 /wd4996
                                                       /MP /Zc:__cplusplus /utf-8 /EHsc /MD /bigobj)
    else()
        target_compile_options(hdInstanceRender PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated -fPIC)
    endif()
    target_link_libraries(hdInstanceRender PRIVATE ${USD_LIBS} ${HYDRA_LIBS} ${LIB_PYTHON} "${LIB_TBB}" "${LIB_EMBREE}"
                                                   "${LIB_DDIMAGE}")
    if(WIN32)
        # embree4.dll sits next to the plugin, not next to the delegate
        target_link_options(hdInstanceRender PRIVATE "/DELAYLOAD:embree4.dll" "/DELAYLOAD:usd_hio.dll")
        target_link_libraries(hdInstanceRender PRIVATE delayimp)
        # TF_REGISTRY_FUNCTION puts its entry in a .pxrctor section that nothing
        # references, and Release-mode /OPT:REF throws it away - so the library loads
        # but never registers its type, and Nuke lists the renderer while refusing to
        # create it ("Render interface is not available for the selected renderer").
        #
        # UNTESTED ON LINUX: the equivalent there is --no-gc-sections, but ELF and
        # PE discard unreferenced sections by different rules, and this project
        # registers from a plain static initialiser anyway (see the note in
        # HdInstanceRender.cpp). Left alone rather than guessed at.
        target_link_options(hdInstanceRender PRIVATE "/OPT:NOREF" "/OPT:NOICF")
    else()
        set_target_properties(hdInstanceRender PROPERTIES
            INSTALL_RPATH "$ORIGIN:$ORIGIN/..:${NUKE_ROOT}:${NUKE_ROOT}/FnUSD/lib"
            BUILD_WITH_INSTALL_RPATH TRUE)
    endif()
    if(NEEDS_BOOST)
        target_include_directories(hdInstanceRender PRIVATE "${BOOST_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/boostns")
        target_compile_definitions(hdInstanceRender PRIVATE BOOST_ALL_NO_LIB boost=foundryboost)
        target_link_libraries(hdInstanceRender PRIVATE "${LIB_FOUNDRYBOOST}")
    endif()

    # the layout USD's plugin registry expects: <dir>/hdInstanceRender.dll and
    # <dir>/hdInstanceRender/resources/plugInfo.json
    # plugInfo.json's LibraryPath names the file, so the copy has to keep the
    # platform's own suffix - .dll on Windows, .so on Linux.
    add_custom_command(TARGET hdInstanceRender POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hdInstanceRender>/hydra/hdInstanceRender/resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:hdInstanceRender>" "$<TARGET_FILE_DIR:hdInstanceRender>/hydra/$<TARGET_FILE_NAME:hdInstanceRender>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/hydra/plugInfo.json" "$<TARGET_FILE_DIR:hdInstanceRender>/hydra/hdInstanceRender/resources/plugInfo.json"
        COMMENT "laying out the Hydra delegate for USD's plugin registry" VERBATIM)
    if(WIN32)
        add_custom_command(TARGET hdInstanceRender POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${EMBREE_ROOT}/bin/embree4.dll" "$<TARGET_FILE_DIR:hdInstanceRender>/hydra/embree4.dll"
            COMMENT "embree beside the delegate" VERBATIM)
    endif()

    install(TARGETS hdInstanceRender DESTINATION InstanceRender/nuke${NUKE_VER}/hydra)
    if(IR_RUNTIME_DLLS)
        install(FILES ${IR_RUNTIME_DLLS} DESTINATION InstanceRender/nuke${NUKE_VER}/hydra)
    endif()
    install(FILES src/hydra/plugInfo.json DESTINATION InstanceRender/nuke${NUKE_VER}/hydra/hdInstanceRender/resources)
    message(STATUS "Hydra delegate: on")
else()
    message(STATUS "Hydra delegate: off")
endif()

# ---------------------------------------------------------------------------
# A standalone cost guard for the particle matcher.  It needs nothing from Nuke
# - MatchInstances.h is header-only - so it builds and runs anywhere, and it is
# not installed.  See test/match_scale_test.cpp for what it holds and why it
# counts cells rather than seconds.
#
# The test suite is not shipped with the source, so these targets only exist
# when it is present. Without the guard a clone of this repository fails at
# CONFIGURE time on a missing source file, which is a poor first impression.
option(IR_BUILD_TESTS "Build the standalone C++ tests (needs the unshipped test/ tree)" ON)
if(IR_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/match_scale_test.cpp")
    add_executable(match_scale_test test/match_scale_test.cpp)
    set_target_properties(match_scale_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    enable_testing()
    add_test(NAME match_scale COMMAND match_scale_test)

    # Holds ir::worldToPixelMatrix (what the viewer draws handles through) to
    # ir::projectToPixel (what the renderer projects with).  Header-only too.
    add_executable(project_test test/project_test.cpp)
    set_target_properties(project_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME project COMMAND project_test)

    # Pins the cryptomatte hash, its float conversion and the manifest hex
    # against published vectors.  Header-only, so no Nuke needed.
    add_executable(crypto_hash_test test/crypto_hash_test.cpp)
    set_target_properties(crypto_hash_test PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_test(NAME crypto_hash COMMAND crypto_hash_test)
endif()
