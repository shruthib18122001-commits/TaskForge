# Sanitizers.cmake
#
# Produces one INTERFACE target, `taskforge_sanitize`, that carries the compile
# and link flags for TASKFORGE_SANITIZER = none | asan | tsan.
#
# Why an INTERFACE target and not CMAKE_CXX_FLAGS: the flags must land on both
# compile and link lines of every TaskForge target, and only those targets
# (not, say, a vendored GoogleTest built in the same tree -- although in
# practice we sanitize that too so it agrees with our TU's runtime).
#
# ASan config = AddressSanitizer + UBSan. They compose and share a runtime.
# TSan config = ThreadSanitizer, which cannot be combined with ASan.
#
# -fno-omit-frame-pointer + -g everywhere so reports have usable stacks.
# -O1 for sanitized builds: enough inlining to keep them bearable, not so much
# that stacks turn to soup.

add_library(taskforge_sanitize INTERFACE)

set(_tf_san "${TASKFORGE_SANITIZER}")

if(_tf_san STREQUAL "none")
  # nothing

elseif(_tf_san STREQUAL "asan")
  if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    message(FATAL_ERROR "ASan build requires GCC or Clang")
  endif()
  set(_tf_flags -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
                -g -O1)
  target_compile_options(taskforge_sanitize INTERFACE ${_tf_flags})
  target_link_options(taskforge_sanitize INTERFACE -fsanitize=address,undefined)
  message(STATUS "Sanitizer: AddressSanitizer + UBSan")

elseif(_tf_san STREQUAL "tsan")
  if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    message(FATAL_ERROR "TSan build requires GCC or Clang")
  endif()
  set(_tf_flags -fsanitize=thread
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
                -g -O1)
  target_compile_options(taskforge_sanitize INTERFACE ${_tf_flags})
  target_link_options(taskforge_sanitize INTERFACE -fsanitize=thread)
  message(STATUS "Sanitizer: ThreadSanitizer")

else()
  message(FATAL_ERROR "Unknown TASKFORGE_SANITIZER='${_tf_san}' (want: none|asan|tsan)")
endif()

unset(_tf_san)
unset(_tf_flags)
