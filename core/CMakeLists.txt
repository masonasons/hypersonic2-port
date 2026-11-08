file(GLOB HS2_FX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/fx/*.cpp
                             ${CMAKE_CURRENT_SOURCE_DIR}/src/types/*.cpp)
add_library(hs2core STATIC
  ${HS2_FX_SOURCES}
  src/bank.cpp
  src/patch.cpp
  src/samples.cpp
  src/content.cpp
  src/element.cpp
  src/voice.cpp
  src/synth.cpp
  src/engine.cpp
  src/combi.cpp
  src/effects.cpp
  src/arp.cpp
)
target_include_directories(hs2core PUBLIC include)
if(MSVC)
  target_compile_options(hs2core PRIVATE /W4 /fp:precise)
else()
  target_compile_options(hs2core PRIVATE -Wall -Wextra -Wno-unused-parameter -ffp-contract=off)
endif()
