/* config.c -- Vector config.txt reader/writer.
 * MIT (see LICENSE). Forked from the SoLoader lineage via badpiggies_nx.
 *
 * Keys:
 *   language=auto|<ISO code>
 *   render_width=<pixels>    internal render size; compositor upscales
 *   render_height=<pixels>
 *   framerate=60             hard-locked 60; 30 is ignored
 *   log=0|1                  debug.log on/off
 *
 * The defaults are 960x540: 16:9, 75% of the panel, and enough headroom for a
 * stable frame rate. 1280x720 is available by setting both keys.
 */
#include <stdio.h>
#include <string.h>
#include "config.h"

int screen_width  = 0;
int screen_height = 0;

Config config = {
  .language = "auto",
  .render_width  = VEC_FORCE_SCREEN_W,
  .render_height = VEC_FORCE_SCREEN_H,
  .framerate = 60,
  .log = 0,
};

static int clamp_int(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

/* Returns 0 = read clean, -1 = missing, 1 = present but missing/extra keys
 * (caller rewrites so the file self-heals to the current schema). */
int read_config(const char *file) {
  FILE *f = fopen(file, "r");
  if (!f) return -1;
  int seen = 0;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;

    char sval[64];
    if (sscanf(line, "language=%63s", sval) == 1) {
      size_t n = strlen(sval);
      while (n && (sval[n-1] == '\r' || sval[n-1] == ' ')) sval[--n] = '\0';
      snprintf(config.language, sizeof config.language, "%s", sval);
      seen |= 1;
      continue;
    }
    int ival = 0;
    if (sscanf(line, "render_width=%d", &ival) == 1) {
      config.render_width = clamp_int(ival, 640, 1920);
      seen |= 2;
      continue;
    }
    if (sscanf(line, "render_height=%d", &ival) == 1) {
      config.render_height = clamp_int(ival, 360, 1080);
      seen |= 4;
      continue;
    }
    if (sscanf(line, "framerate=%d", &ival) == 1) {
      /* Hard-locked 60. Accept the key for old configs, but never honour 30. */
      (void)ival;
      config.framerate = 60;
      seen |= 8;
      continue;
    }
    if (sscanf(line, "log=%d", &ival) == 1) {
      config.log = (ival != 0) ? 1 : 0;
      seen |= 16;
      continue;
    }
  }
  fclose(f);
  return (seen == 31) ? 0 : 1;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) return -1;
  fprintf(f,
    "# Vector (vector_nx) config\n"
    "\n"
    "# language: 'auto' follows the Switch system language, or an ISO code\n"
    "# (en, ru, de, fr, es, it, pt, tr, ja, ko, zh_CN, ...).\n"
    "language=%s\n"
    "\n"
    "# Internal render size. The compositor upscales to the panel. 960x540 is\n"
    "# the default for a stable frame rate; 1280x720 is full quality.\n"
    "render_width=%d\n"
    "render_height=%d\n"
    "\n"
    "# framerate: hard-locked at 60. The key is kept for compatibility; a value\n"
    "# of 30 is ignored and rewritten to 60.\n"
    "framerate=60\n"
    "\n"
    "# log: 1 writes debug.log to the SD card (useful for diagnosing a boot or\n"
    "# a crash, but the per-frame lines cost I/O); 0 is the play setting.\n"
    "log=%d\n",
    config.language[0] ? config.language : "auto",
    config.render_width, config.render_height,
    config.log);
  fclose(f);
  return 0;
}
