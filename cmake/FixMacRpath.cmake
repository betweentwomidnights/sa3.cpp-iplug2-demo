# The sa3/ggml dylibs are built in sa3.cpp's build tree, so they carry that tree's absolute rpaths
# and reference each other as @rpath/libggml*.dylib. Copying them beside the binary is not enough:
# while a build-tree rpath remains, a bundle can load from the build tree on the build machine and
# look fine, then fail on any other machine. Leave @loader_path as each copied dylib's only rpath,
# then re-sign (install_name_tool invalidates the signature, and arm64 refuses to load an unsigned
# dylib).
function(sa3_edit_rpath action rpath lib)
  execute_process(COMMAND install_name_tool ${action} "${rpath}" "${lib}"
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err OUTPUT_QUIET)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_name_tool ${action} ${rpath} failed for ${lib}: ${_err}")
  endif()
endfunction()

file(GLOB _sa3_dylibs "${BUNDLE_DIR}/*.dylib")
foreach(_lib ${_sa3_dylibs})
  execute_process(COMMAND otool -l "${_lib}" OUTPUT_VARIABLE _load_cmds ERROR_QUIET)
  string(REGEX MATCHALL "cmd LC_RPATH\n[^\n]*\n *path [^\n]+ \\(offset" _rpath_cmds "${_load_cmds}")
  set(_has_loader_path FALSE)
  foreach(_rpath_cmd ${_rpath_cmds})
    string(REGEX REPLACE ".*\n *path (.+) \\(offset$" "\\1" _rpath "${_rpath_cmd}")
    if(_rpath STREQUAL "@loader_path")
      set(_has_loader_path TRUE)
    else()
      sa3_edit_rpath(-delete_rpath "${_rpath}" "${_lib}")
    endif()
  endforeach()
  if(NOT _has_loader_path)
    sa3_edit_rpath(-add_rpath "@loader_path" "${_lib}")
  endif()
  execute_process(COMMAND codesign --force --sign - "${_lib}" ERROR_QUIET OUTPUT_QUIET)
endforeach()
