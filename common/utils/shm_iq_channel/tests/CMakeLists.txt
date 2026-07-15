# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_executable(test_shm_td_iq_channel test_shm_td_iq_channel.c)
target_link_libraries(test_shm_td_iq_channel shm_td_iq_channel minimal_lib pthread)
# Upstream attaches this to the top-level `tests` aggregate target, which does not
# exist on this branch; guard so it works both with and without that target.
if(TARGET tests)
  add_dependencies(tests test_shm_td_iq_channel)
endif()
# Commented out due to issues with ASAN in the unit test docker container
#add_test(test_shm_td_iq_channel ./test_shm_td_iq_channel)
