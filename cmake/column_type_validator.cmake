# Helper utilities for MODEL_COLUMN_TYPE validation in simfil.
#
# Why this file exists:
# - We want the validator integration to stay readable in CMakeLists.txt.
# - We want a single implementation for file discovery rules.
# - We want deterministic behavior without GLOB/GLOB_RECURSE.
#
# Design constraints:
# - Discover files from target metadata (SOURCES and HEADER_SETs).
# - Ignore generator expressions because they are not concrete file paths.
# - Normalize to absolute paths so the Python validator has stable input.
# - Filter to C/C++ translation units and headers only.
#
# Additional consumer goal:
# - Reuse the same target-discovery logic in projects that *link* simfil,
#   so they don't have to duplicate this machinery in their own repository.

# Collect all source/header files that belong to `target` and are suitable
# inputs for the column type validator script.
#
# Args:
#   target  - CMake target name to inspect.
#   out_var - Variable name (in parent scope) receiving discovered files.
function(simfil_collect_column_validation_files_for_target target out_var)
  # Raw target file list prior to normalization and filtering.
  set(_raw_files)

  # Classic target sources added via add_library/add_executable/target_sources.
  get_target_property(_sources ${target} SOURCES)
  if (_sources AND NOT _sources STREQUAL "NOTFOUND")
    list(APPEND _raw_files ${_sources})
  endif()

  # Files contributed through CMake file sets (especially modern header sets).
  get_target_property(_header_sets ${target} HEADER_SETS)
  if (_header_sets AND NOT _header_sets STREQUAL "NOTFOUND")
    foreach(_set IN LISTS _header_sets)
      get_target_property(_set_files ${target} "HEADER_SET_${_set}")
      if (_set_files AND NOT _set_files STREQUAL "NOTFOUND")
        list(APPEND _raw_files ${_set_files})
      endif()
    endforeach()
  endif()

  # Final validated file list.
  set(_files)

  # Resolve relative paths against the target's source directory when possible.
  get_target_property(_target_source_dir ${target} SOURCE_DIR)
  if (NOT _target_source_dir OR _target_source_dir STREQUAL "NOTFOUND")
    set(_target_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  foreach(_file IN LISTS _raw_files)
    # Generator expressions (e.g. $<...>) are configuration-time expressions,
    # not concrete file paths. The validator script expects concrete files.
    if (_file MATCHES "^\\$<")
      continue()
    endif()

    # Normalize to absolute paths so downstream tooling gets deterministic input.
    if (NOT IS_ABSOLUTE "${_file}")
      get_filename_component(_abs "${_file}" ABSOLUTE BASE_DIR "${_target_source_dir}")
    else()
      set(_abs "${_file}")
    endif()

    # Ignore virtual or stale entries that do not map to a file on disk.
    if (NOT EXISTS "${_abs}")
      continue()
    endif()

    # Keep validator input constrained to C/C++ source and header files.
    get_filename_component(_ext "${_abs}" EXT)
    string(TOLOWER "${_ext}" _ext)
    if (_ext STREQUAL ".h" OR _ext STREQUAL ".hh" OR _ext STREQUAL ".hpp" OR
        _ext STREQUAL ".hxx" OR _ext STREQUAL ".c" OR _ext STREQUAL ".cc" OR
        _ext STREQUAL ".cpp" OR _ext STREQUAL ".cxx")
      list(APPEND _files "${_abs}")
    endif()
  endforeach()

  list(REMOVE_DUPLICATES _files)

  # Return results to caller.
  set(${out_var} "${_files}" PARENT_SCOPE)
endfunction()

# Enable MODEL_COLUMN_TYPE validation for a specific target.
#
# This sets up:
# - Python interpreter discovery
# - file discovery for the given target
# - a dedicated custom target that runs the validator script
# - build dependency from the real build target to the validator target
#
# The explicit dependency ensures schema safety checks run as part of regular
# builds, not only during configure.
function(simfil_enable_column_type_validation target validator_script)
  # Keep failure mode explicit and user-readable when script location is wrong.
  if (NOT EXISTS "${validator_script}")
    message(WARNING
      "SIMFIL_VALIDATE_MODEL_COLUMNS is ON, but validator script is missing: ${validator_script}")
    return()
  endif()

  # Validation is script-based; Python is required.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  # Gather all relevant files from the selected target.
  simfil_collect_column_validation_files_for_target("${target}" _simfil_validation_files)
  if (NOT _simfil_validation_files)
    message(WARNING
      "SIMFIL_VALIDATE_MODEL_COLUMNS is ON, but no files were discovered for target `${target}`.")
    return()
  endif()

  # Use a dedicated target so users can run validation directly and so build
  # graph dependencies stay explicit.
  add_custom_target(simfil-column-type-validation
    COMMAND "${Python3_EXECUTABLE}"
            "${validator_script}"
            ${_simfil_validation_files}
    VERBATIM)

  # Ensure validation runs before compiling/linking the target.
  add_dependencies("${target}" simfil-column-type-validation)
endfunction()

# Recursively collect all buildsystem targets under `dir`.
#
# This is useful for consumer projects (e.g. mapget) that want to discover
# "all local targets that link simfil", then wire validation automatically.
#
# Args:
#   dir     - project directory to inspect recursively
#   out_var - parent-scope variable receiving discovered target names
function(simfil_collect_directory_targets dir out_var)
  # Targets declared directly in this directory.
  get_property(_dir_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  set(_targets ${_dir_targets})

  # Recurse into child directories to gather the full project-local target set.
  get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_subdir IN LISTS _subdirs)
    simfil_collect_directory_targets("${_subdir}" _sub_targets)
    list(APPEND _targets ${_sub_targets})
  endforeach()

  list(REMOVE_DUPLICATES _targets)
  # Return accumulated targets to caller.
  set(${out_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Determine whether `target` links a given library target name.
#
# We check both direct and interface link properties so this works for
# normal and propagated link relationships.
#
# Args:
#   target    - target to inspect
#   library   - link library token to search for (e.g. "simfil::simfil")
#   out_var   - parent-scope ON/OFF result
function(simfil_target_links_library target library out_var)
  # Merge link properties into one list for matching.
  set(_all_link_libraries)

  get_target_property(_link_libraries ${target} LINK_LIBRARIES)
  if (_link_libraries AND NOT _link_libraries STREQUAL "NOTFOUND")
    list(APPEND _all_link_libraries ${_link_libraries})
  endif()

  get_target_property(_interface_link_libraries ${target} INTERFACE_LINK_LIBRARIES)
  if (_interface_link_libraries AND NOT _interface_link_libraries STREQUAL "NOTFOUND")
    list(APPEND _all_link_libraries ${_interface_link_libraries})
  endif()

  # Match either exact token or substring (for generator-expression-heavy items).
  set(_links_library OFF)
  foreach(_lib IN LISTS _all_link_libraries)
    if (_lib STREQUAL "${library}")
      set(_links_library ON)
      break()
    endif()
    string(FIND "${_lib}" "${library}" _library_index)
    if (NOT _library_index EQUAL -1)
      set(_links_library ON)
      break()
    endif()
  endforeach()

  # Return bool-like result to caller.
  set(${out_var} "${_links_library}" PARENT_SCOPE)
endfunction()

# Enable MODEL_COLUMN_TYPE validation for all non-imported targets in a project
# that link a given library (typically `simfil::simfil`).
#
# This is the consumer-facing "automatic linked-target detection" entrypoint.
#
# Args:
#   project_root           - root directory where local targets should be discovered
#   link_library           - link token used to select relevant targets
#   validator_script       - absolute/relative path to column_type_validator.py
#   validation_target_name - custom target name to create (e.g. mapget-column-type-validation)
function(simfil_enable_column_type_validation_for_linked_targets
    project_root
    link_library
    validator_script
    validation_target_name)
  # Keep failure mode explicit when script path changes or dependency layout differs.
  if (NOT EXISTS "${validator_script}")
    message(WARNING
      "Column type validation is enabled, but validator script is missing: ${validator_script}")
    return()
  endif()

  # Validation runs as an external script.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  # `_validation_files` feeds the Python validator.
  # `_validation_targets` are local targets that must depend on validation.
  set(_validation_files)
  set(_validation_targets)

  # Also validate simfil's own files if the source target is available in this build.
  # This keeps shared tagged types covered even when invoked from consumers.
  if (TARGET simfil)
    simfil_collect_column_validation_files_for_target(simfil _simfil_validation_files)
    list(APPEND _validation_files ${_simfil_validation_files})
  endif()

  # Discover all local targets and keep only those that link `link_library`.
  simfil_collect_directory_targets("${project_root}" _all_targets)
  foreach(_target IN LISTS _all_targets)
    # Imported targets are external dependency artifacts and must not be wired.
    get_target_property(_is_imported ${_target} IMPORTED)
    if (_is_imported)
      continue()
    endif()

    simfil_target_links_library("${_target}" "${link_library}" _links_library)
    if (_links_library)
      list(APPEND _validation_targets "${_target}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _validation_targets)

  # Gather and de-duplicate all validator input files from selected targets.
  foreach(_target IN LISTS _validation_targets)
    simfil_collect_column_validation_files_for_target("${_target}" _target_validation_files)
    list(APPEND _validation_files ${_target_validation_files})
  endforeach()
  list(REMOVE_DUPLICATES _validation_files)

  # Keep behavior explicit if no files were found.
  if (NOT _validation_files)
    message(WARNING
      "Column type validation is enabled, but no files were discovered for validation.")
    return()
  endif()

  # Create one centralized validator target.
  add_custom_target("${validation_target_name}"
    COMMAND "${Python3_EXECUTABLE}"
            "${validator_script}"
            ${_validation_files}
    VERBATIM)

  # Ensure validation runs before each selected build target.
  foreach(_target IN LISTS _validation_targets)
    add_dependencies("${_target}" "${validation_target_name}")
  endforeach()
endfunction()
