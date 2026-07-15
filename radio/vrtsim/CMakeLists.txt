# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# develop-native vrtsim build, with the taps client and unit tests re-enabled.
# Reconciled to this branch's structure, NOT a verbatim copy of develop:
#   * channel_pipeline + noise_device are built here as a local static lib because this branch
#     has no openair1/SIMULATION/TOOLS/CMakeLists.txt.
#   * Link lists use this branch's target names (UTIL provides the thread-pool; there are no
#     separate noise_device / log_headers / thread-pool targets as on develop).
#   * taps client (OAI_VRTSIM_TAPS_CLIENT) and tests (ENABLE_TESTS) match develop's wiring.

# channel_pipeline + noise_device vendored from upstream openair1/SIMULATION/TOOLS.
add_library(channel_pipeline STATIC
  ${OPENAIR1_DIR}/SIMULATION/TOOLS/channel_pipeline.c
  ${OPENAIR1_DIR}/SIMULATION/TOOLS/noise_device.c
)
target_include_directories(channel_pipeline
  PUBLIC ${OPENAIR1_DIR}/SIMULATION/TOOLS ${OPENAIR_DIR} ${OPENAIR1_DIR} ${OPENAIR_DIR}/common/utils/threadPool)
# SIMU provides channelmod; UTIL provides the thread-pool used by channel_pipeline.c.
target_link_libraries(channel_pipeline PRIVATE SIMU UTIL)

# vrtsim shared-memory radio module.
add_library(vrtsim MODULE
  vrtsim.c
  cirdb_provider.c
  cirdb_yaml.cpp
)
set_target_properties(vrtsim PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(yaml-cpp REQUIRED)

target_link_libraries(vrtsim
  PRIVATE
    SIMU
    UTIL
    shm_td_iq_channel
    channel_pipeline
    yaml-cpp
)

if (ENABLE_CHANNEL_SIM_CUDA)
  target_compile_definitions(vrtsim PRIVATE CHANNEL_SIM_CUDA)
endif()

# Optional taps client (develop-native): network-delivered precomputed channel taps.
# The trailing ON adds -DOAI_VRTSIM_TAPS_CLIENT globally so vrtsim.c's #ifdef guards compile in.
add_boolean_option(OAI_VRTSIM_TAPS_CLIENT OFF "Enable taps client" ON)
if (OAI_VRTSIM_TAPS_CLIENT)
  target_link_libraries(vrtsim PRIVATE taps_client)

  file(DOWNLOAD
    "https://gitlab.eurecom.fr/oai/raytracing-channel-emulator/-/raw/59589ad2a534881810b2aa7dd81e63066245bb8d/server/api/taps.fbs?inline=false"
    ${CMAKE_SOURCE_DIR}/taps.fbs
  )

  set(TAPS_IF_PATH ${CMAKE_SOURCE_DIR}/taps.fbs)
  set(TAPS_API_HEADER taps_generated.h)

  add_custom_command(OUTPUT ${TAPS_API_HEADER}
    COMMAND flatc -c ${TAPS_IF_PATH}
    DEPENDS ${TAPS_IF_PATH}
    COMMENT "Generating flatbuffers API from ${TAPS_IF_PATH}"
  )

  add_library(taps_api INTERFACE)
  target_include_directories(taps_api INTERFACE ${CMAKE_CURRENT_BINARY_DIR})

  add_library(taps_client taps_client.cpp ${TAPS_API_HEADER})
  target_include_directories(taps_client PUBLIC .)
  # This branch is globally -std=c++11; taps_client.cpp (develop, C++17) uses std::make_unique etc.
  # Append -std=c++17 for just this target so gcc's last -std wins, without touching the rest of the
  # C++11-locked branch. (Goal-1; develop builds everything at CMAKE_CXX_STANDARD 17.)
  target_compile_options(taps_client PRIVATE -std=c++17)
  find_package(FlatBuffers QUIET)
  if(NOT FlatBuffers_FOUND)
    # Fall back to lowercase (Ubuntu 22.04)
    find_package(Flatbuffers REQUIRED)
    set(FLATBUFFERS_TARGET flatbuffers)
  else()
    set(FLATBUFFERS_TARGET flatbuffers::flatbuffers)
  endif()
  target_link_libraries(taps_client PUBLIC ${FLATBUFFERS_TARGET})
  target_link_libraries(taps_client PUBLIC flatbuffers taps_api SIMU nanomsg)
endif()

if (ENABLE_TESTS)
  add_library(vrtsim_static vrtsim.c cirdb_provider.c cirdb_yaml.cpp)
  target_link_libraries(vrtsim_static
    PUBLIC
      SIMU
      UTIL
      shm_td_iq_channel
      channel_pipeline
      yaml-cpp
  )
  if (OAI_VRTSIM_TAPS_CLIENT)
    target_link_libraries(vrtsim_static PRIVATE taps_client)
  endif()
  add_subdirectory(tests)
endif()
