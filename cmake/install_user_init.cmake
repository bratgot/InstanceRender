# Idempotently register the InstanceRender plugin folder in the user's
# ~/.nuke/init.py (CMAKE_INSTALL_PREFIX is expected to be ~/.nuke).
#
# The registration lives in init.py (not menu.py) so it also works in terminal
# (-t) sessions; the plugin folder's own init.py then adds the build matching
# the running Nuke version, and its menu.py adds the toolbar entry.

set(_marker_begin "# --- InstanceRender (auto-added by cmake --install) ---")
set(_marker_end   "# --- end InstanceRender ---")
set(_block "${_marker_begin}\nimport nuke\nnuke.pluginAddPath('./InstanceRender')\n${_marker_end}\n")

set(_path "${CMAKE_INSTALL_PREFIX}/init.py")
set(_existing "")
if(EXISTS "${_path}")
    file(READ "${_path}" _existing)
endif()
string(FIND "${_existing}" "${_marker_begin}" _pos)
if(_pos EQUAL -1)
    if(NOT _existing STREQUAL "" AND NOT _existing MATCHES "\n$")
        set(_existing "${_existing}\n")
    endif()
    file(WRITE "${_path}" "${_existing}${_block}")
    message(STATUS "InstanceRender: registered plugin path in ${_path}")
else()
    message(STATUS "InstanceRender: ${_path} already registers the plugin path (skipped)")
endif()
