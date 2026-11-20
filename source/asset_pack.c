/* asset_pack.c -- read-only packed assets for VectorNX.
 *
 * WHY. The 0.5.0 hardware log's first level load took 14,052 ms with
 * open=348: 348 separate FAT directory lookups on the SD card. The story text
 * was fixed by the font RAM cache, but every level still pays the same cost
 * for level1, sharedassets, .resource files and `unity default resources`.
 *
 * A pack turns those 348 lookups into 348 binary searches in a path index held
 * in RAM, followed by reads from one already-open file. The loose tree stays
 * on the card as a fallback, so a missing or invalid pack cannot make the game
 * look broken.
 *
 * FORMAT (shared with phigros_nx's asset_pack.c, version 2):
 *
 *   assets.nxpack:
 *     PackHeader (40 bytes)
 *     payload, each entry 16-byte aligned
 *
 *   assets.nxidx:
 *     IndexHeader (56 bytes)
 *     DiskEntry[count] (24 bytes each)
 *     NUL-terminated path blob (sorted with strcmp order)
 *
 * FNV-1a 64 is used with the same non-standard offset basis as the reference
 * (1469598103934665603), so the Python builder and this reader agree.
 *
 * Unlike the reference, a packed open does NOT open the pack file again. All
 * handles are synthetic high descriptors and every read goes through the one
 * mount-time descriptor under a mutex. That removes the per-asset pack open
 * (which would still be a FAT lookup) and avoids ever exposing a real fd.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>

#include "asset_pack.h"

#define PACK_VERSION 2u
#define PACK_HANDLES 256
#define PACK_CACHE_SIZE (32u * 1024u)
#define PACK_FD_BASE 0x50000000
#define PACK_DIRECTORY_ENTRY UINT32_MAX
#define PACK_DIR_MAGIC 0x53534e5844495232ULL

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t reserved;
  uint64_t pack_id;
  uint64_t file_size;
  uint64_t data_checksum;
} PackHeader;

typedef struct {
  char magic[8];
  uint32_t version;
  uint32_t count;
  uint64_t pack_id;
  uint64_t pack_size;
  uint64_t paths_size;
  uint64_t checksum;
  uint64_t data_checksum;
} IndexHeader;

typedef struct {
  uint64_t offset;
  uint64_t size;
  uint32_t path_offset;
  uint32_t path_length;
} DiskEntry;

typedef struct {
  int used;
  uint32_t entry;
  uint64_t directory_ino;
  uint64_t position;
  uint64_t cache_offset;
  size_t cache_size;
  unsigned char *cache;
  Mutex lock;
} PackHandle;

typedef struct {
  uint64_t magic;
  size_t cursor;
  unsigned dots;
  char prefix[768];
  char name[256];
  char last[256];
} PackDir;

static int g_pack_fd = -1;
static DiskEntry *g_entries;
static char *g_paths;
static size_t g_entry_count;
static uint64_t g_data_size;
static PackHandle g_handles[PACK_HANDLES];
static Mutex g_handle_lock;
static Mutex g_pack_io_lock;
static int g_locks_ready;
static char g_error[192];

static void set_error(const char *message) {
  snprintf(g_error, sizeof g_error, "%s", message ? message : "Unknown error");
}

const char *asset_pack_error(void) {
  return g_error[0] ? g_error : "Asset pack is unavailable";
}

static uint64_t fnv_bytes(uint64_t hash, const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; i++) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

/* Exact read at an absolute offset, no locking. Used during mount. */
static int read_exact_fd(int fd, void *buffer, size_t size, uint64_t offset) {
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) return 0;
  size_t done = 0;
  while (done < size) {
    ssize_t got = read(fd, (char *)buffer + done, size - done);
    if (got <= 0) return 0;
    done += (size_t)got;
  }
  return 1;
}

/* All payload reads serialize on the one mount-time pack descriptor. SD reads
 * are serial anyway; this keeps every handle's position independent. */
