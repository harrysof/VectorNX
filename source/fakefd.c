/* fakefd.c -- fake file-descriptor pipes for the Switch port.
 *
 * libc_shim.c routes pipe()/read()/write()/close() on high-numbered fds through
 * these so the engine's pipe()/eventfd-style thread signalling works without a
 * real OS fd table. Ported from cr3_nx's android_native.c but standalone: Unity
 * uses ALooper only as a wait/wake primitive (no addFd), so this needs no looper
 * coupling. Thread-safe; lazily initialised.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <switch.h>

#define FAKE_FD_BASE 0x40000000
#define MAX_FAKE_FDS 32
#define PIPE_CAP     4096

enum { FD_NONE = 0, FD_PIPE_R, FD_PIPE_W, FD_MEM };

typedef struct { uint8_t buf[PIPE_CAP]; size_t head, len; int refs; } Pipe;
typedef struct {
  int kind;
  Pipe *pipe;
  /* FD_MEM: a read-only file backed by a caller-owned or copied buffer. Used
   * for the console's cached fonts, whose bytes are already in RAM. */
  uint8_t *mem;
  size_t   mem_size, mem_pos;
  int      mem_own;
} FakeFd;

static FakeFd  g_fds[MAX_FAKE_FDS];
static Mutex   g_lock;
static CondVar g_cond;
static int     g_inited = 0;

static void fakefd_once(void) {
  if (!g_inited) { mutexInit(&g_lock); condvarInit(&g_cond); g_inited = 1; }
}
static int alloc_slot(void) {
  for (int i = 0; i < MAX_FAKE_FDS; i++)
    if (g_fds[i].kind == FD_NONE) return i;
  return -1;
}

int fakefd_is_fake(int fd) {
  return fd >= FAKE_FD_BASE && fd < FAKE_FD_BASE + MAX_FAKE_FDS;
}

int fakefd_pipe(int fds[2]) {
  fakefd_once();
  mutexLock(&g_lock);
  int r = alloc_slot();
  if (r < 0) { mutexUnlock(&g_lock); return -1; }
  g_fds[r].kind = FD_PIPE_R;
  int w = alloc_slot();
  if (w < 0) { g_fds[r].kind = FD_NONE; mutexUnlock(&g_lock); return -1; }
  g_fds[w].kind = FD_PIPE_W;
  Pipe *p = calloc(1, sizeof(*p));
  if (!p) { g_fds[r].kind = g_fds[w].kind = FD_NONE; mutexUnlock(&g_lock); return -1; }
  p->refs = 2;
  g_fds[r].pipe = g_fds[w].pipe = p;
  mutexUnlock(&g_lock);
  fds[0] = FAKE_FD_BASE + r;
  fds[1] = FAKE_FD_BASE + w;
  return 0;
}

/* Read-only memory file. `copy` != 0 makes an owned snapshot. */
int fakefd_open_mem(const void *data, unsigned long size, int copy) {
  if (!data || !size) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  int slot = alloc_slot();
  if (slot < 0) { mutexUnlock(&g_lock); return -1; }
  FakeFd *f = &g_fds[slot];
  f->kind = FD_MEM;
  f->mem_size = size;
  f->mem_pos = 0;
  if (copy) {
    f->mem = malloc(size);
    if (!f->mem) { memset(f, 0, sizeof *f); mutexUnlock(&g_lock); return -1; }
    memcpy(f->mem, data, size);
    f->mem_own = 1;
  } else {
    f->mem = (uint8_t *)(uintptr_t)data;
    f->mem_own = 0;
  }
  mutexUnlock(&g_lock);
  return FAKE_FD_BASE + slot;
}

long fakefd_lseek(int fd, long off, int whence) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  if (f->kind != FD_MEM) { mutexUnlock(&g_lock); return -1; }
  long base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? (long)f->mem_pos
             : (whence == SEEK_END) ? (long)f->mem_size : -1;
  if (base < 0 || (off < 0 && base + off < 0)) { mutexUnlock(&g_lock); return -1; }
  long np = base + off;
  if (np < 0) { mutexUnlock(&g_lock); return -1; }
  f->mem_pos = (size_t)np;
  mutexUnlock(&g_lock);
  return np;
}

long fakefd_pread(int fd, void *buf, unsigned long n, long off) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  if (f->kind != FD_MEM || off < 0) { mutexUnlock(&g_lock); return -1; }
  size_t at = (size_t)off;
  if (at >= f->mem_size) { mutexUnlock(&g_lock); return 0; }
  size_t avail = f->mem_size - at;
  size_t got = n < avail ? n : avail;
  memcpy(buf, f->mem + at, got);
  mutexUnlock(&g_lock);
  return (long)got;
}

int fakefd_stat(int fd, unsigned long long *size, unsigned long long *ino) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return 0;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  int ok = (f->kind == FD_MEM);
  if (ok) {
    if (size) *size = f->mem_size;
    if (ino) *ino = 0x464f4e5400000000ULL | (unsigned)(slot + 1);
  }
  mutexUnlock(&g_lock);
  return ok;
}

const void *fakefd_data(int fd, unsigned long *size) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return NULL;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  const void *p = (f->kind == FD_MEM) ? f->mem : NULL;
  if (p && size) *size = (unsigned long)f->mem_size;
  mutexUnlock(&g_lock);
  return p;
}

long fakefd_write(int fd, const void *buf, unsigned long n) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  Pipe *p = g_fds[slot].pipe;
  if (g_fds[slot].kind != FD_PIPE_W || !p) { mutexUnlock(&g_lock); return -1; }
  size_t wrote = 0;
  const uint8_t *src = buf;
  while (wrote < n && p->len < PIPE_CAP) {
    const size_t tail = (p->head + p->len) % PIPE_CAP;
    p->buf[tail] = src[wrote++];
    p->len++;
  }
  condvarWakeAll(&g_cond);
  mutexUnlock(&g_lock);
  return (long)wrote;
}

long fakefd_read(int fd, void *buf, unsigned long n) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  if (f->kind == FD_MEM) {
    if (f->mem_pos >= f->mem_size) { mutexUnlock(&g_lock); return 0; }
    size_t avail = f->mem_size - f->mem_pos;
    size_t got = n < avail ? n : avail;
    memcpy(buf, f->mem + f->mem_pos, got);
    f->mem_pos += got;
    mutexUnlock(&g_lock);
    return (long)got;
  }
  Pipe *p = g_fds[slot].pipe;
  if (g_fds[slot].kind != FD_PIPE_R || !p) { mutexUnlock(&g_lock); return -1; }
  /* block until data arrives or all writers close (refs drops to the reader) */
  while (p->len == 0 && p->refs > 1) condvarWait(&g_cond, &g_lock);
  size_t got = 0;
  uint8_t *dst = buf;
  while (got < n && p->len > 0) {
    dst[got++] = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_CAP;
    p->len--;
  }
  mutexUnlock(&g_lock);
  return (long)got;            /* 0 == EOF (writers gone, buffer drained) */
}

int fakefd_close(int fd) {
  const int slot = fd - FAKE_FD_BASE;
  if (slot < 0 || slot >= MAX_FAKE_FDS) return -1;
  fakefd_once();
  mutexLock(&g_lock);
  FakeFd *f = &g_fds[slot];
  if (f->kind == FD_MEM && f->mem_own) free(f->mem);
  if (f->pipe && --f->pipe->refs <= 0) free(f->pipe);
  memset(f, 0, sizeof(*f));
  condvarWakeAll(&g_cond);      /* wake any blocked reader -> treats as EOF */
  mutexUnlock(&g_lock);
  return 0;
}
