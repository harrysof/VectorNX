/* vector_patches.h -- game-specific IL2CPP patches for Vector.
 *
 * All patches are applied after the modules are loaded and finalized, and are
 * self-verifying: each site checks the expected instruction word first. A
 * mismatch logs and skips, leaving the original code untouched.
 */
#ifndef VECTOR_PATCHES_H
#define VECTOR_PATCHES_H

#include <stdint.h>

/* Apply the Vector boot patches. Returns the number of patches applied.
 * unity_base / il2cpp_base are the modules' load_virtbase values.  string_new
 * is the resolved il2cpp_string_new export, or NULL when unavailable. */
int vector_install_patches(uintptr_t unity_base, uintptr_t il2cpp_base,
                           void *string_new);

#endif /* VECTOR_PATCHES_H */