static int read_at_pack(void *buffer, size_t size, uint64_t offset) {
  if (g_pack_fd < 0) return 0;
  mutexLock(&g_pack_io_lock);
  int ok = read_exact_fd(g_pack_fd, buffer, size, offset);
  mutexUnlock(&g_pack_io_lock);
  return ok;
}

static int valid_relative(const char *path, size_t length) {
  if (!path || !length || path[0] == '/' || path[length - 1] == '/') return 0;
  size_t part = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i == length || path[i] == '/') {
      if (!part || (part == 1 && path[i - 1] == '.') ||
          (part == 2 && path[i - 2] == '.' && path[i - 1] == '.')) return 0;
      part = 0;
    } else {
      if (path[i] == '\\' || path[i] == ':') return 0;
      part++;
    }
  }
  return 1;
}

/* Accepts every shape the loader and Unity produce:
 *   sdmc:/switch/vector/assets/bin/Data/level1
 *   /switch/vector/assets/bin/Data/level1
 *   assets/bin/Data/level1
 * and reduces it to "bin/Data/level1", matching the builder's paths. */
static int normalize_asset_path(const char *input, char *output, size_t capacity,
                                int allow_empty) {
  if (!input || !output || capacity == 0) return 0;
  char temp[768];
  size_t length = strlen(input);
  if (length >= sizeof temp) return 0;
  for (size_t i = 0; i <= length; i++) temp[i] = input[i] == '\\' ? '/' : input[i];

  const char *relative = NULL;
  if (!strcmp(temp, "assets")) relative = temp + 6;
  else if (!strncmp(temp, "assets/", 7)) relative = temp + 7;
  for (const char *p = temp; (p = strstr(p, "/assets")) != NULL; p++) {
    if (p[7] == 0) relative = p + 7;
    else if (p[7] == '/') relative = p + 8;
  }
  if (!relative) return 0;

  size_t used = 0;
  const char *p = relative;
  while (*p) {
    while (*p == '/') p++;
    const char *start = p;
    while (*p && *p != '/') p++;
    size_t part = (size_t)(p - start);
    if (!part || (part == 1 && start[0] == '.')) continue;
    if (part == 2 && start[0] == '.' && start[1] == '.') return 0;
    if (used && used + 1 >= capacity) return 0;
    if (used) output[used++] = '/';
    if (used + part >= capacity) return 0;
    memcpy(output + used, start, part);
    used += part;
  }
  output[used] = 0;
  return allow_empty || used != 0;
}

static int normalize_relative(const char *input, char *output, size_t capacity) {
  return normalize_asset_path(input, output, capacity, 0);
}

static int entry_compare_path(const char *path, size_t *result) {
  size_t low = 0, high = g_entry_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const char *candidate = g_paths + g_entries[mid].path_offset;
    int cmp = strcmp(path, candidate);
    if (cmp == 0) { *result = mid; return 1; }
    if (cmp < 0) high = mid;
    else low = mid + 1;
  }
  *result = low;
  return 0;
}

static int find_relative(const char *path, size_t *result) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (!valid_relative(path, length)) return 0;
  return entry_compare_path(path, result);
}

static int find_directory_relative(const char *path, uint64_t *ino) {
  if (!g_entries || !path) return 0;
  size_t length = strlen(path);
  if (length && !valid_relative(path, length)) return 0;
  if (!length) {
    if (!g_entry_count) return 0;
  } else {
    char prefix[768];
    if (length + 2 > sizeof prefix) return 0;
    memcpy(prefix, path, length);
    prefix[length++] = '/';
    prefix[length] = 0;
    size_t index;
    entry_compare_path(prefix, &index);
    if (index >= g_entry_count ||
        strncmp(g_paths + g_entries[index].path_offset, prefix, length)) return 0;
  }
  uint64_t hash = fnv_bytes(1469598103934665603ULL, path, strlen(path));
  if (ino) *ino = hash ? hash : 1;
  return 1;
}

