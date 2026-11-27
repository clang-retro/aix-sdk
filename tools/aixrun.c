// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 clang-retro contributors

/*
 * aixrun: run a 32-bit AIX XCOFF program on any host.
 *
 * aixrun loads a statically linked XCOFF executable the way the AIX loader
 * does: it maps .text, .data and .bss, applies the .loader relocations,
 * binds every symbol imported from /unix to a kernel call it emulates, and
 * starts the program through the entry point's function descriptor with
 * argc, argv and envp in r3, r4 and r5. A PowerPC interpreter then runs
 * the program. It covers the 32-bit user instruction set that compilers
 * emit for the 601, 603 and 604, including floating point.
 *
 * This is a test harness for the clang-retro AIX toolchain, so that CI can
 * run what it builds. It is not an AIX emulator: only the kernel calls
 * below exist, and they map onto the host's own.
 *
 * Usage: aixrun [-v] [-k] program [args...]
 *   -v  trace every kernel call to stderr
 *   -k  list the kernel calls aixrun provides, and exit
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#define host_mkdir(p, m) _mkdir(p)
#ifndef O_BINARY
#define O_BINARY 0
#endif
#else
#include <unistd.h>
#define host_mkdir(p, m) mkdir(p, m)
#define O_BINARY 0
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

static int trace;

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fputs("aixrun: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  exit(127);
}

/* ---- Memory ------------------------------------------------------------ */

struct region {
  u32 base, size;
  u8 *mem;
  int writable;
  const char *name;
};

#define MAX_REGIONS 8
static struct region regions[MAX_REGIONS];
static int nregions;
static struct region *last_region;

static struct region *add_region(u32 base, u32 size, int writable,
                                 const char *name) {
  struct region *r;
  if (nregions == MAX_REGIONS)
    die("too many memory regions");
  r = &regions[nregions++];
  r->base = base;
  r->size = size;
  r->mem = calloc(1, size ? size : 1);
  if (!r->mem)
    die("out of memory for %s (%u bytes)", name, size);
  r->writable = writable;
  r->name = name;
  return r;
}

static u32 cur_pc;

static u8 *addr(u32 a, u32 len, int write) {
  struct region *r = last_region;
  int i;
  if (r && a - r->base < r->size && r->size - (a - r->base) >= len)
    goto found;
  for (i = 0; i < nregions; i++) {
    r = &regions[i];
    if (a - r->base < r->size && r->size - (a - r->base) >= len)
      goto found;
  }
  die("%s of %u bytes at 0x%08x, outside the program (pc 0x%08x)",
      write ? "write" : "read", len, a, cur_pc);
found:
  if (write && !r->writable)
    die("write to read-only %s at 0x%08x (pc 0x%08x)", r->name, a, cur_pc);
  last_region = r;
  return r->mem + (a - r->base);
}

