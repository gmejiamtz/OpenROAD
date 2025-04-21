# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2019-2025, The OpenROAD Authors

include("openroad")

# https://github.com/The-OpenROAD-Project/OpenROAD/issues/1186
find_package(LEMON NAMES LEMON lemon REQUIRED)

add_library(cms_lib
    Clock.cpp
    TreeBuilder.cpp
    HTreeBuilder.cpp
    SinkClustering.cpp
    TechChar.cpp
    TritonCTS.cpp
    Clustering.cpp
    LevelBalancer.cpp
    CtsOptions.cpp
)

swig_lib(NAME      cms
         NAMESPACE cms
         I_FILE    TritonCTS.i
         SCRIPTS   TritonCTS.tcl
)

target_sources(cms
  PRIVATE
    MakeTritoncts.cpp
    CtsGraphics.cpp
)

target_include_directories(cms_lib
  PUBLIC
    ../include
  PRIVATE
    .
  ${LEMON_INCLUDE_DIRS}
  ${Boost_INCLUDE_DIRS}
 )

target_include_directories(cms
  PUBLIC
    ../include
  PRIVATE
    .
  ${Boost_INCLUDE_DIRS}
 )

target_link_libraries(cms_lib
  PUBLIC
    rsz_lib
  PRIVATE
    odb
    dbSta_lib
    OpenSTA
    stt_lib
    utl_lib
)

target_link_libraries(cms
  PRIVATE
    odb
    cms_lib
    dbSta_lib
    rsz_lib
    OpenSTA
    gui
 )

messages(
  TARGET cms
  OUTPUT_DIR ..
)

if (Python3_FOUND AND BUILD_PYTHON)
  swig_lib(NAME          cms_py
           NAMESPACE     cms
           LANGUAGE      python
           I_FILE        TritonCTS-py.i
           SWIG_INCLUDES ${PROJECT_SOURCE_DIR}/../include/cms
                         ${PROJECT_SOURCE_DIR}
           SCRIPTS       ${CMAKE_CURRENT_BINARY_DIR}/cms_py.py
  )

  target_include_directories(cms_py
    PUBLIC
      .
  )
  
  target_link_libraries(cms_py
    PUBLIC
      cms
      dbSta
      OpenSTA
      odb
      rsz
  )

endif()
