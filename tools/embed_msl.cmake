# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Wrap a Metal shader source file into a C++ raw-string constant so it can be
# embedded into the binary at build time (no runtime filesystem dependency).
#
# Invoked per shader via add_custom_command:
#   cmake -DSRC=<in.metal> -DDST=<out.inc> -DVAR=<CONSTANT_NAME> -P embed_msl.cmake
#
# The generated .inc is meant to be #included inside an anonymous namespace:
#   namespace { #include "embedded_shaders/foo.metal.inc" }
#
file(READ "${SRC}" _content)

# The raw-string delimiter must not appear in the source, or the literal would
# terminate early. MSL never contains this token, but guard anyway.
if(_content MATCHES "\\)MSLEMBED\"")
    message(FATAL_ERROR "embed_msl: '${SRC}' contains the raw-string delimiter )MSLEMBED\"")
endif()

get_filename_component(_srcName "${SRC}" NAME)

file(WRITE "${DST}"
"// AUTO-GENERATED from ${_srcName} by tools/embed_msl.cmake — DO NOT EDIT.\n"
"constexpr const char* ${VAR} = R\"MSLEMBED(\n"
"${_content}"
")MSLEMBED\";\n")
