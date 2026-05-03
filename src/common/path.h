/*
 * Path:: namespace collapsed to path_ prefix.  std::string returns become
 * char* (caller frees with free()) or are written into caller-supplied
 * small_string_t* buffers.  std::string_view args become (data, len) pairs.
 * std::vector<std::string_view> split/join are dropped here - callers walk
 * paths in-line; we only export Combine/BuildRelativePath/Canonicalize/
 * MakeRelative/GetExtension/StripExtension/GetFileName/GetDirectory/
 * RemoveLength/ChangeExtension/IsAbsolute/ToNativePath that the C port
 * actually needs.
 *
 * Native separator on Linux is '/'.  ToNativePath is effectively a slash
 * normaliser (collapses runs of '/' in source, strips trailing slashes).
 */

#ifndef CUPID_COMMON_PATH_H
#define CUPID_COMMON_PATH_H

#include "small_string.h"
#include "types.h"

#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"

/* Returns true if path is absolute (starts with '/'). */
bool path_is_absolute_view(const char* data, u32 len);
bool path_is_absolute_cstr(const char* path);

 /* Linux: normalise (collapse repeated '/', strip trailing).  Writes to out
 * (which is reset).  Allocates if needed. */
void path_to_native_view(small_string_t* out, const char* data, u32 len);
void path_to_native_cstr(small_string_t* out, const char* path);

/* Build relative path: take directory of `filename`, append `new_filename`. */
void path_build_relative_view(small_string_t* out,
                              const char* filename_data, u32 filename_len,
                              const char* new_filename_data, u32 new_filename_len);
void path_build_relative_cstr(small_string_t* out, const char* filename, const char* new_filename);

/* Join two/three components, normalising separators, into out. */
void path_combine_view2(small_string_t* out,
                        const char* base_data, u32 base_len,
                        const char* next_data, u32 next_len);
void path_combine_view3(small_string_t* out,
                        const char* base_data, u32 base_len,
                        const char* subdir_data, u32 subdir_len,
                        const char* next_data, u32 next_len);
void path_combine_cstr2(small_string_t* out, const char* base, const char* next);
void path_combine_cstr3(small_string_t* out, const char* base, const char* subdir, const char* next);

/* Canonicalize: collapse "." and ".." components.  Operates on a copy. */
void path_canonicalize_view   (small_string_t* out, const char* data, u32 len);
void path_canonicalize_inplace(small_string_t* path);

/* Make path relative to relative_to.  Both must be absolute or input is
 * returned verbatim. */
void path_make_relative_view(small_string_t* out,
                             const char* path_data, u32 path_len,
                             const char* rel_to_data, u32 rel_to_len);

void path_get_extension_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len);
void path_get_extension_cstr(const char* path, const char** out_data, u32* out_len);

void path_strip_extension_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len);
void path_strip_extension_cstr(const char* path, const char** out_data, u32* out_len);

/* Returns a view of the basename (everything after last '/').  No alloc. */
void path_get_file_name_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len);
void path_get_file_name_cstr(const char* path, const char** out_data, u32* out_len);

/* Returns a view of the directory portion (everything before last '/').
 * Empty when no separator present. */
void path_get_directory_view(const char* path_data, u32 path_len, const char** out_data, u32* out_len);
void path_get_directory_cstr(const char* path, const char** out_data, u32* out_len);

/* Replace extension on path.  If path has no '.', appends ".new_ext". */
void path_change_extension_view(small_string_t* out,
                                const char* path_data, u32 path_len,
                                const char* new_ext_data, u32 new_ext_len);
void path_change_extension_cstr(small_string_t* out, const char* path, const char* new_ext);

void path_remove_length_limits_view(small_string_t* out, const char* data, u32 len);

#endif /* CUPID_COMMON_PATH_H */