static int load_pair(const char *pack_path, const char *index_path,
                     DiskEntry **entries_out, char **paths_out,
                     size_t *count_out, int *fd_out) {
  int pack_fd = -1, index_fd = -1;
  DiskEntry *entries = NULL;
  char *paths = NULL;
  struct stat pack_stat, index_stat;
  PackHeader pack_header;
  IndexHeader index_header;

  pack_fd = open(pack_path, O_RDONLY);
  index_fd = open(index_path, O_RDONLY);
  if (pack_fd < 0 || index_fd < 0 || fstat(pack_fd, &pack_stat) != 0 ||
      fstat(index_fd, &index_stat) != 0) {
    set_error("No complete asset pack was found");
    goto failed;
  }
  if (!read_exact_fd(pack_fd, &pack_header, sizeof pack_header, 0) ||
      !read_exact_fd(index_fd, &index_header, sizeof index_header, 0) ||
      memcmp(pack_header.magic, "SSNXPAK1", 8) ||
      memcmp(index_header.magic, "SSNXIDX1", 8) ||
      pack_header.version != PACK_VERSION || index_header.version != PACK_VERSION ||
      pack_header.pack_id != index_header.pack_id ||
      pack_header.file_size != (uint64_t)pack_stat.st_size ||
      pack_header.data_checksum != index_header.data_checksum ||
      index_header.pack_size != (uint64_t)pack_stat.st_size ||
      index_header.count == 0 || index_header.count > 100000 ||
      index_header.paths_size == 0 || index_header.paths_size > (64u << 20)) {
    set_error("The asset pack header is invalid");
    goto failed;
  }

  uint64_t entries_size = (uint64_t)index_header.count * sizeof(DiskEntry);
  uint64_t expected_index = sizeof(IndexHeader) + entries_size + index_header.paths_size;
  if (expected_index != (uint64_t)index_stat.st_size || expected_index > SIZE_MAX) {
    set_error("The asset index size is invalid");
    goto failed;
  }
  entries = malloc((size_t)entries_size);
  paths = malloc((size_t)index_header.paths_size);
  if (!entries || !paths ||
      !read_exact_fd(index_fd, entries, (size_t)entries_size, sizeof(IndexHeader)) ||
      !read_exact_fd(index_fd, paths, (size_t)index_header.paths_size,
                     sizeof(IndexHeader) + entries_size)) {
    set_error("The asset index could not be read");
    goto failed;
  }
  uint64_t checksum = fnv_bytes(1469598103934665603ULL, entries, (size_t)entries_size);
  checksum = fnv_bytes(checksum, paths, (size_t)index_header.paths_size);
  if (checksum != index_header.checksum) {
    set_error("The asset index checksum is invalid");
    goto failed;
  }
  for (uint32_t i = 0; i < index_header.count; i++) {
    DiskEntry *entry = &entries[i];
    uint64_t path_end = (uint64_t)entry->path_offset + entry->path_length + 1;
    if (entry->path_length == 0 || path_end > index_header.paths_size ||
        paths[entry->path_offset + entry->path_length] != 0 ||
        !valid_relative(paths + entry->path_offset, entry->path_length) ||
        entry->offset < sizeof(PackHeader) || entry->offset > index_header.pack_size ||
        entry->size > index_header.pack_size - entry->offset ||
        (i && strcmp(paths + entries[i - 1].path_offset,
                     paths + entry->path_offset) >= 0)) {
      set_error("The asset index contains an invalid entry");
      goto failed;
    }
  }

  close(index_fd);
  *entries_out = entries;
  *paths_out = paths;
  *count_out = index_header.count;
  *fd_out = pack_fd;
  return 1;

failed:
  if (index_fd >= 0) close(index_fd);
  if (pack_fd >= 0) close(pack_fd);
  free(entries);
  free(paths);
  return 0;
}

