# OVERLAY of the tinygltf port at vcpkg.json's builtin-baseline, identical except
# for the SHA512.
#
# GitHub regenerated the source archive for tag v2.9.7, so the baseline port's
# hash no longer matches and every machine WITHOUT tinygltf already cached fails
# to configure — a fresh CI runner first among them. The regenerated archive was
# checked file for file against a git checkout of the v2.9.7 tag (commit
# 488a70a3df62a4df1a736e9e56fb8836580c4888) and is identical: only GitHub's
# tarball changed, not the source.
#
# vcpkg never re-hashed 2.9.7; upstream moved the port to 3.0.0 instead. Delete
# this overlay when the baseline is bumped past that, which is a tinygltf major
# version and wants its own change.

# Header-only library
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO syoyo/tinygltf
    REF "v${VERSION}"
    SHA512 553c7ad329da5a4d46235747db9d937957d5698e74c8d1751c17da5a1d09d35f4212e2476652a63ee1a12ff74220531b5e288eaaddb1014d47000a82d30f03a2
    HEAD_REF master
)

# Put the licence file where vcpkg expects it
# Copy the tinygltf header files and fix the path to json
vcpkg_replace_string("${SOURCE_PATH}/tiny_gltf.h" "#include \"json.hpp\"" "#include <nlohmann/json.hpp>")
file(INSTALL "${SOURCE_PATH}/tiny_gltf.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