static u32 rd32(u32 a) {
  u8 *p = addr(a, 4, 0);
  return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static u16 rd16(u32 a) {
  u8 *p = addr(a, 2, 0);
  return (u16)(p[0] << 8 | p[1]);
}
static u8 rd8(u32 a) { return *addr(a, 1, 0); }
static void wr32(u32 a, u32 v) {
  u8 *p = addr(a, 4, 1);
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
static void wr16(u32 a, u16 v) {
  u8 *p = addr(a, 2, 1);
  p[0] = v >> 8;
  p[1] = v;
}
static void wr8(u32 a, u8 v) { *addr(a, 1, 1) = v; }
static u64 rd64(u32 a) { return (u64)rd32(a) << 32 | rd32(a + 4); }
static void wr64(u32 a, u64 v) {
  wr32(a, (u32)(v >> 32));
  wr32(a + 4, (u32)v);
}

/* A guest NUL-terminated string. */
static const char *gstr(u32 a) {
  struct region *r;
  u8 *p = addr(a, 1, 0);
  r = last_region;
  if (!memchr(p, 0, r->size - (a - r->base)))
    die("unterminated string at 0x%08x", a);
  return (const char *)p;
}

/* ---- CPU ----------------------------------------------------------------- */

static u32 gpr[32];
static double fpr[32];
static u32 cr, lr, ctr, xer, fpscr;
static u32 pc;
static u64 icount;

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

static void set_cr_field(int field, u32 bits) {
  int shift = (7 - field) * 4;
  cr = (cr & ~(0xfu << shift)) | ((bits & 0xf) << shift);
}

static void record(u32 v) {
  u32 bits = (s32)v < 0 ? 8 : (s32)v > 0 ? 4 : 2;
  if (xer & XER_SO)
    bits |= 1;
  set_cr_field(0, bits);
}

static u32 rotl(u32 v, int n) {
  n &= 31;
  return n ? (v << n) | (v >> (32 - n)) : v;
}

static u32 mask(int mb, int me) {
  u32 m;
  if (mb <= me)
    m = (0xffffffffu >> mb) & (0xffffffffu << (31 - me));
  else
    m = (0xffffffffu >> mb) | (0xffffffffu << (31 - me));
  return m;
}

static int cond_ok(int bo, int bi) {
  int ctr_ok, cond;
  if (!(bo & 4))
    ctr--;
  ctr_ok = (bo & 4) || ((ctr != 0) ^ ((bo & 2) != 0));
  cond = (bo & 16) || ((((cr >> (31 - bi)) & 1) != 0) == ((bo & 8) != 0));
  return ctr_ok && cond;
}

static u32 carry_add(u32 a, u32 b, u32 c) {
  u64 s = (u64)a + b + c;
  if (s >> 32)
    xer |= XER_CA;
  else
    xer &= ~XER_CA;
  return (u32)s;
}

static void set_ov(int ov) {
  if (ov)
    xer |= XER_OV | XER_SO;
  else
    xer &= ~XER_OV;
}

static float as_float(u32 bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}
static u32 float_bits(float f) {
  u32 b;
  memcpy(&b, &f, 4);
  return b;
}
static double as_double(u64 bits) {
  double d;
  memcpy(&d, &bits, 8);
  return d;
}
static u64 double_bits(double d) {
  u64 b;
  memcpy(&b, &d, 8);
  return b;
}

static void fp_record(void) {
  set_cr_field(1, fpscr >> 28);
}

static void fcompare(int crf, double a, double b) {
  u32 c;
  if (isnan(a) || isnan(b))
    c = 1;
  else if (a < b)
    c = 8;
  else if (a > b)
    c = 4;
  else
    c = 2;
  set_cr_field(crf, c);
  fpscr = (fpscr & ~0xf000u) | (c << 12);
}

static s32 to_int32(double d, int truncate) {
  double r;
  if (isnan(d))
    return (s32)0x80000000u;
  if (truncate)
    r = trunc(d);
  else {
    switch (fpscr & 3) {
    case 0:
      r = nearbyint(d);
      break;
    case 1:
      r = trunc(d);
      break;
    case 2:
      r = ceil(d);
      break;
    default:
      r = floor(d);
      break;
    }
  }
  if (r >= 2147483647.0)
    return 0x7fffffff;
  if (r <= -2147483648.0)
    return (s32)0x80000000u;
  return (s32)r;
}

/* ---- Kernel calls ------------------------------------------------------- */

/*
 * Imports from /unix are bound to function descriptors whose code address
 * lies in a region that holds no code. Reaching one runs the kernel call.
 */
#define KCALL_BASE 0x0f000000u
#define KDESC_BASE 0x0e000000u

typedef u32 (*kfn)(void);

struct kcall {
  const char *name;
  kfn fn;
};

static int aix_errno;     /* the error of the last failing kernel call */
static u32 errno_addr;    /* guest errno variable, when the program has one */
static u32 brk_start, brk_cur, brk_limit;

static u32 fail(int host_err);

/* AIX errno values for host errors. */
static int aix_err(int e) {
  switch (e) {
  case EPERM: return 1;
  case ENOENT: return 2;
  case ESRCH: return 3;
  case EINTR: return 4;
  case EIO: return 5;
  case ENXIO: return 6;
  case E2BIG: return 7;
  case ENOEXEC: return 8;
  case EBADF: return 9;
  case ECHILD: return 10;
  case EAGAIN: return 11;
  case ENOMEM: return 12;
  case EACCES: return 13;
  case EFAULT: return 14;
  case EBUSY: return 16;
  case EEXIST: return 17;
  case EXDEV: return 18;
  case ENODEV: return 19;
  case ENOTDIR: return 20;
  case EISDIR: return 21;
  case EINVAL: return 22;
  case ENFILE: return 23;
  case EMFILE: return 24;
  case ENOTTY: return 25;
  case EFBIG: return 27;
  case ENOSPC: return 28;
  case ESPIPE: return 29;
  case EROFS: return 30;
  case EMLINK: return 31;
  case EPIPE: return 32;
  case EDOM: return 33;
  case ERANGE: return 34;
#ifdef ENOTEMPTY
  case ENOTEMPTY: return 87;
#endif
#ifdef ENAMETOOLONG
  case ENAMETOOLONG: return 86;
#endif
#ifdef ELOOP
  case ELOOP: return 85;
#endif
#ifdef ENOSYS
  case ENOSYS: return 109;
#endif
  default: return 5;
  }
}

static u32 fail(int host_err) {
  aix_errno = aix_err(host_err);
  if (errno_addr)
    wr32(errno_addr, (u32)aix_errno);
  return (u32)-1;
}

static int host_oflags(u32 f) {
  int h = 0;
  switch (f & 3) {
  case 0: h = O_RDONLY; break;
  case 1: h = O_WRONLY; break;
  default: h = O_RDWR; break;
  }
  if (f & 0x8) h |= O_APPEND;
  if (f & 0x100) h |= O_CREAT;
  if (f & 0x200) h |= O_TRUNC;
  if (f & 0x400) h |= O_EXCL;
  return h | O_BINARY;
}

static u32 k_exit(void) {
  fflush(stdout);
  exit((int)(gpr[3] & 0xff));
  return 0;
}

static u32 k_write(void) {
  u32 n = gpr[5];
  long r;
  if (n == 0)
    return 0;
  r = write((int)gpr[3], addr(gpr[4], n, 0), n);
  return r < 0 ? fail(errno) : (u32)r;
}

static u32 k_read(void) {
  u32 n = gpr[5];
  long r;
  if (n == 0)
    return 0;
  r = read((int)gpr[3], addr(gpr[4], n, 1), n);
  return r < 0 ? fail(errno) : (u32)r;
}

static u32 k_open(void) {
  int fd = open(gstr(gpr[3]), host_oflags(gpr[4]), (int)(gpr[5] & 0777));
  return fd < 0 ? fail(errno) : (u32)fd;
}

static u32 k_creat(void) {
  int fd = open(gstr(gpr[3]), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                (int)(gpr[4] & 0777));
  return fd < 0 ? fail(errno) : (u32)fd;
}

static u32 k_close(void) {
  int fd = (int)gpr[3];
  if (fd <= 2)
    return 0; /* keep the runner's own standard streams */
  return close(fd) < 0 ? fail(errno) : 0;
}

static u32 k_lseek(void) {
  long r = (long)lseek((int)gpr[3], (long)(s32)gpr[4], (int)gpr[5]);
  return r < 0 ? fail(errno) : (u32)r;
}

static u32 k_unlink(void) {
  return unlink(gstr(gpr[3])) < 0 ? fail(errno) : 0;
}

static u32 k_mkdir(void) {
  return host_mkdir(gstr(gpr[3]), (int)(gpr[4] & 0777)) < 0 ? fail(errno) : 0;
}

static u32 k_rmdir(void) {
  return rmdir(gstr(gpr[3])) < 0 ? fail(errno) : 0;
}

static u32 k_chdir(void) {
  return chdir(gstr(gpr[3])) < 0 ? fail(errno) : 0;
}

static u32 k_rename(void) {
  const char *from = gstr(gpr[3]);
  char *to = strdup(gstr(gpr[4]));
  int r;
#ifdef _WIN32
  remove(to);
#endif
  r = rename(from, to);
  free(to);
  return r < 0 ? fail(errno) : 0;
}

static u32 k_access(void) {
  return access(gstr(gpr[3]), (int)(gpr[4] & 7)) < 0 ? fail(errno) : 0;
}

static u32 k_getpid(void) { return (u32)getpid(); }

static u32 k_time(void) {
  u32 t = (u32)time(NULL);
  if (gpr[3])
    wr32(gpr[3], t);
  return t;
}

/* Fills in the 32-bit AIX struct stat (116 bytes) at guest address a. */
static u32 put_stat(u32 a, u32 len, const struct stat *st) {
  u32 b[29];
  unsigned i;
  if (len < sizeof b)
    return fail(EINVAL);
  memset(b, 0, sizeof b);
  b[0] = (u32)st->st_dev;
  b[1] = (u32)st->st_ino;
  b[2] = (u32)st->st_mode;
  b[3] = (u32)(st->st_nlink & 0xffff) << 16; /* st_nlink, st_flag */
  b[4] = (u32)st->st_uid;
  b[5] = (u32)st->st_gid;
  b[6] = (u32)st->st_rdev;
  b[7] = (u32)st->st_size;
  b[8] = (u32)st->st_atime;
  b[10] = (u32)st->st_mtime;
  b[12] = (u32)st->st_ctime;
#ifdef _WIN32
  b[14] = 512;
  b[15] = (u32)((st->st_size + 511) / 512);
#else
  b[14] = (u32)st->st_blksize;
  b[15] = (u32)st->st_blocks;
#endif
  for (i = 0; i < 29; i++)
    wr32(a + 4 * i, b[i]);
  return 0;
}

static u32 k_statx(void) {
  struct stat st;
  if (stat(gstr(gpr[3]), &st) < 0)
    return fail(errno);
  return put_stat(gpr[4], gpr[5], &st);
}

static u32 k_fstatx(void) {
  struct stat st;
  if (fstat((int)gpr[3], &st) < 0)
    return fail(errno);
  return put_stat(gpr[4], gpr[5], &st);
}

/* gettimer(TIMEOFDAY, struct timestruc_t *): seconds and nanoseconds. */
static u32 k_gettimer(void) {
  struct timespec ts;
  if (gpr[3] != 9)
    return fail(EINVAL);
#if defined(_WIN32) && !defined(TIME_UTC)
  ts.tv_sec = time(NULL);
  ts.tv_nsec = 0;
#else
  timespec_get(&ts, TIME_UTC);
#endif
  wr32(gpr[4], (u32)ts.tv_sec);
  wr32(gpr[4] + 4, (u32)ts.tv_nsec);
  return 0;
}

static u32 k_sbrk(void) {
  s32 incr = (s32)gpr[3];
  u32 old = brk_cur;
  if ((incr > 0 && brk_cur + (u32)incr > brk_limit) ||
      (incr < 0 && brk_cur - brk_start < (u32)-incr))
    return fail(ENOMEM);
  brk_cur += (u32)incr;
  return old;
}

static u32 k_brk(void) {
  u32 want = gpr[3];
  if (want < brk_start || want > brk_limit)
    return fail(ENOMEM);
  brk_cur = want;
  return 0;
}

static u32 k_ioctl(void) {
  /* Only isatty-style probes: a terminal answers, anything else is not. */
  int fd = (int)gpr[3];
#ifdef _WIN32
  int tty = _isatty(fd);
#else
  int tty = isatty(fd);
#endif
  if (!tty)
    return fail(ENOTTY);
  if (gpr[5])
    memset(addr(gpr[5], 4, 1), 0, 4);
  return 0;
}

static u32 k_kill(void) {
  if ((s32)gpr[3] == getpid() || gpr[3] == 0) {
    fflush(stdout);
    exit(128 + (int)(gpr[4] & 0x7f));
  }
  return fail(EPERM);
}

static const struct kcall kcalls[] = {
    {"_exit", k_exit},    {"exit", k_exit},
    {"kwrite", k_write},  {"write", k_write},
    {"kread", k_read},    {"read", k_read},
    {"kopen", k_open},    {"open", k_open},
    {"creat", k_creat},   {"close", k_close},
    {"klseek", k_lseek},  {"lseek", k_lseek},
    {"unlink", k_unlink}, {"mkdir", k_mkdir},
    {"rmdir", k_rmdir},   {"chdir", k_chdir},
    {"rename", k_rename}, {"access", k_access},
    {"kaccess", k_access}, {"getpid", k_getpid},
    {"_getpid", k_getpid}, {"time", k_time},
    {"sbrk", k_sbrk},     {"__libc_sbrk", k_sbrk},
    {"brk", k_brk},       {"kioctl", k_ioctl},
    {"ioctl", k_ioctl},   {"kill", k_kill},
    {"accessx", k_access}, {"statx", k_statx},
    {"fstatx", k_fstatx}, {"gettimer", k_gettimer},
};

#define MAX_IMPORTS 256
static const struct kcall *bound[MAX_IMPORTS];
static int nbound;

static void kernel_call(u32 target) {
  u32 i = (target - KCALL_BASE) / 4;
  const struct kcall *k;
  u32 r;
  if (i >= (u32)nbound)
    die("call to 0x%08x, which is not a kernel call", target);
  k = bound[i];
  if (trace)
    fprintf(stderr, "aixrun: %s(0x%x, 0x%x, 0x%x)", k->name, gpr[3], gpr[4],
            gpr[5]);
  r = k->fn();
  if (trace)
    fprintf(stderr, " = 0x%x\n", r);
  gpr[3] = r;
  pc = lr;
}

/* ---- Loader -------------------------------------------------------------- */

static u8 *image;
static long image_size;

static u32 be32(const u8 *p) {
  return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static const u8 *file_at(u32 off, u32 len) {
  if (off > (u32)image_size || len > (u32)image_size - off)
    die("program file is truncated");
  return image + off;
}

static u32 load(const char *path) {
  FILE *f = fopen(path, "rb");
  const u8 *fh, *ah, *sh, *ld;
  u16 nscns, opthdr;
  u32 entry, tsize, dsize, bsize, tstart, dstart;
  u32 toff = 0, doff = 0, loff = 0, lsize = 0, i;
  u32 nsyms, nrel, impoff, stoff;
  u32 *symaddr;
  struct region *data, *kdesc;

  if (!f)
    die("cannot open %s: %s", path, strerror(errno));
  fseek(f, 0, SEEK_END);
  image_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  image = malloc(image_size ? image_size : 1);
  if (!image || fread(image, 1, image_size, f) != (size_t)image_size)
    die("cannot read %s", path);
  fclose(f);

  fh = file_at(0, 20);
  if (be16(fh) != 0x01df)
    die("%s is not a 32-bit XCOFF file", path);
  nscns = be16(fh + 2);
  opthdr = be16(fh + 16);
  if (!(be16(fh + 18) & 0x0002))
    die("%s is not an executable", path);
  if (opthdr < 72)
    die("%s has no full auxiliary header", path);
  ah = file_at(20, 72);
  tsize = be32(ah + 4);
  dsize = be32(ah + 8);
  bsize = be32(ah + 12);
  entry = be32(ah + 16);
  tstart = be32(ah + 20);
  dstart = be32(ah + 24);

  sh = file_at(20 + opthdr, nscns * 40u);
  for (i = 0; i < nscns; i++) {
    const u8 *s = sh + i * 40;
    u32 flags = be32(s + 36) & 0xffff;
    if (flags == 0x20)
      toff = be32(s + 20);
    else if (flags == 0x40)
      doff = be32(s + 20);
    else if (flags == 0x1000) {
      loff = be32(s + 20);
      lsize = be32(s + 16);
    }
  }

  add_region(tstart, tsize, 0, ".text");
  memcpy(regions[0].mem, file_at(toff, tsize), tsize);

  /* .data, .bss and a heap for brk/sbrk share one region. */
  brk_start = (dstart + dsize + bsize + 15) & ~15u;
  brk_cur = brk_start;
  brk_limit = brk_start + 32u * 1024 * 1024;
  data = add_region(dstart, brk_limit - dstart, 1, ".data");
  memcpy(data->mem, file_at(doff, dsize), dsize);

  if (!loff)
    die("%s has no .loader section", path);
  ld = file_at(loff, lsize);
  if (lsize < 32 || be32(ld) != 1)
    die("unknown .loader section version");
  nsyms = be32(ld + 4);
  nrel = be32(ld + 8);
  impoff = be32(ld + 20);
  stoff = be32(ld + 28);

  /* Bind imports. */
  symaddr = calloc(nsyms + 1, sizeof(u32));
  kdesc = add_region(KDESC_BASE, MAX_IMPORTS * 12, 0, "import descriptors");
  add_region(KCALL_BASE, MAX_IMPORTS * 4, 0, "kernel");
  for (i = 0; i < nsyms; i++) {
    const u8 *s = file_at(loff + 32 + i * 24, 24);
    char name[256];
    if (be32(s) == 0) {
      u32 off = be32(s + 4);
      const u8 *n = file_at(loff + stoff + off, 1);
      snprintf(name, sizeof name, "%s", (const char *)n);
    } else {
      memcpy(name, s, 8);
      name[8] = 0;
    }
    if ((s[14] & 0x10) && !strcmp(name, "errno")) {
      /* The program exports its errno variable for the kernel to set. */
      errno_addr = be32(s + 8);
      continue;
    }
    if (!(s[14] & 0x40))
      continue; /* not an import */
    {
      u32 ifile = be32(s + 16);
      const char *p = (const char *)ld + impoff;
      const char *base;
      u32 k;
      const struct kcall *found = NULL;
      /* Find import file ID ifile: three strings per entry. */
      for (k = 0; k < ifile * 3 + 1; k++)
        p += strlen(p) + 1;
      base = p;
      if (strcmp(base, "unix") != 0)
        die("%s is imported from %s, but only /unix is available", name,
            base);
      if (!strcmp(name, "errno")) {
        /* The program's errno variable, kept by the kernel. */
        u32 a = brk_start;
        brk_start += 16;
        brk_cur = brk_start;
        errno_addr = a;
        symaddr[i] = a;
        continue;
      }
      for (k = 0; k < sizeof kcalls / sizeof kcalls[0]; k++)
        if (!strcmp(kcalls[k].name, name))
          found = &kcalls[k];
      if (!found)
        die("%s needs the kernel call %s, which aixrun does not provide",
            path, name);
      if (nbound == MAX_IMPORTS)
        die("too many imports");
      bound[nbound] = found;
      {
        u8 *d = kdesc->mem + nbound * 12;
        u32 code = KCALL_BASE + nbound * 4;
        d[0] = code >> 24; d[1] = code >> 16; d[2] = code >> 8; d[3] = code;
      }
      symaddr[i] = KDESC_BASE + nbound * 12;
      nbound++;
    }
  }

  /* Relocate: the program is loaded where it was linked, so only imports
     change. */
  for (i = 0; i < nrel; i++) {
    const u8 *r = file_at(loff + 32 + nsyms * 24 + i * 12, 12);
    u32 va = be32(r), symndx = be32(r + 4);
    u16 rtype = be16(r + 8);
    if ((rtype & 0xff) != 0 || (rtype >> 8) != 0x1f)
      die("unsupported loader relocation type 0x%04x", rtype);
    if (symndx < 3)
      continue;
    if (symndx - 3 >= nsyms)
      die("bad loader relocation symbol %u", symndx);
    wr32(va, rd32(va) + symaddr[symndx - 3]);
  }
  free(symaddr);
  return entry;
}

/* ---- Interpreter --------------------------------------------------------- */

#define RA ((insn >> 16) & 31)
#define RB ((insn >> 11) & 31)
#define RS ((insn >> 21) & 31)
#define RT RS
#define RC (insn & 1)
#define SIMM ((s32)(int16_t)(insn & 0xffff))
#define UIMM (insn & 0xffff)
#define EA_D ((RA ? gpr[RA] : 0) + (u32)SIMM)
#define EA_X ((RA ? gpr[RA] : 0) + gpr[RB])

static void illegal(u32 insn) {
  die("unsupported instruction 0x%08x at 0x%08x", insn, cur_pc);
}

static void step31(u32 insn) {
  u32 xo = (insn >> 1) & 0x3ff;
  u32 a = gpr[RA], b = gpr[RB], s = gpr[RS], r;
  int oe = (insn >> 10) & 1;

  /* XO-form arithmetic uses a 9-bit opcode with OE in bit 21. */
  switch (xo & 0x1ff) {
  case 266: /* add */
    r = a + b;
    if (oe) set_ov(((~(a ^ b) & (a ^ r)) >> 31) != 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 40: /* subf */
    r = b - a;
    if (oe) set_ov((((a ^ b) & (b ^ r)) >> 31) != 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 10: /* addc */
    r = carry_add(a, b, 0);
    if (oe) set_ov(((~(a ^ b) & (a ^ r)) >> 31) != 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 8: /* subfc */
    r = carry_add(~a, b, 1);
    if (oe) set_ov((((a ^ b) & (b ^ r)) >> 31) != 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 138: /* adde */
    r = carry_add(a, b, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 136: /* subfe */
    r = carry_add(~a, b, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 202: /* addze */
    r = carry_add(a, 0, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 200: /* subfze */
    r = carry_add(~a, 0, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 234: /* addme */
    r = carry_add(a, 0xffffffffu, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 232: /* subfme */
    r = carry_add(~a, 0xffffffffu, (xer & XER_CA) ? 1 : 0);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 104: /* neg */
    r = (u32)0 - a;
    if (oe) set_ov(a == 0x80000000u);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 235: { /* mullw */
    s64 p = (s64)(s32)a * (s32)b;
    r = (u32)p;
    if (oe) set_ov(p != (s32)r);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  }
  case 75: /* mulhw */
    r = (u32)(((s64)(s32)a * (s32)b) >> 32);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 11: /* mulhwu */
    r = (u32)(((u64)a * b) >> 32);
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 491: /* divw */
    if (b == 0 || (a == 0x80000000u && b == 0xffffffffu)) {
      r = 0;
      if (oe) set_ov(1);
    } else {
      r = (u32)((s32)a / (s32)b);
      if (oe) set_ov(0);
    }
    gpr[RT] = r;
    if (RC) record(r);
    return;
  case 459: /* divwu */
    if (b == 0) {
      r = 0;
      if (oe) set_ov(1);
    } else {
      r = a / b;
      if (oe) set_ov(0);
    }
    gpr[RT] = r;
    if (RC) record(r);
    return;
  default:
    break;
  }

  switch (xo) {
  case 0: /* cmp */
  case 32: { /* cmpl */
    int crf = RS >> 2;
    u32 c;
    if (xo == 0)
      c = (s32)a < (s32)b ? 8 : (s32)a > (s32)b ? 4 : 2;
    else
      c = a < b ? 8 : a > b ? 4 : 2;
    if (xer & XER_SO)
      c |= 1;
    set_cr_field(crf, c);
    return;
  }
  case 4: /* tw */
    die("trap at 0x%08x", cur_pc);
    return;
  case 19: /* mfcr */
    gpr[RT] = cr;
    return;
  case 144: { /* mtcrf */
    u32 fxm = (insn >> 12) & 0xff, m = 0;
    int i;
    for (i = 0; i < 8; i++)
      if (fxm & (0x80 >> i))
        m |= 0xf0000000u >> (i * 4);
    cr = (cr & ~m) | (s & m);
    return;
  }
  case 339: { /* mfspr */
    u32 spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
    if (spr == 8)
      gpr[RT] = lr;
    else if (spr == 9)
      gpr[RT] = ctr;
    else if (spr == 1)
      gpr[RT] = xer;
    else
      illegal(insn);
    return;
  }
  case 467: { /* mtspr */
    u32 spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
    if (spr == 8)
      lr = s;
    else if (spr == 9)
      ctr = s;
    else if (spr == 1)
      xer = s;
    else
      illegal(insn);
    return;
  }
  case 371: /* mftb */
    gpr[RT] = (u32)icount;
    return;
  case 512: /* mcrxr */
    set_cr_field(RS >> 2, xer >> 28);
    xer &= 0x0fffffffu;
    return;
  case 28: r = s & b; break;          /* and */
  case 60: r = s & ~b; break;         /* andc */
  case 444: r = s | b; break;         /* or */
  case 412: r = s | ~b; break;        /* orc */
  case 316: r = s ^ b; break;         /* xor */
  case 476: r = ~(s & b); break;      /* nand */
  case 124: r = ~(s | b); break;      /* nor */
  case 284: r = ~(s ^ b); break;      /* eqv */
  case 24: r = (b & 32) ? 0 : s << (b & 31); break;  /* slw */
  case 536: r = (b & 32) ? 0 : s >> (b & 31); break; /* srw */
  case 792: { /* sraw */
    u32 n = b & 63;
    if (n > 31) {
      r = (s32)s < 0 ? 0xffffffffu : 0;
      if ((s32)s < 0) xer |= XER_CA; else xer &= ~XER_CA;
    } else {
      r = (u32)((s32)s >> n);
      if ((s32)s < 0 && n && (s & ((1u << n) - 1)))
        xer |= XER_CA;
      else
        xer &= ~XER_CA;
    }
    break;
  }
  case 824: { /* srawi */
    u32 n = RB;
    r = (u32)((s32)s >> n);
    if ((s32)s < 0 && n && (s & ((1u << n) - 1)))
      xer |= XER_CA;
    else
      xer &= ~XER_CA;
    break;
  }
  case 26: { /* cntlzw */
    u32 n = 0;
    while (n < 32 && !(s & (0x80000000u >> n)))
      n++;
    r = n;
    break;
  }
  case 922: r = (u32)(s32)(int16_t)s; break; /* extsh */
  case 954: r = (u32)(s32)(int8_t)s; break;  /* extsb */

  /* Loads and stores. */
  case 23: gpr[RT] = rd32(EA_X); return;                 /* lwzx */
  case 55: gpr[RT] = rd32(a + b); gpr[RA] = a + b; return; /* lwzux */
  case 87: gpr[RT] = rd8(EA_X); return;                  /* lbzx */
  case 119: gpr[RT] = rd8(a + b); gpr[RA] = a + b; return; /* lbzux */
  case 279: gpr[RT] = rd16(EA_X); return;                /* lhzx */
  case 311: gpr[RT] = rd16(a + b); gpr[RA] = a + b; return;
  case 343: gpr[RT] = (u32)(s32)(int16_t)rd16(EA_X); return; /* lhax */
  case 375:
    gpr[RT] = (u32)(s32)(int16_t)rd16(a + b);
    gpr[RA] = a + b;
    return;
  case 151: wr32(EA_X, s); return;                       /* stwx */
  case 183: wr32(a + b, s); gpr[RA] = a + b; return;     /* stwux */
  case 215: wr8(EA_X, (u8)s); return;                    /* stbx */
  case 247: wr8(a + b, (u8)s); gpr[RA] = a + b; return;
  case 407: wr16(EA_X, (u16)s); return;                  /* sthx */
  case 439: wr16(a + b, (u16)s); gpr[RA] = a + b; return;
  case 534: { /* lwbrx */
    u32 v = rd32(EA_X);
    gpr[RT] = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) |
              (v << 24);
    return;
  }
  case 662: { /* stwbrx */
    u32 v = s;
    wr32(EA_X, (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) |
                   (v << 24));
    return;
  }
  case 790: { /* lhbrx */
    u16 v = rd16(EA_X);
    gpr[RT] = (u16)((v >> 8) | (v << 8));
    return;
  }
  case 918: { /* sthbrx */
    u16 v = (u16)s;
    wr16(EA_X, (u16)((v >> 8) | (v << 8)));
    return;
  }
  case 20: gpr[RT] = rd32(EA_X); return; /* lwarx: one thread */
  case 150: /* stwcx. */
    wr32(EA_X, s);
    set_cr_field(0, 2 | ((xer & XER_SO) ? 1 : 0));
    return;
  case 597: { /* lswi */
    u32 ea = RA ? gpr[RA] : 0, n = RB ? RB : 32, reg = RT, i;
    for (i = 0; i < n; i++) {
      if (i % 4 == 0) {
        if (i) reg = (reg + 1) & 31;
        gpr[reg] = 0;
      }
      gpr[reg] |= (u32)rd8(ea + i) << (24 - 8 * (i % 4));
    }
    return;
  }
  case 725: { /* stswi */
    u32 ea = RA ? gpr[RA] : 0, n = RB ? RB : 32, reg = RS, i;
    for (i = 0; i < n; i++) {
      if (i % 4 == 0 && i)
        reg = (reg + 1) & 31;
      wr8(ea + i, (u8)(gpr[reg] >> (24 - 8 * (i % 4))));
    }
    return;
  }
  case 535: fpr[RT] = as_float(rd32(EA_X)); return;  /* lfsx */
  case 567: fpr[RT] = as_float(rd32(a + b)); gpr[RA] = a + b; return;
  case 599: fpr[RT] = as_double(rd64(EA_X)); return; /* lfdx */
  case 631: fpr[RT] = as_double(rd64(a + b)); gpr[RA] = a + b; return;
  case 663: wr32(EA_X, float_bits((float)fpr[RS])); return; /* stfsx */
  case 695: wr32(a + b, float_bits((float)fpr[RS])); gpr[RA] = a + b; return;
  case 727: wr64(EA_X, double_bits(fpr[RS])); return; /* stfdx */
  case 759: wr64(a + b, double_bits(fpr[RS])); gpr[RA] = a + b; return;
  case 983: wr32(EA_X, (u32)double_bits(fpr[RS])); return; /* stfiwx */

  /* Cache and ordering instructions do nothing here. */
  case 54: case 86: case 246: case 278: case 470: case 598: case 854:
  case 982:
    return;
  case 1014: { /* dcbz: 32-byte lines on the 603 and 604 */
    u32 ea = EA_X & ~31u;
    memset(addr(ea, 32, 1), 0, 32);
    return;
  }
  default:
    illegal(insn);
    return;
  }
  gpr[RA] = r;
  if (RC)
    record(r);
}

static void step63(u32 insn) {
  u32 xo10 = (insn >> 1) & 0x3ff, xo5 = (insn >> 1) & 31;
  double a = fpr[RA], b = fpr[RB], c = fpr[(insn >> 6) & 31];
  int rc = RC;

  switch (xo5) {
  case 18: fpr[RT] = a / b; goto done;
  case 20: fpr[RT] = a - b; goto done;
  case 21: fpr[RT] = a + b; goto done;
  case 22: fpr[RT] = sqrt(b); goto done;
  case 23: fpr[RT] = a >= 0.0 ? c : b; goto done; /* fsel */
  case 25: fpr[RT] = a * c; goto done;
  case 26: fpr[RT] = 1.0 / sqrt(b); goto done;
  case 28: fpr[RT] = fma(a, c, -b); goto done;
  case 29: fpr[RT] = fma(a, c, b); goto done;
  case 30: fpr[RT] = -fma(a, c, -b); goto done;
  case 31: fpr[RT] = -fma(a, c, b); goto done;
  default: break;
  }
  switch (xo10) {
  case 0:  /* fcmpu */
  case 32: /* fcmpo */
    fcompare(RS >> 2, a, b);
    return;
  case 12: fpr[RT] = (double)(float)b; break; /* frsp */
  case 14:
  case 15: { /* fctiw, fctiwz */
    s32 v = to_int32(b, xo10 == 15);
    fpr[RT] = as_double((u64)0xfff80000u << 32 | (u32)v);
    break;
  }
  case 40: fpr[RT] = -b; break;          /* fneg */
  case 72: fpr[RT] = b; break;           /* fmr */
  case 136: fpr[RT] = -fabs(b); break;   /* fnabs */
  case 264: fpr[RT] = fabs(b); break;    /* fabs */
  case 583: fpr[RT] = as_double((u64)0xfff80000u << 32 | fpscr); break;
  case 711: { /* mtfsf */
    u32 flm = (insn >> 17) & 0xff, m = 0, v = (u32)double_bits(b);
    int i;
    for (i = 0; i < 8; i++)
      if (flm & (0x80 >> i))
        m |= 0xf0000000u >> (i * 4);
    fpscr = (fpscr & ~m) | (v & m);
    break;
  }
  case 134: { /* mtfsfi */
    int field = RS >> 2;
    u32 imm = (insn >> 12) & 15, shift = (7 - field) * 4;
    fpscr = (fpscr & ~(0xfu << shift)) | (imm << shift);
    break;
  }
  case 38: fpscr |= 0x80000000u >> RS; break;   /* mtfsb1 */
  case 70: fpscr &= ~(0x80000000u >> RS); break; /* mtfsb0 */
  case 64: { /* mcrfs */
    int src = RA >> 2;
    set_cr_field(RS >> 2, fpscr >> ((7 - src) * 4));
    break;
  }
  default:
    illegal(insn);
  }
  if (rc)
    fp_record();
  return;
done:
  if (rc)
    fp_record();
}

static void step59(u32 insn) {
  u32 xo5 = (insn >> 1) & 31;
  double a = fpr[RA], b = fpr[RB], c = fpr[(insn >> 6) & 31], r;
  switch (xo5) {
  case 18: r = a / b; break;
  case 20: r = a - b; break;
  case 21: r = a + b; break;
  case 22: r = sqrt(b); break;
  case 24: r = 1.0 / b; break;
  case 25: r = a * c; break;
  case 28: r = fma(a, c, -b); break;
  case 29: r = fma(a, c, b); break;
  case 30: r = -fma(a, c, -b); break;
  case 31: r = -fma(a, c, b); break;
  default:
    illegal(insn);
    return;
  }
  fpr[RT] = (double)(float)r;
  if (RC)
    fp_record();
}

static void run(void) {
  for (;;) {
    u32 insn, op;
    cur_pc = pc;
    if (pc - KCALL_BASE < MAX_IMPORTS * 4) {
      kernel_call(pc);
      continue;
    }
    insn = rd32(pc);
    pc += 4;
    icount++;
    op = insn >> 26;
    switch (op) {
    case 3: /* twi */
      die("trap at 0x%08x", cur_pc);
      break;
    case 7: { /* mulli */
      gpr[RT] = (u32)((s32)gpr[RA] * SIMM);
      break;
    }
    case 8: /* subfic */
      gpr[RT] = carry_add(~gpr[RA], (u32)SIMM, 1);
      break;
    case 10: { /* cmpli */
      u32 a = gpr[RA], c = a < UIMM ? 8 : a > UIMM ? 4 : 2;
      if (xer & XER_SO) c |= 1;
      set_cr_field(RS >> 2, c);
      break;
    }
    case 11: { /* cmpi */
      s32 a = (s32)gpr[RA];
      u32 c = a < SIMM ? 8 : a > SIMM ? 4 : 2;
      if (xer & XER_SO) c |= 1;
      set_cr_field(RS >> 2, c);
      break;
    }
    case 12: /* addic */
      gpr[RT] = carry_add(gpr[RA], (u32)SIMM, 0);
      break;
    case 13: /* addic. */
      gpr[RT] = carry_add(gpr[RA], (u32)SIMM, 0);
      record(gpr[RT]);
      break;
    case 14: /* addi */
      gpr[RT] = (RA ? gpr[RA] : 0) + (u32)SIMM;
      break;
    case 15: /* addis */
      gpr[RT] = (RA ? gpr[RA] : 0) + (UIMM << 16);
      break;
    case 16: { /* bc */
      int bo = RS, bi = RA;
      u32 target = (u32)(s32)(int16_t)(insn & 0xfffc);
      if (!(insn & 2))
        target += cur_pc;
      if (insn & 1)
        lr = pc;
      if (cond_ok(bo, bi))
        pc = target;
      break;
    }
    case 17:
      die("sc instruction at 0x%08x: AIX programs call the kernel through "
          "imports from /unix",
          cur_pc);
      break;
    case 18: { /* b */
      u32 li = insn & 0x03fffffc;
      u32 target = (u32)((s32)(li << 6) >> 6);
      if (!(insn & 2))
        target += cur_pc;
      if (insn & 1)
        lr = pc;
      pc = target;
      break;
    }
    case 19: {
      u32 xo = (insn >> 1) & 0x3ff;
      int bt = RS, ba = RA, bb = RB;
      int va = (cr >> (31 - ba)) & 1, vb = (cr >> (31 - bb)) & 1, v = -1;
      switch (xo) {
      case 0: /* mcrf */
        set_cr_field(bt >> 2, cr >> ((7 - (ba >> 2)) * 4));
        break;
      case 16: { /* bclr */
        u32 target = lr & ~3u;
        if (cond_ok(RS, RA)) {
          if (insn & 1)
            lr = pc;
          pc = target;
        } else if (insn & 1) {
          lr = pc;
        }
        break;
      }
      case 528: { /* bcctr */
        u32 target = ctr & ~3u;
        int cond = (RS & 16) ||
                   ((((cr >> (31 - RA)) & 1) != 0) == ((RS & 8) != 0));
        if (insn & 1)
          lr = pc;
        if (cond)
          pc = target;
        break;
      }
      case 33: v = !(va | vb); break;  /* crnor */
      case 129: v = va & !vb; break;   /* crandc */
      case 193: v = va ^ vb; break;    /* crxor */
      case 225: v = !(va & vb); break; /* crnand */
      case 257: v = va & vb; break;    /* crand */
      case 289: v = !(va ^ vb); break; /* creqv */
      case 417: v = va | !vb; break;   /* crorc */
      case 449: v = va | vb; break;    /* cror */
      case 150: break;                 /* isync */
      default:
        illegal(insn);
      }
      if (v >= 0) {
        if (v)
          cr |= 0x80000000u >> bt;
        else
          cr &= ~(0x80000000u >> bt);
      }
      break;
    }
    case 20: { /* rlwimi */
      u32 m = mask((insn >> 6) & 31, (insn >> 1) & 31);
      u32 r = (rotl(gpr[RS], RB) & m) | (gpr[RA] & ~m);
      gpr[RA] = r;
      if (RC) record(r);
      break;
    }
    case 21: { /* rlwinm */
      u32 r = rotl(gpr[RS], RB) & mask((insn >> 6) & 31, (insn >> 1) & 31);
      gpr[RA] = r;
      if (RC) record(r);
      break;
    }
    case 23: { /* rlwnm */
      u32 r = rotl(gpr[RS], gpr[RB] & 31) &
              mask((insn >> 6) & 31, (insn >> 1) & 31);
      gpr[RA] = r;
      if (RC) record(r);
      break;
    }
    case 24: gpr[RA] = gpr[RS] | UIMM; break;          /* ori */
    case 25: gpr[RA] = gpr[RS] | (UIMM << 16); break;  /* oris */
    case 26: gpr[RA] = gpr[RS] ^ UIMM; break;          /* xori */
    case 27: gpr[RA] = gpr[RS] ^ (UIMM << 16); break;  /* xoris */
    case 28: gpr[RA] = gpr[RS] & UIMM; record(gpr[RA]); break;
    case 29: gpr[RA] = gpr[RS] & (UIMM << 16); record(gpr[RA]); break;
    case 31: step31(insn); break;
    case 32: gpr[RT] = rd32(EA_D); break;
    case 33: { u32 ea = gpr[RA] + (u32)SIMM; gpr[RT] = rd32(ea); gpr[RA] = ea; break; }
    case 34: gpr[RT] = rd8(EA_D); break;
    case 35: { u32 ea = gpr[RA] + (u32)SIMM; gpr[RT] = rd8(ea); gpr[RA] = ea; break; }
    case 36: wr32(EA_D, gpr[RS]); break;
    case 37: { u32 ea = gpr[RA] + (u32)SIMM; wr32(ea, gpr[RS]); gpr[RA] = ea; break; }
    case 38: wr8(EA_D, (u8)gpr[RS]); break;
    case 39: { u32 ea = gpr[RA] + (u32)SIMM; wr8(ea, (u8)gpr[RS]); gpr[RA] = ea; break; }
    case 40: gpr[RT] = rd16(EA_D); break;
    case 41: { u32 ea = gpr[RA] + (u32)SIMM; gpr[RT] = rd16(ea); gpr[RA] = ea; break; }
    case 42: gpr[RT] = (u32)(s32)(int16_t)rd16(EA_D); break;
    case 43: { u32 ea = gpr[RA] + (u32)SIMM; gpr[RT] = (u32)(s32)(int16_t)rd16(ea); gpr[RA] = ea; break; }
    case 44: wr16(EA_D, (u16)gpr[RS]); break;
    case 45: { u32 ea = gpr[RA] + (u32)SIMM; wr16(ea, (u16)gpr[RS]); gpr[RA] = ea; break; }
    case 46: { /* lmw */
      u32 ea = EA_D;
      int i;
      for (i = RT; i < 32; i++, ea += 4)
        gpr[i] = rd32(ea);
      break;
    }
    case 47: { /* stmw */
      u32 ea = EA_D;
      int i;
      for (i = RS; i < 32; i++, ea += 4)
        wr32(ea, gpr[i]);
      break;
    }
    case 48: fpr[RT] = as_float(rd32(EA_D)); break;
    case 49: { u32 ea = gpr[RA] + (u32)SIMM; fpr[RT] = as_float(rd32(ea)); gpr[RA] = ea; break; }
    case 50: fpr[RT] = as_double(rd64(EA_D)); break;
    case 51: { u32 ea = gpr[RA] + (u32)SIMM; fpr[RT] = as_double(rd64(ea)); gpr[RA] = ea; break; }
    case 52: wr32(EA_D, float_bits((float)fpr[RS])); break;
    case 53: { u32 ea = gpr[RA] + (u32)SIMM; wr32(ea, float_bits((float)fpr[RS])); gpr[RA] = ea; break; }
    case 54: wr64(EA_D, double_bits(fpr[RS])); break;
    case 55: { u32 ea = gpr[RA] + (u32)SIMM; wr64(ea, double_bits(fpr[RS])); gpr[RA] = ea; break; }
    case 59: step59(insn); break;
    case 63: step63(insn); break;
    default:
      illegal(insn);
    }
  }
}

/* ---- Start-up ------------------------------------------------------------ */

#define STACK_TOP 0x2ff23000u
#define STACK_SIZE (1024u * 1024)

extern char **environ;

int main(int argc, char **argv) {
  int i = 1, n, envc = 0;
  u32 entry, sp, argv_g, envp_g, *argp, *envpp;
  char **e;

  while (i < argc && argv[i][0] == '-') {
    if (!strcmp(argv[i], "-v"))
      trace = 1;
    else if (!strcmp(argv[i], "-k")) {
      size_t k;
      for (k = 0; k < sizeof kcalls / sizeof kcalls[0]; k++)
        printf("%s\n", kcalls[k].name);
      return 0;
    } else if (!strcmp(argv[i], "--")) {
      i++;
      break;
    } else
      die("unknown option %s", argv[i]);
    i++;
  }
  if (i >= argc) {
    fprintf(stderr, "usage: aixrun [-v] [-k] program [args...]\n");
    return 127;
  }

  entry = load(argv[i]);
  add_region(STACK_TOP - STACK_SIZE, STACK_SIZE, 1, "stack");

  /* Strings at the top of the stack, then envp, argv and a frame. */
  n = argc - i;
  for (e = environ; e && *e; e++)
    envc++;
  sp = STACK_TOP;
  argp = calloc(n + 1, sizeof(u32));
  envpp = calloc(envc + 1, sizeof(u32));
  for (int k = 0; k < envc; k++) {
    size_t len = strlen(environ[k]) + 1;
    sp -= (u32)len;
    memcpy(addr(sp, (u32)len, 1), environ[k], len);
    envpp[k] = sp;
  }
  for (int k = n - 1; k >= 0; k--) {
    size_t len = strlen(argv[i + k]) + 1;
    sp -= (u32)len;
    memcpy(addr(sp, (u32)len, 1), argv[i + k], len);
    argp[k] = sp;
  }
  sp &= ~15u;
  sp -= 4 * (envc + 1);
  envp_g = sp;
  for (int k = 0; k <= envc; k++)
    wr32(envp_g + 4 * k, envpp[k]);
  sp -= 4 * (n + 1);
  argv_g = sp;
  for (int k = 0; k <= n; k++)
    wr32(argv_g + 4 * k, argp[k]);
  sp = (sp - 64) & ~15u;
  wr32(sp, 0); /* back chain */

  gpr[1] = sp;
  gpr[2] = rd32(entry + 4);
  gpr[3] = (u32)n;
  gpr[4] = argv_g;
  gpr[5] = envp_g;
  lr = 0;
  pc = rd32(entry);
  run();
  return 0;
}