int asset_pack_open_existing(const char *root) {
  if (g_pack_fd >= 0) return 1;
  if (!root || !root[0]) { set_error("No asset pack root"); return 0; }
  if (!g_locks_ready) {
    mutexInit(&g_handle_lock);
    mutexInit(&g_pack_io_lock);
    for (int i = 0; i < PACK_HANDLES; i++) mutexInit(&g_handles[i].lock);
    g_locks_ready = 1;
  }
  char pack_path[768], index_path[768];
  snprintf(pack_path, sizeof pack_path, "%s/assets.nxpack", root);
  snprintf(index_path, sizeof index_path, "%s/assets.nxidx", root);
  DiskEntry *entries = NULL;
  char *paths = NULL;
  size_t count = 0;
  int fd = -1;
  if (!load_pair(pack_path, index_path, &entries, &paths, &count, &fd)) return 0;
  g_entries = entries;
  g_paths = paths;
  g_entry_count = count;
  g_pack_fd = fd;
  g_data_size = 0;
  for (size_t i = 0; i < count; i++) g_data_size += entries[i].size;
  g_error[0] = 0;
  return 1;
}

int asset_pack_active(void) {
  return g_pack_fd >= 0;
}

uint64_t asset_pack_data_size(void) {
  return g_data_size;
}

static PackHandle *get_handle(int fd) {
  if (fd < PACK_FD_BASE) return NULL;
  uint32_t slot = (uint32_t)(fd - PACK_FD_BASE);
  if (slot >= PACK_HANDLES) return NULL;
  return g_handles[slot].used ? &g_handles[slot] : NULL;
}

int asset_pack_fd_is(int fd) {
  return get_handle(fd) != NULL;
}

static int add_handle(uint32_t entry, uint64_t position, uint64_t directory_ino) {
  mutexLock(&g_handle_lock);
  for (int i = 0; i < PACK_HANDLES; i++) {
    PackHandle *h = &g_handles[i];
    if (!h->used) {
      h->used = 1;
      h->entry = entry;
      h->directory_ino = directory_ino;
      h->position = position;
      h->cache_offset = 0;
      h->cache_size = 0;
      h->cache = NULL;
      mutexUnlock(&g_handle_lock);
      return PACK_FD_BASE + i;
    }
  }
  mutexUnlock(&g_handle_lock);
  errno = EMFILE;
  return -1;
}

int asset_pack_open_path(const char *path) {
  char relative[768];
  size_t index;
  uint64_t directory_ino = 0;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return -1;
  int directory = !relative[0] || !find_relative(relative, &index);
  if (directory && !find_directory_relative(relative, &directory_ino)) {
    return -1;
  }
  uint32_t entry = directory ? PACK_DIRECTORY_ENTRY : (uint32_t)index;
  return add_handle(entry, 0, directory_ino);
}

int asset_pack_dup_fd(int fd) {
  PackHandle *source = get_handle(fd);
  if (!source) { errno = EBADF; return -1; }
  mutexLock(&source->lock);
  uint32_t entry = source->entry;
  uint64_t directory_ino = source->directory_ino;
  uint64_t position = source->position;
  mutexUnlock(&source->lock);
  return add_handle(entry, position, directory_ino);
}

int asset_pack_dup2_fd(int fd, int target) {
  /* Synthetic descriptors cannot be installed at an arbitrary real fd. The
   * game has never required it for an asset fd; return a fresh handle so a
   * caller that uses the return value still works. */
  (void)target;
  return asset_pack_dup_fd(fd);
}

int asset_pack_stat_relative(const char *path, uint64_t *size, uint64_t *ino) {
  size_t index;
  if (!find_relative(path, &index)) return 0;
  if (size) *size = g_entries[index].size;
  if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
  return 1;
}

int asset_pack_stat_path(const char *path, uint64_t *size, uint64_t *ino) {
  char relative[768];
  if (!normalize_relative(path, relative, sizeof relative)) return 0;
  return asset_pack_stat_relative(relative, size, ino);
}

int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino,
                              int *directory) {
  char relative[768];
  size_t index;
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return 0;
  if (relative[0] && find_relative(relative, &index)) {
    if (size) *size = g_entries[index].size;
    if (ino) *ino = 0x5353000000000000ULL | (uint64_t)(index + 1);
    if (directory) *directory = 0;
    return 1;
  }
  if (find_directory_relative(relative, ino)) {
    if (size) *size = 0;
    if (directory) *directory = 1;
    return 1;
  }
  return 0;
}

static long pread_entry(uint32_t entry_index, void *buffer, size_t count, uint64_t offset) {
  if (entry_index >= g_entry_count) { errno = EBADF; return -1; }
  DiskEntry *entry = &g_entries[entry_index];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  if (!read_at_pack(buffer, count, entry->offset + offset)) return -1;
  return (long)count;
}

static long read_handle_at(PackHandle *handle, void *buffer, size_t count, uint64_t offset) {
  if (handle->entry == PACK_DIRECTORY_ENTRY) { errno = EISDIR; return -1; }
  DiskEntry *entry = &g_entries[handle->entry];
  if (offset >= entry->size) return 0;
  uint64_t available = entry->size - offset;
  if ((uint64_t)count > available) count = (size_t)available;
  if (!read_at_pack(buffer, count, entry->offset + offset)) return -1;
  return (long)count;
}

long asset_pack_read_fd(int fd, void *buffer, size_t count) {
  PackHandle *handle = get_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  mutexLock(&handle->lock);
  uint32_t entry = handle->entry;
  long result = 0;
  if (entry == PACK_DIRECTORY_ENTRY) {
    errno = EISDIR;
    result = -1;
  } else if (handle->position >= g_entries[entry].size) {
    result = 0;
  } else if (count > PACK_CACHE_SIZE / 2) {
    result = read_handle_at(handle, buffer, count, handle->position);
    if (result > 0) handle->position += (uint64_t)result;
  } else {
    size_t done = 0;
    while (done < count) {
      if (!handle->cache || handle->position < handle->cache_offset ||
          handle->position >= handle->cache_offset + handle->cache_size) {
        if (!handle->cache) handle->cache = malloc(PACK_CACHE_SIZE);
        if (!handle->cache) { result = done ? (long)done : -1; break; }
        handle->cache_offset = handle->position;
        handle->cache_size = 0;
        long got = read_handle_at(handle, handle->cache, PACK_CACHE_SIZE,
                                  handle->cache_offset);
        if (got <= 0) { result = done ? (long)done : got; break; }
        handle->cache_size = (size_t)got;
      }
      size_t inside = (size_t)(handle->position - handle->cache_offset);
      size_t available = handle->cache_size - inside;
      size_t take = count - done < available ? count - done : available;
      memcpy((char *)buffer + done, handle->cache + inside, take);
      handle->position += take;
      done += take;
      result = (long)done;
    }
  }
  mutexUnlock(&handle->lock);
  return result;
}

long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset) {
  PackHandle *handle = get_handle(fd);
  if (!handle || offset < 0) { errno = EINVAL; return -1; }
  mutexLock(&handle->lock);
  uint32_t entry = handle->entry;
  long result = entry == PACK_DIRECTORY_ENTRY ? (errno = EISDIR, -1) :
                read_handle_at(handle, buffer, count, (uint64_t)offset);
  mutexUnlock(&handle->lock);
  return result;
}

long asset_pack_lseek_fd(int fd, long offset, int whence) {
  PackHandle *handle = get_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  mutexLock(&handle->lock);
  if (handle->entry == PACK_DIRECTORY_ENTRY) {
    mutexUnlock(&handle->lock);
    errno = EISDIR;
    return -1;
  }
  int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ?
                 (int64_t)handle->position : whence == SEEK_END ?
                 (int64_t)g_entries[handle->entry].size : -1;
  if (base < 0 || (offset < 0 && base < -offset)) {
    mutexUnlock(&handle->lock);
    errno = EINVAL;
    return -1;
  }
  handle->position = (uint64_t)(base + offset);
  long result = (long)handle->position;
  mutexUnlock(&handle->lock);
  return result;
}

int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory) {
  PackHandle *handle = get_handle(fd);
  if (!handle) return 0;
  if (handle->entry == PACK_DIRECTORY_ENTRY) {
    if (size) *size = 0;
    if (ino) *ino = handle->directory_ino;
    if (directory) *directory = 1;
    return 1;
  }
  if (directory) *directory = 0;
  return asset_pack_stat_relative(g_paths + g_entries[handle->entry].path_offset,
                                  size, ino);
}

int asset_pack_close_fd(int fd) {
  PackHandle *handle = get_handle(fd);
  if (!handle) { errno = EBADF; return -1; }
  mutexLock(&handle->lock);
  free(handle->cache);
  handle->cache = NULL;
  handle->cache_size = 0;
  handle->cache_offset = 0;
  handle->used = 0;
  mutexUnlock(&handle->lock);
  return 0;
}

int asset_pack_read_all_relative(const char *path, void **data, size_t *size) {
  size_t index;
  if (!data || !size) return 0;
  if (!find_relative(path, &index)) return 0;
  if (g_entries[index].size > SIZE_MAX) return 0;
  size_t length = (size_t)g_entries[index].size;
  void *buffer = malloc(length ? length : 1);
  if (!buffer || (length && pread_entry((uint32_t)index, buffer, length, 0) != (long)length)) {
    free(buffer);
    return 0;
  }
  *data = buffer;
  *size = length;
  return 1;
}

int asset_pack_read_all_path(const char *path, void **data, size_t *size) {
  char relative[768];
  return normalize_relative(path, relative, sizeof relative) &&
         asset_pack_read_all_relative(relative, data, size);
}

size_t asset_pack_entry_count(void) {
  return g_entry_count;
}

const char *asset_pack_entry_path(size_t index) {
  return index < g_entry_count ? g_paths + g_entries[index].path_offset : NULL;
}

void *asset_pack_opendir_path(const char *path) {
  char relative[768];
  if (!normalize_asset_path(path, relative, sizeof relative, 1)) return NULL;
  size_t prefix = strlen(relative);
  int found = 0;
  for (size_t i = 0; i < g_entry_count; i++) {
    const char *entry = g_paths + g_entries[i].path_offset;
    if ((!prefix && entry[0]) ||
        (prefix && !strncmp(entry, relative, prefix) && entry[prefix] == '/')) {
      found = 1;
      break;
    }
  }
  if (!found) return NULL;
  PackDir *dir = calloc(1, sizeof(*dir));
  if (!dir) return NULL;
  dir->magic = PACK_DIR_MAGIC;
  snprintf(dir->prefix, sizeof dir->prefix, "%s", relative);
  return dir;
}

int asset_pack_dir_is(const void *dir) {
  return dir && ((const PackDir *)dir)->magic == PACK_DIR_MAGIC;
}

const char *asset_pack_readdir_path(void *opaque, uint8_t *type, uint64_t *ino) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return NULL;
  if (dir->dots < 2) {
    snprintf(dir->name, sizeof dir->name, "%s", dir->dots++ ? ".." : ".");
    if (type) *type = DT_DIR;
    if (ino) *ino = dir->dots;
    return dir->name;
  }
  size_t prefix = strlen(dir->prefix);
  while (dir->cursor < g_entry_count) {
    size_t index = dir->cursor++;
    const char *entry = g_paths + g_entries[index].path_offset;
    const char *tail = entry;
    if (prefix) {
      if (strncmp(entry, dir->prefix, prefix) || entry[prefix] != '/') continue;
      tail = entry + prefix + 1;
    }
    const char *slash = strchr(tail, '/');
    size_t length = slash ? (size_t)(slash - tail) : strlen(tail);
    if (!length || length >= sizeof dir->name) continue;
    if (strlen(dir->last) == length && !memcmp(dir->last, tail, length)) continue;
    memcpy(dir->name, tail, length);
    dir->name[length] = 0;
    snprintf(dir->last, sizeof dir->last, "%s", dir->name);
    if (type) *type = slash ? DT_DIR : DT_REG;
    if (ino) {
      uint64_t hash = fnv_bytes(1469598103934665603ULL, dir->name, length);
      *ino = hash ? hash : 1;
    }
    return dir->name;
  }
  return NULL;
}

int asset_pack_closedir_path(void *opaque) {
  PackDir *dir = opaque;
  if (!asset_pack_dir_is(dir)) return -1;
  dir->magic = 0;
  free(dir);
  return 0;
}
