/* fuse-overlayfs: Overlay Filesystem in Userspace

   Copyright (C) 2018 Giuseppe Scrivano <giuseppe@scrivano.org>
   Copyright (C) 2018-2020 Red Hat Inc.
   Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#define FUSE_USE_VERSION 32
#define _FILE_OFFSET_BITS 64

#include <config.h>

#include "fuse-overlayfs.h"

#include <fuse_lowlevel.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <dirent.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <sys/ioctl.h>
#include <pwd.h>
#ifdef HAVE_SYS_SENDFILE_H
# include <sys/sendfile.h>
#endif

#include <fuse_overlayfs_error.h>

#include <inttypes.h>
#include <fcntl.h>
#include <hash.h>
#include <sys/statvfs.h>
#include <sys/file.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <linux/fs.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#include <utils.h>
#include <plugin.h>
#include <syslog.h>
#include <libgen.h>
#include <glob.h>
#include <pwd.h>

  // OpenSSL < 1.1.0 or LibreSSL
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
  
  // Equivalent methods
#define EVP_MD_CTX_new EVP_MD_CTX_create
#define EVP_MD_CTX_free EVP_MD_CTX_destroy
#define HMAC_CTX_reset HMAC_CTX_cleanup
  
  // Missing methods (based on 1.1.0 versions)
  HMAC_CTX *HMAC_CTX_new(void) {
    HMAC_CTX *ctx = (HMAC_CTX *)OPENSSL_malloc(sizeof(HMAC_CTX));
    if (ctx != NULL) {
      memset(ctx, 0, sizeof(HMAC_CTX));
      HMAC_CTX_reset(ctx);
    }
    return ctx;
  }
  
  void HMAC_CTX_free(HMAC_CTX *ctx) {
    if (ctx != NULL) {
      HMAC_CTX_cleanup(ctx);
      OPENSSL_free(ctx);
    }
  }
#endif

#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(expression) \
  (__extension__                                                              \
    ({ long int __result;                                                     \
       do __result = (long int) (expression);                                 \
       while (__result == -1L && errno == EINTR);                             \
       __result; }))
#endif

struct fuse_req {
	struct fuse_session *se;
	uint64_t unique;
	int ctr;
	pthread_mutex_t lock;
	struct fuse_ctx ctx;
	struct fuse_chan *ch;
	int interrupted;
	unsigned int ioctl_64bit : 1;
	union {
		struct {
			uint64_t unique;
		} i;
		struct {
			fuse_interrupt_func_t func;
			void *data;
		} ni;
	} u;
	struct fuse_req *next;
	struct fuse_req *prev;
};

typedef struct profile_entry_t {
    struct profile_entry_t *next;
    char *data;
} ProfileEntry;

ProfileEntry *whitelist;
ProfileEntry *nowhitelist;
ProfileEntry *blacklist;
ProfileEntry *mergewhitelist;
ProfileEntry *mergelist;

struct fuse_session *g_fuse_se = NULL;

static bool disable_locking;
static pthread_mutex_t lock;
char hostpid[64];
bool isBoxRunning=false;

struct SSLCipher gSSLCipher;
struct SSLKey gSSLKey;

unsigned int gBlockSize = 1024;
unsigned int gKeyLen = 256;
bool gAllowHoles = true;

const int MAX_IVLENGTH = 16;   // 128 bit (AES block size, Blowfish has 64)

static pid_t gOvlPid = 0;
static pid_t gManagePid = 0;

#define MAX_PATH_STR  1024
#define INVALID_PID -1
#define MAX_READ 8192

static int
enter_big_lock ()
{
  if (disable_locking)
    return 0;

  pthread_mutex_lock (&lock);
  return 1;
}

static int
release_big_lock ()
{
  if (disable_locking)
    return 0;

  pthread_mutex_unlock (&lock);
  return 0;
}

static inline void
cleanup_lockp (int *l)
{
  if (*l == 0)
    return;

  pthread_mutex_unlock (&lock);
  *l = 0;
}

#define cleanup_lock __attribute__((cleanup (cleanup_lockp)))

#ifndef HAVE_OPEN_BY_HANDLE_AT
struct file_handle
{
  unsigned int  handle_bytes;   /* Size of f_handle [in, out] */
  int           handle_type;    /* Handle type [out] */
  unsigned char f_handle[0];    /* File identifier (sized by
				   caller) [out] */
};

int
open_by_handle_at (int mount_fd, struct file_handle *handle, int flags)
{
  return syscall (SYS_open_by_handle_at, mount_fd, handle, flags);
}
#endif

#ifndef RENAME_EXCHANGE
# define RENAME_EXCHANGE (1 << 1)
# define RENAME_NOREPLACE (1 << 2)
#endif

#ifndef RENAME_WHITEOUT
# define RENAME_WHITEOUT (1 << 2)
#endif

#define XATTR_PREFIX "user.fuseoverlayfs."
#define ORIGIN_XATTR "user.fuseoverlayfs.origin"
#define OPAQUE_XATTR "user.fuseoverlayfs.opaque"
#define XATTR_CONTAINERS_PREFIX "user.containers."
#define PRIVILEGED_XATTR_PREFIX "trusted.overlay."
#define PRIVILEGED_OPAQUE_XATTR "trusted.overlay.opaque"
#define PRIVILEGED_ORIGIN_XATTR "trusted.overlay.origin"
#define OPAQUE_WHITEOUT ".wh..wh..opq"
#define WHITEOUT_MAX_LEN (sizeof (".wh.")-1)
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "%s:%d: ", __func__, __LINE__); \
            perror(#x);                                     \
            exit(-1);                                       \
        }                                                   \
    } while (0)

#if !defined FICLONE && defined __linux__
# define FICLONE _IOW (0x94, 9, int)
#endif

#if defined(__GNUC__) && (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 6) && !defined __cplusplus
_Static_assert (sizeof (fuse_ino_t) >= sizeof (uintptr_t),
		"fuse_ino_t too small to hold uintptr_t values!");
#else
struct _uintptr_to_must_hold_fuse_ino_t_dummy_struct
{
  unsigned _uintptr_to_must_hold_fuse_ino_t:
    ((sizeof (fuse_ino_t) >= sizeof (uintptr_t)) ? 1 : -1);
};
#endif

static uid_t overflow_uid;
static gid_t overflow_gid;

static struct ovl_ino dummy_ino;

struct stats_s
{
  size_t nodes;
  size_t inodes;
};

static volatile struct stats_s stats;

void SIGUSR1_handle(int sig)
{
  fprintf (stderr, "Reveice SIGUSR1 signal %d \n", sig);
  isBoxRunning = false;
  char fmt[128];
  int l = snprintf (fmt, sizeof (fmt) - 1, "# INODES: %zu\n# NODES: %zu\n", stats.inodes, stats.nodes);
  fmt[l] = '\0';
  write (STDERR_FILENO, fmt, l + 1);
}

void SIGUSR2_handle(int sig)
{
    fprintf (stderr, "Reveice SIGUSR2 signal %d \n", sig);
    isBoxRunning = true;
}

static double
get_timeout (struct ovl_data *lo)
{
  return lo->timeout;
}

static const struct fuse_opt ovl_opts[] = {
  {"redirect_dir=%s",
   offsetof (struct ovl_data, redirect_dir), 0},
  {"context=%s",
   offsetof (struct ovl_data, context), 0},
  {"lowerdir=%s",
   offsetof (struct ovl_data, lowerdir), 0},
  {"upperdir=%s",
   offsetof (struct ovl_data, upperdir), 0},
  {"workdir=%s",
   offsetof (struct ovl_data, workdir), 0},
  {"uidmapping=%s",
   offsetof (struct ovl_data, uid_str), 0},
  {"gidmapping=%s",
   offsetof (struct ovl_data, gid_str), 0},
  {"timeout=%s",
   offsetof (struct ovl_data, timeout_str), 0},
  {"threaded=%d",
   offsetof (struct ovl_data, threaded), 0},
  {"fsync=%d",
   offsetof (struct ovl_data, fsync), 1},
  {"fast_ino=%d",
   offsetof (struct ovl_data, fast_ino_check), 0},
  {"writeback=%d",
   offsetof (struct ovl_data, writeback), 1},
  {"noxattrs=%d",
   offsetof (struct ovl_data, disable_xattrs), 1},
  {"plugins=%s",
   offsetof (struct ovl_data, plugins), 0},
  {"xattr_permissions=%d",
   offsetof (struct ovl_data, xattr_permissions), 0},
  {"squash_to_root",
   offsetof (struct ovl_data, squash_to_root), 1},
  {"squash_to_uid=%d",
   offsetof (struct ovl_data, squash_to_uid), 1},
  {"squash_to_gid=%d",
   offsetof (struct ovl_data, squash_to_gid), 1},
  {"static_nlink",
   offsetof (struct ovl_data, static_nlink), 1},
  {"volatile",  /* native overlay supports "volatile" to mean fsync=0.  */
   offsetof (struct ovl_data, fsync), 0},
  FUSE_OPT_END
};

/* The current process has enough privileges to use mknod.  */
static bool can_mknod = true;

/* Kernel definitions.  */

typedef unsigned char u8;
typedef unsigned char uuid_t[16];

/* The type returned by overlay exportfs ops when encoding an ovl_fh handle */
#define OVL_FILEID	0xfb

/* On-disk and in-memory format for redirect by file handle */
struct ovl_fh
{
  u8 version;  /* 0 */
  u8 magic;    /* 0xfb */
  u8 len;      /* size of this header + size of fid */
  u8 flags;    /* OVL_FH_FLAG_* */
  u8 type;     /* fid_type of fid */
  uuid_t uuid; /* uuid of filesystem */
  u8 fid[0];   /* file identifier */
} __packed;

static struct ovl_data *
ovl_data (fuse_req_t req)
{
  return (struct ovl_data *) fuse_req_userdata (req);
}

static unsigned long
get_next_wd_counter ()
{
  static unsigned long counter = 1;
  return counter++;
}

static ino_t
node_to_inode (struct ovl_node *n)
{
  return (ino_t) n->ino;
}

static struct ovl_ino *
lookup_inode (struct ovl_data *lo, ino_t n)
{
  return (struct ovl_ino *) n;
}

static struct ovl_node *
inode_to_node (struct ovl_data *lo, ino_t n)
{
  return lookup_inode (lo, n)->node;
}

static void
check_can_mknod (struct ovl_data *lo)
{
  int ret;
  char path[PATH_MAX];

  if (getenv ("FUSE_OVERLAYFS_DISABLE_OVL_WHITEOUT"))
    {
      can_mknod = false;
      return;
    }

  sprintf (path, "%lu", get_next_wd_counter ());

  ret = mknodat (lo->workdir_fd, path, S_IFCHR|0700, makedev (0, 0));
  if (ret == 0)
    unlinkat (lo->workdir_fd, path, 0);
  if (ret < 0 && errno == EPERM)
    can_mknod = false;
}

static struct ovl_mapping *
read_mappings (const char *str)
{
  char *buf = NULL, *saveptr = NULL, *it, *endptr;
  struct ovl_mapping *tmp, *ret = NULL;
  unsigned int a, b, c;
  int state = 0;

  buf = alloca (strlen (str) + 1);
  strcpy (buf, str);

  for (it = strtok_r (buf, ":", &saveptr); it; it = strtok_r (NULL, ":", &saveptr))
    {
      switch (state)
        {
        case 0:
          a = strtol (it, &endptr, 10);
          if (*endptr != 0)
            error (EXIT_FAILURE, 0, "invalid mapping specified: %s", str);
          state++;
          break;

        case 1:
          b = strtol (it, &endptr, 10);
          if (*endptr != 0)
            error (EXIT_FAILURE, 0, "invalid mapping specified: %s", str);
          state++;
          break;

        case 2:
          c = strtol (it, &endptr, 10);
          if (*endptr != 0)
            error (EXIT_FAILURE, 0, "invalid mapping specified: %s", str);
          state = 0;

          tmp = malloc (sizeof (*tmp));
          if (tmp == NULL)
            return NULL;
          tmp->next = ret;
          tmp->host = a;
          tmp->to = b;
          tmp->len = c;
          ret = tmp;
          break;
        }
    }

  if (state != 0)
    error (EXIT_FAILURE, 0, "invalid mapping specified: %s", str);

  return ret;
}

static void
free_mapping (struct ovl_mapping *it)
{
  struct ovl_mapping *next = NULL;
  for (; it; it = next)
    {
      next = it->next;
      free (it);
    }
}

/* Useful in a gdb session.  */
void
dump_directory (struct ovl_node *node)
{
  struct ovl_node *it;

  if (node->children == NULL)
    return;

  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    printf ("ENTRY: %s (%s)\n", it->name, it->path);
}

static long int
read_file_as_int (const char *file)
{
  cleanup_close int fd = -1;
  long int ret;
  char buffer[256];
  int r;

  fd = open (file, O_RDONLY);
  if (fd < 0)
    error (EXIT_FAILURE, errno, "can't open %s", file);

  r = read (fd, buffer, sizeof (buffer) - 1);
  if (r < 0)
    error (EXIT_FAILURE, errno, "can't read from %s", file);
  buffer[r] = '\0';

  ret = strtol (buffer, NULL, 10);
  if (ret == 0)
    error (EXIT_FAILURE, errno, "can't parse %s", file);

  return ret;
}

static void
read_overflowids (void)
{
  overflow_uid = read_file_as_int ("/proc/sys/kernel/overflowuid");
  overflow_gid = read_file_as_int ("/proc/sys/kernel/overflowgid");
}

static bool
ovl_debug (fuse_req_t req)
{
  return ovl_data (req)->debug != 0;
}

static void
ovl_init (void *userdata, struct fuse_conn_info *conn)
{
  struct ovl_data *lo = (struct ovl_data *) userdata;

  if ((conn->capable & FUSE_CAP_WRITEBACK_CACHE) == 0)
    lo->writeback = 0;

  if (conn->capable & FUSE_CAP_POSIX_ACL)
    conn->want |= FUSE_CAP_POSIX_ACL;

  conn->want |= FUSE_CAP_DONT_MASK | FUSE_CAP_SPLICE_READ | FUSE_CAP_SPLICE_WRITE | FUSE_CAP_SPLICE_MOVE;
  if (lo->writeback)
    conn->want |= FUSE_CAP_WRITEBACK_CACHE;
}

static struct ovl_layer *
get_first_layer (struct ovl_data *lo)
{
  return lo->layers;
}

static struct ovl_layer *
get_upper_layer (struct ovl_data *lo)
{
  if (lo->upperdir == NULL)
    return NULL;

  return lo->layers;
}

static struct ovl_layer *
get_lower_layers (struct ovl_data *lo)
{
  if (lo->upperdir == NULL)
    return lo->layers;

  return lo->layers->next;
}

static inline bool
node_dirp (struct ovl_node *n)
{
  return n->children != NULL;
}

static int
node_dirfd (struct ovl_node *n)
{
  if (n->hidden)
    return n->hidden_dirfd;
  return n->layer->fd;
}

static bool
has_prefix (const char *str, const char *pref)
{
  while (1)
    {
      if (*pref == '\0')
        return true;
      if (*str == '\0')
        return false;
      if (*pref != *str)
        return false;
      str++;
      pref++;
    }
  return false;
}

static bool
can_access_xattr (const char *name)
{
  return !has_prefix (name, XATTR_PREFIX)               \
    && !has_prefix (name, PRIVILEGED_XATTR_PREFIX);
}

static ssize_t
write_permission_xattr (struct ovl_data *lo, int fd, const char *path, uid_t uid, gid_t gid, mode_t mode)
{
  char buf[64];
  size_t len;
  int ret;
  const char *name = NULL;

  switch (lo->xattr_permissions)
    {
    case 0:
      return 0;

    case 1:
    name = XATTR_PRIVILEGED_OVERRIDE_STAT;
    break;

    case 2:
      name = XATTR_OVERRIDE_STAT;
      break;

    default:
      errno = EINVAL;
      return -1;
    }

  if (path == NULL && fd < 0)
    {
      errno = EINVAL;
      return -1;
    }

  len = sprintf (buf, "%d:%d:%o", uid, gid, mode);
  if (fd >= 0)
    return fsetxattr (fd, name, buf, len, 0);

  ret = lsetxattr (path, name, buf, len, 0);
  /* Ignore EPERM in unprivileged mode.  */
  if (ret < 0 && lo->xattr_permissions == 2 && errno == EPERM)
    return 0;
  return ret;
}

static int
do_fchown (struct ovl_data *lo, int fd, uid_t uid, gid_t gid, mode_t mode)
{
  int ret;
  if (lo->xattr_permissions)
    ret = write_permission_xattr (lo, fd, NULL, uid, gid, mode);
  else
    ret = fchown (fd, uid, gid);
  return (lo->squash_to_root || lo->squash_to_uid != -1 || lo->squash_to_gid != -1) ? 0 : ret;
}
/* Make sure it is not used anymore.  */
#define fchown ERROR

static int
do_chown (struct ovl_data *lo, const char *path, uid_t uid, gid_t gid, mode_t mode)
{
  int ret;
  if (lo->xattr_permissions)
    ret = write_permission_xattr (lo, -1, path, uid, gid, mode);
  else
    ret = chown (path, uid, gid);
  return (lo->squash_to_root || lo->squash_to_uid != -1 || lo->squash_to_gid != -1) ? 0 : ret;
}
/* Make sure it is not used anymore.  */
#define chown ERROR

static int
do_fchownat (struct ovl_data *lo, int dfd, const char *path, uid_t uid, gid_t gid, mode_t mode, int flags)
{
  int ret;
  if (lo->xattr_permissions)
    {
      char proc_path[32];
      cleanup_close int fd = openat (dfd, path, O_NOFOLLOW|O_PATH);
      if (fd < 0)
        return fd;

      sprintf (proc_path, "/proc/self/fd/%d", fd);
      ret = write_permission_xattr (lo, -1, proc_path, uid, gid, mode);
    }
  else
    ret = fchownat (dfd, path, uid, gid, flags);
  return (lo->squash_to_root || lo->squash_to_uid != -1 || lo->squash_to_gid != -1) ? 0 : ret;
}
/* Make sure it is not used anymore.  */
#define fchownat ERROR

static int
do_fchmod (struct ovl_data *lo, int fd, mode_t mode)
{
  if (lo->xattr_permissions)
    {
      struct ovl_layer *upper = get_upper_layer (lo);
      struct stat st;

      if (upper == NULL)
        {
          errno = EROFS;
          return -1;
        }

      st.st_uid = 0;
      st.st_gid = 0;
      if (override_mode (upper, fd, NULL, NULL, &st) < 0 && errno != ENODATA)
        return -1;

      return write_permission_xattr (lo, fd, NULL, st.st_uid, st.st_gid, mode);
    }
  return fchmod (fd, mode);
}
/* Make sure it is not used anymore.  */
#define fchmod ERROR

static int
do_chmod (struct ovl_data *lo, const char *path, mode_t mode)
{
  if (lo->xattr_permissions)
    {
      struct ovl_layer *upper = get_upper_layer (lo);
      struct stat st;

      if (upper == NULL)
        {
          errno = EROFS;
          return -1;
        }

      st.st_uid = 0;
      st.st_gid = 0;
      if (override_mode (upper, -1, path, NULL, &st) < 0 && errno != ENODATA)
        return -1;

      return write_permission_xattr (lo, -1, path, st.st_uid, st.st_gid, mode);
    }
  return chmod (path, mode);
}
/* Make sure it is not used anymore.  */
#define chmod ERROR

static int
set_fd_origin (int fd, const char *origin)
{
  cleanup_close int opq_whiteout_fd = -1;
  size_t len = strlen (origin) + 1;
  int ret;

  ret = fsetxattr (fd, ORIGIN_XATTR, origin, len, 0);
  if (ret < 0)
    {
      if (errno == ENOTSUP)
        return 0;
    }
  return ret;
}

static int
set_fd_opaque (int fd)
{
  cleanup_close int opq_whiteout_fd = -1;
  int ret;

  ret = fsetxattr (fd, PRIVILEGED_OPAQUE_XATTR, "y", 1, 0);
  if (ret < 0)
    {
      if (errno == ENOTSUP)
        goto create_opq_whiteout;
      if (errno != EPERM || (fsetxattr (fd, OPAQUE_XATTR, "y", 1, 0) < 0 && errno != ENOTSUP))
          return -1;
    }
 create_opq_whiteout:
  opq_whiteout_fd = TEMP_FAILURE_RETRY (safe_openat (fd, OPAQUE_WHITEOUT, O_CREAT|O_WRONLY|O_NONBLOCK, 0700));
  return (opq_whiteout_fd >= 0 || ret == 0) ? 0 : -1;
}

static int
is_directory_opaque (struct ovl_layer *l, const char *path)
{
  char b[16];
  ssize_t s;

  s = l->ds->getxattr (l, path, PRIVILEGED_OPAQUE_XATTR, b, sizeof (b));
  if (s < 0 && errno == ENODATA)
    s = l->ds->getxattr (l, path, OPAQUE_XATTR, b, sizeof (b));

  if (s < 0)
    {
      if (errno == ENOTSUP || errno == ENODATA)
        {
          char whiteout_opq_path[PATH_MAX];

          strconcat3 (whiteout_opq_path, PATH_MAX, path, "/" OPAQUE_WHITEOUT, NULL);

          if (l->ds->file_exists (l, whiteout_opq_path) == 0)
            return 1;

          return (errno == ENOENT) ? 0 : -1;
        }
      return -1;
    }

  return b[0] == 'y' ? 1 : 0;
}

static int
create_whiteout (struct ovl_data *lo, struct ovl_node *parent, const char *name, bool skip_mknod, bool force_create)
{
  char whiteout_wh_path[PATH_MAX];
  cleanup_close int fd = -1;
  int ret;

  if (! force_create)
    {
      char path[PATH_MAX];
      struct ovl_layer *l;
      bool found = false;

      strconcat3 (path, PATH_MAX, parent->path, "/", name);

      for (l = get_lower_layers (lo); l; l = l->next)
        {
          ret = l->ds->file_exists (l, path);
          if (ret < 0 && errno == ENOENT)
            continue;

          found = true;
          break;
        }
      /* Not present in the lower layers, do not do anything.  */
      if (!found)
        return 0;
    }

  if (!skip_mknod && can_mknod)
    {
      char whiteout_path[PATH_MAX];

      strconcat3 (whiteout_path, PATH_MAX, parent->path, "/", name);

      ret = mknodat (get_upper_layer (lo)->fd, whiteout_path, S_IFCHR|0700, makedev (0, 0));
      if (ret == 0)
        return 0;

      if (errno == EEXIST)
        {
          int saved_errno = errno;
          struct stat st;

          /* Check whether it is already a whiteout.  */
          if (TEMP_FAILURE_RETRY (fstatat (get_upper_layer (lo)->fd, whiteout_path, &st, AT_SYMLINK_NOFOLLOW)) == 0
              && (st.st_mode & S_IFMT) == S_IFCHR
              && major (st.st_rdev) == 0
              && minor (st.st_rdev) == 0)
            return 0;

          errno = saved_errno;
        }

      if (errno != EPERM && errno != ENOTSUP)
        return -1;

      /* if it fails with EPERM then do not attempt mknod again.  */
      can_mknod = false;
    }

  strconcat3 (whiteout_wh_path, PATH_MAX, parent->path, "/.wh.", name);

  fd = get_upper_layer (lo)->ds->openat (get_upper_layer (lo), whiteout_wh_path, O_CREAT|O_WRONLY|O_NONBLOCK, 0700);
  if (fd < 0 && errno != EEXIST)
    return -1;

  return 0;
}

static int
delete_whiteout (struct ovl_data *lo, int dirfd, struct ovl_node *parent, const char *name)
{
  struct stat st;

  if (can_mknod)
    {
      if (dirfd >= 0)
        {
          if (TEMP_FAILURE_RETRY (fstatat (dirfd, name, &st, AT_SYMLINK_NOFOLLOW)) == 0
              && (st.st_mode & S_IFMT) == S_IFCHR
              && major (st.st_rdev) == 0
              && minor (st.st_rdev) == 0)
            {
              if (unlinkat (dirfd, name, 0) < 0)
                return -1;
            }
        }
      else
        {
          char whiteout_path[PATH_MAX];

          strconcat3 (whiteout_path, PATH_MAX, parent->path, "/", name);

          if (get_upper_layer (lo)->ds->statat (get_upper_layer (lo), whiteout_path, &st, AT_SYMLINK_NOFOLLOW, STATX_MODE|STATX_TYPE) == 0
              && (st.st_mode & S_IFMT) == S_IFCHR
              && major (st.st_rdev) == 0
              && minor (st.st_rdev) == 0)
            {
              if (unlinkat (get_upper_layer (lo)->fd, whiteout_path, 0) < 0)
                return -1;
            }
        }
    }

  /* Look for the .wh. alternative as well.  */

  if (dirfd >= 0)
    {
      char whiteout_path[PATH_MAX];

      strconcat3 (whiteout_path, PATH_MAX, ".wh.", name, NULL);

      if (unlinkat (dirfd, whiteout_path, 0) < 0 && errno != ENOENT)
        return -1;
    }
  else
    {
      char whiteout_path[PATH_MAX];

      strconcat3 (whiteout_path, PATH_MAX, parent->path, "/.wh.", name);

      if (unlinkat (get_upper_layer (lo)->fd, whiteout_path, 0) < 0 && errno != ENOENT)
        return -1;
    }

  return 0;
}

/*
int getParentPid(int pid, int *decision)
{
    char dir[1024]={0};
    char statPath[1024] = {0};
    char buf[1024] = {0};
    int rpid=0;
    int fpid=0;
    char fpath[1024]={0};
    struct stat st;
    ssize_t ret =0;

    sprintf(dir,"/proc/%d/",pid);

    sprintf(statPath,"%sstat",dir);

    if(stat(statPath,&st)!=0)
    {
        return -2; 
    }

    memset(buf,0,strlen(buf));

    FILE * fp = fopen(statPath,"r");

    ret += fread(buf + ret,1,300-ret,fp);

    fclose(fp);

    sscanf(buf,"%*d %*c%s %*c %d %*s",fpath,&fpid);

    fpath[strlen(fpath)-1]='\0';
    
    if(strncmp(fpath, "firejail", strlen("firejail"))==0
       || strncmp(fpath, "encfs", strlen("encfs"))==0
       || strncmp(fpath, "fuse-overlayfs", strlen("fuse-overlayfs"))==0)
    {
        *decision=1;
    }
    
    if(strcmp(fpath,"bash")!=0 && strcmp(fpath,"sudo")!=0 ) //bash �ն� sudo �ն�
    {
        if(fpid==1)
        {
            *decision=0;
            return pid;
        }
        else if(fpid==2)
        {
            *decision=0;
            return -1; //�ں��߳�
        }
        rpid = getParentPid(fpid, decision);
        if(rpid == 0)
        {
            rpid = pid;
        }
    }

    return rpid;
}

static int checkAuthority(int pid_guest) {
    //int decision = 0;
    char guestpid[64];
    char path[64];
    int pid;
    int len;

    if (hostpid[0] == 0)
    {
        pid = getpid();
        snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid);
        if ((len = readlink(path, hostpid, sizeof(hostpid)-1)) != -1)
        {
            hostpid[len] = '\0';
        }
    }

    snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid_guest);
    if ((len = readlink(path, guestpid, sizeof(guestpid)-1)) != -1)
    {
        guestpid[len] = '\0';
    }

    //box����
    if (strcmp(guestpid, hostpid) == 0) {
        return 1;
    }
    //box����
    else{
        return 1;
    }
    //getParentPid(pid, &decision);

    //return decision;
}
*/

static pid_t getParentPid(pid_t pid)
{
  char statPath[MAX_PATH_STR] = {0};
  char buf[MAX_PATH_STR] = {0};
  char procName[MAX_PATH_STR]={0};
  int fpid=0;
  struct stat st;
  ssize_t ret =0;

  if (pid < 0)
      return INVALID_PID;

  sprintf(statPath,"/proc/%d/stat",pid);

  if(stat(statPath,&st)!=0)
  {
      return INVALID_PID; 
  }

  FILE * fp = fopen(statPath,"r");
  ret += fread(buf + ret,1,300-ret,fp);
  fclose(fp);

  sscanf(buf,"%*d %*c%s %*c %d %*s",procName,&fpid);

  if (strlen(procName) > 0) {
      procName[strlen(procName)-1]='\0';
  }
  //syslog(LOG_INFO, "procName=%s\n", procName);

  return fpid;
}

int isInBox(fuse_req_t req, pid_t accessPid)
{
    char statPath[MAX_PATH_STR] = {0};
    char buf[MAX_PATH_STR] = {0};
    char procName[MAX_PATH_STR]={0};
    char accessprocName[MAX_PATH_STR]={0};
    pid_t pid =accessPid;
    pid_t fpid = 0;
    struct stat st;
    ssize_t ret = 0;

    while(1)
    {
        //idle
        if(pid==0)
        {
            return true;
        }
        //systemd
        if(pid==1)
        {
            if (UNLIKELY (ovl_debug (req)))
	    {
                syslog(LOG_INFO, "systemd:%d, accessprocName=%s\n", accessPid, accessprocName);
	    }
            return false;
        }
        //kthreadd
        if(pid==2)
        {
            if (UNLIKELY (ovl_debug (req)))
	    {
                syslog(LOG_INFO, "kthreadd:%d, accessprocName=%s\n", accessPid, accessprocName);
	    }
            return true;
        }

        if(pid==gManagePid)
        {
            //syslog(LOG_INFO, "pid=%d, gManagePid=%d\n", pid, gManagePid);
            return true;
        }

        sprintf(statPath,"/proc/%d/stat",pid);
        if(stat(statPath,&st)!=0)
        {
            syslog(LOG_INFO, "stat fail:%s\n", statPath);
            return false;
        }

        FILE * fp = fopen(statPath,"r");
        ret += fread(buf + ret,1,300-ret,fp);
        fclose(fp);

        sscanf(buf,"%*d %*c%s %*c %d %*s",procName,&fpid);
        procName[strlen(procName)-1]='\0';

	if (pid == accessPid)
	{
     	    strcpy(accessprocName, procName);
	}

        //allow firejail --join
        if(strncmp(procName, "firejail", strlen("firejail"))==0)
        {
            return true;
        }

        if(strncmp(procName, "EnDeskTop", strlen("EnDeskTop"))==0)
        {
            return true;
        }

        if(strncmp(procName, "uebm", strlen("uebm"))==0)
        {
            return true;
        }

        if(strncmp(procName, "StreamTran", strlen("StreamTran"))==0)
        {
            return true;
        }

        if(strncmp(procName, "BgIOThr~Poo", strlen("BgIOThr~Poo"))==0)
        {
            return true;
        }
        

        if(strncmp(procName, "TaskCon~lle", strlen("TaskCon~lle"))==0)
        {
            return true;

        }

        if(strncmp(procName, "apport", strlen("apport"))==0)
        {
            return true;

        }

        if(strncmp(procName, "Backgro~Poo", strlen("Backgro~Poo"))==0)
        {
            return true;

        }
        pid = fpid;
        memset(statPath,0,strlen(statPath));
        memset(buf,0,strlen(buf));
        memset(procName,0,strlen(procName));
        ret = 0;
    }

    syslog(LOG_INFO, "End!\n");
    return false;
}

static int checkAuthority(fuse_req_t req, fuse_ino_t ino)
{
    int fpid = 0;
    bool flag = false;

    if (UNLIKELY (ovl_debug (req)))
    {
        fprintf (stderr, "checkAuthority(pid=%d gManagePid=%d)\n", req->ctx.pid, gManagePid);
    }

    if (ino == FUSE_ROOT_ID)
    {
        return true;
    }


    flag = isInBox(req, req->ctx.pid);
    if(!flag)
    {
        fprintf (stderr, "checkAuthority deny!\n");
        syslog(LOG_INFO, "checkAuthority deny!\n");
    }
    //flag = true;
    return flag;
}

//防止死循环挂载
int checkPath(struct ovl_data *lo, char *path) 
{
    char *dirc;
    char *dname;

    dirc = strdup(lo->mountpoint);
    dname = dirname(dirc);

    if (0 == strcmp(path, dname+1)) {
        fprintf(stderr, "CheckPath deny, path=%s\n", path);
        //syslog(LOG_INFO, "CheckPath deny, path=%s\n", path);
        return 0;
    }
    else
    {
        return 1;
    }
}

int checkAccess(fuse_req_t req, struct ovl_data *lo, char *nodePath) {
    char guestpid[64];
    char path[64];
    int pid;
    int len;
    //FILE *fp;
    //char buf[PIPE_BUF];
    //char command[150];
    //int count;

    /*
    if (UNLIKELY (ovl_debug (req)))
    {
        fprintf(stderr, "checkAccess path=%s\n", nodePath);
    }
    */

    if (hostpid[0] == 0)
    {
        pid = getpid();
        snprintf(path, sizeof(path), "/proc/%d/ns/pid", pid);
        if ((len = readlink(path, hostpid, sizeof(hostpid)-1)) != -1)
        {
            hostpid[len] = '\0';
        }
    }

    snprintf(path, sizeof(path), "/proc/%d/ns/pid", req->ctx.pid);
    if ((len = readlink(path, guestpid, sizeof(guestpid)-1)) != -1)
    {
        guestpid[len] = '\0';
    }

    if (strcmp(guestpid, hostpid) == 0) {
        if(isBoxRunning)
        {
            return 0;
        }
        else
        {
            return 1;
        }
        /*
        sprintf(command, "ps -ef |grep firejail | grep -v grep | wc -l");
        
        if((fp=popen(command, "r"))==NULL)
        {
            return 1;
        }

        if((fgets(buf, PIPE_BUF, fp))==NULL)
        {
            pclose(fp);
            return 1;
        }

        count=atoi(buf);
        if(count>0)
        {
            pclose(fp);
            return 0;
        }
        else
        {
            pclose(fp);
            return 1;
        }
        */

        /*
        if (UNLIKELY (ovl_debug (req)))
        {
            fprintf(stderr, "checkAccess strcmp same\n");
        }


        */
    }
    else{
        return checkPath(lo, nodePath);
    }
}

static unsigned int
find_mapping (unsigned int id, const struct ovl_data *data,
              bool direct, bool uid)
{
  const struct ovl_mapping *mapping = (uid ? data->uid_mappings
                                       : data->gid_mappings);

  if (direct && uid && data->squash_to_uid != -1)
    return data->squash_to_uid;
  if (direct && !uid && data->squash_to_gid != -1)
    return data->squash_to_gid;
  if (direct && data->squash_to_root)
    return 0;

  if (mapping == NULL)
    return id;
  for (; mapping; mapping = mapping->next)
    {
      if (direct)
        {
          if (id >= mapping->host && id < mapping->host + mapping->len)
            return mapping->to + (id - mapping->host);
        }
      else
        {
          if (id >= mapping->to && id < mapping->to + mapping->len)
            return mapping->host + (id - mapping->to);
        }
    }
  return uid ? overflow_uid : overflow_gid;
}

static uid_t
get_uid (struct ovl_data *data, uid_t id)
{
  return find_mapping (id, data, false, true);
}

static uid_t
get_gid (struct ovl_data *data, gid_t id)
{
  return find_mapping (id, data, false, false);
}

static int
rpl_stat (fuse_req_t req, struct ovl_node *node, int fd, const char *path, struct stat *st_in, struct stat *st)
{
  int ret = 0;
  struct ovl_layer *l = node->layer;
  struct ovl_data *data = ovl_data (req);

  if (st_in)
    memcpy (st, st_in, sizeof (* st));
  else if (fd >= 0)
    ret = l->ds->fstat (l, fd, path, STATX_BASIC_STATS, st);
  else if (path != NULL)
    ret = stat (path, st);
  else if (node->hidden)
    ret = fstatat (node_dirfd (node), node->path, st, AT_SYMLINK_NOFOLLOW);
  else
    ret = l->ds->statat (l, node->path, st, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS);

  if (ret < 0)
    return ret;

  st->st_uid = find_mapping (st->st_uid, data, true, true);
  st->st_gid = find_mapping (st->st_gid, data, true, false);

  st->st_ino = node->tmp_ino;
  st->st_dev = node->tmp_dev;
  if (ret == 0 && node_dirp (node))
    {
      if (!data->static_nlink)
        {
          struct ovl_node *it;

          st->st_nlink = 2;

          for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
            {
              if (node_dirp (it))
                st->st_nlink++;
            }
        }
      else
        st->st_nlink = 1;
    }

  return ret;
}

static void
node_mark_all_free (void *p)
{
  struct ovl_node *it, *n = (struct ovl_node *) p;

  for (it = n->next_link; it; it = it->next_link)
    it->ino->lookups = 0;

  n->ino->lookups = 0;

  if (n->children)
    {
      for (it = hash_get_first (n->children); it; it = hash_get_next (n->children, it))
        node_mark_all_free (it);
    }
}

static void
node_free (void *p)
{
  struct ovl_node *n = (struct ovl_node *) p;

  if (n->parent)
    {
      if (n->parent->children && hash_lookup (n->parent->children, n) == n)
        hash_delete (n->parent->children, n);
      n->parent->loaded = 0;
      n->parent = NULL;
    }

  if ((n->ino && n->ino != &dummy_ino) || n->node_lookups > 0)
    return;

  if (n->children)
    {
      struct ovl_node *it;

      for (it = hash_get_first (n->children); it; it = hash_get_next (n->children, it))
        it->parent = NULL;

      hash_free (n->children);
      n->children = NULL;
    }

  if (n->do_unlink)
    unlinkat (n->hidden_dirfd, n->path, 0);
  if (n->do_rmdir)
    unlinkat (n->hidden_dirfd, n->path, AT_REMOVEDIR);

  stats.nodes--;
  free (n->name);
  free (n->path);
  free (n->cache.data);

  EVP_CIPHER_CTX_free(n->block_enc);
  EVP_CIPHER_CTX_free(n->block_dec);
  EVP_CIPHER_CTX_free(n->stream_enc);
  EVP_CIPHER_CTX_free(n->stream_dec);

  pthread_mutex_destroy(&n->mutex);
  
  free (n);
}

static void
inode_free (void *p)
{
  struct ovl_node *n, *tmp;
  struct ovl_ino *i = (struct ovl_ino *) p;

  n = i->node;
  while (n)
    {
      tmp = n;
      n = n->next_link;

      tmp->ino = NULL;
      node_free (tmp);
  }

  stats.inodes--;
  free (i);
}

static void
drop_node_from_ino (Hash_table *inodes, struct ovl_node *node)
{
  struct ovl_ino *ino;
  struct ovl_node *it, *prev = NULL;

  ino = node->ino;

  if (ino->lookups == 0)
    {
      hash_delete (inodes, ino);
      inode_free (ino);
      return;
    }

  /* If it is the only node referenced by the inode, do not destroy it.  */
  if (ino->node == node && node->next_link == NULL)
    return;

  node->ino = NULL;

  for (it = ino->node; it; it = it->next_link)
    {
      if (it == node)
        {
          if (prev)
            prev->next_link = it->next_link;
          else
            ino->node = it->next_link;
          break;
        }
      prev = it;
    }
}

static int
direct_renameat2 (int olddirfd, const char *oldpath,
                  int newdirfd, const char *newpath, unsigned int flags)
{
  return syscall (SYS_renameat2, olddirfd, oldpath, newdirfd, newpath, flags);
}

static int
hide_node (struct ovl_data *lo, struct ovl_node *node, bool unlink_src)
{
  char *newpath = NULL;
  int ret;

  ret = asprintf (&newpath, "%lu", get_next_wd_counter ());
  if (ret < 0)
    return ret;

  assert (node->layer == get_upper_layer (lo));

  if (unlink_src)
    {
      bool moved = false;
      bool whiteout_created = false;
      bool needs_whiteout;

      needs_whiteout = (node->last_layer != get_upper_layer (lo)) && (node->parent && node->parent->last_layer != get_upper_layer (lo));
      if (!needs_whiteout && node_dirp (node))
        {
          ret = is_directory_opaque (get_upper_layer (lo), node->path);
          if (ret < 0)
            return ret;
          if (ret)
            needs_whiteout = true;
        }

      // if the parent directory is opaque, there's no need to put a whiteout in it.
      if (node->parent != NULL)
        needs_whiteout = needs_whiteout && (is_directory_opaque(get_upper_layer(lo), node->parent->path) < 1);

      if (needs_whiteout)
        {
          /* If the atomic rename+mknod failed, then fallback into doing it in two steps.  */
          if (can_mknod && syscall (SYS_renameat2, node_dirfd (node), node->path, lo->workdir_fd, newpath, RENAME_WHITEOUT) == 0)
            {
              whiteout_created = true;
              moved = true;
            }

          if (!whiteout_created)
            {
              if (node->parent)
                {
                  /* If we are here, it means we have no permissions to use mknod.  Also
                     since the file is not yet moved, creating a whiteout would fail on
                     the mknodat call.  */
                  if (create_whiteout (lo, node->parent, node->name, true, false) < 0)
                    return -1;
                }
            }
        }

      if (!moved)
        {
          if (renameat (node_dirfd (node), node->path, lo->workdir_fd, newpath) < 0)
            return -1;
        }
    }
  else
    {
      if (node_dirp (node))
        {
          if (mkdirat (lo->workdir_fd, newpath, 0700) < 0)
            return -1;
        }
      else
        {
          if (linkat (node_dirfd (node), node->path, lo->workdir_fd, newpath, 0) < 0)
            return -1;
        }
    }
  drop_node_from_ino (lo->inodes, node);

  node->hidden_dirfd = lo->workdir_fd;
  free (node->path);
  node->path = newpath;
  newpath = NULL;  /* Do not auto cleanup.  */

  node->hidden = 1;
  if (node->parent)
    node->parent->loaded = 0;
  node->parent = NULL;

  if (node_dirp (node))
    node->do_rmdir = 1;
  else
    node->do_unlink = 1;
  return 0;
}

static size_t
node_inode_hasher (const void *p, size_t s)
{
  struct ovl_ino *n = (struct ovl_ino *) p;

  return (n->ino ^ n->dev) % s;
}

static bool
node_inode_compare (const void *n1, const void *n2)
{
  struct ovl_ino *i1 = (struct ovl_ino *) n1;
  struct ovl_ino *i2 = (struct ovl_ino *) n2;

  return i1->ino == i2->ino && i1->dev == i2->dev;
}

static size_t
node_hasher (const void *p, size_t s)
{
  struct ovl_node *n = (struct ovl_node *) p;
  return n->name_hash % s;
}

static bool
node_compare (const void *n1, const void *n2)
{
  struct ovl_node *node1 = (struct ovl_node *) n1;
  struct ovl_node *node2 = (struct ovl_node *) n2;

  if (node1->name_hash != node2->name_hash)
    return false;

  return strcmp (node1->name, node2->name) == 0 ? true : false;
}

static struct ovl_node *
register_inode (struct ovl_data *lo, struct ovl_node *n, mode_t mode)
{
  struct ovl_ino key;
  struct ovl_ino *ino = NULL;

  key.ino = n->tmp_ino;
  key.dev = n->tmp_dev;

  /* Already registered.  */
  if (n->ino)
    return n;

  ino = hash_lookup (lo->inodes, &key);
  if (ino)
    {
      struct ovl_node *it;

      for (it = ino->node; it; it = it->next_link)
        {
          if (n->parent == it->parent && node_compare (n, it))
            {
              node_free (n);
              return it;
            }
        }

      n->next_link = ino->node;
      ino->node = n;
      ino->mode = mode;
      n->ino = ino;
      return n;
    }

  ino = calloc (1, sizeof (*ino));
  if (ino == NULL)
    return NULL;

  ino->ino = n->tmp_ino;
  ino->dev = n->tmp_dev;
  ino->node = n;
  n->ino = ino;
  ino->mode = mode;

  if (hash_insert (lo->inodes, ino) == NULL)
    {
      free (ino);
      node_free (n);
      return NULL;
    }

  stats.inodes++;
  return ino->node;
}

static bool
do_forget (struct ovl_data *lo, fuse_ino_t ino, uint64_t nlookup)
{
  struct ovl_ino *i;

  if (ino == FUSE_ROOT_ID || ino == 0)
    return false;

  i = lookup_inode (lo, ino);
  if (i == NULL || i == &dummy_ino)
    return false;

  i->lookups -= nlookup;
  if (i->lookups <= 0)
    {
      hash_delete (lo->inodes, i);
      inode_free (i);
    }
  return true;
}

/* cleanup any inode that has 0 lookups.  */
static void
cleanup_inodes (struct ovl_data *lo)
{
  cleanup_free struct ovl_ino **to_cleanup = NULL;
  size_t no_lookups = 0;
  struct ovl_ino *it;
  size_t i;

  /* Also attempt to cleanup any inode that has 0 lookups.  */
  for (it = hash_get_first (lo->inodes); it; it = hash_get_next (lo->inodes, it))
    {
      if (it->lookups == 0)
        no_lookups++;
    }
  if (no_lookups > 0)
    {
      to_cleanup = malloc (sizeof (*to_cleanup) * no_lookups);
      if (! to_cleanup)
        return;

      for (i = 0, it = hash_get_first (lo->inodes); it; it = hash_get_next (lo->inodes, it))
        {
          if (it->lookups == 0)
            to_cleanup[i++] = it;
        }
      for (i = 0; i < no_lookups; i++)
        do_forget (lo, (fuse_ino_t) to_cleanup[i], 0);
    }
}

static void
ovl_forget (fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_forget(ino=%" PRIu64 ", nlookup=%lu)\n",
	     ino, nlookup);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }
           
  do_forget (lo, ino, nlookup);
  fuse_reply_none (req);
}

static void
ovl_forget_multi (fuse_req_t req, size_t count, struct fuse_forget_data *forgets)
{
  size_t i;
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_forget_multi(count=%zu, forgets=%p)\n",
	     count, forgets);

  for (i = 0; i < count; i++)
    do_forget (lo, forgets[i].ino, forgets[i].nlookup);

  cleanup_inodes (lo);

  fuse_reply_none (req);
}

static inline void
cleanup_node_initp (struct ovl_node **p)
{
  struct ovl_node *n = *p;
  if (n == NULL)
    return;
  if (n->children)
    hash_free (n->children);
  free (n->name);
  free (n->path);
  free (n);
}

#define cleanup_node_init __attribute__((cleanup (cleanup_node_initp)))

static void
node_set_name (struct ovl_node *node, char *name)
{
  node->name = name;
  if (name == NULL)
    node->name_hash = 0;
  else
    node->name_hash = hash_string (name, SIZE_MAX);
}

static struct ovl_node *
make_whiteout_node (const char *path, const char *name)
{
  cleanup_node_init struct ovl_node *ret = NULL;
  struct ovl_node *ret_xchg;
  char *new_name;

  ret = calloc (1, sizeof (*ret));
  if (ret == NULL)
    return NULL;

  new_name = strdup (name);
  if (new_name == NULL)
    return NULL;

  node_set_name (ret, new_name);

  ret->path = strdup (path);
  if (ret->path == NULL)
    return NULL;

  ret->whiteout = 1;
  ret->ino = &dummy_ino;

  ret_xchg = ret;
  ret = NULL;

  stats.nodes++;
  return ret_xchg;
}

static ssize_t
safe_read_xattr (char **ret, int sfd, const char *name, size_t initial_size)
{
  cleanup_free char *buffer = NULL;
  size_t current_size;
  ssize_t s;

  current_size = initial_size;
  buffer = malloc (current_size + 1);
  if (buffer == NULL)
    return -1;

  while (1)
    {
      char *tmp;

      s = fgetxattr (sfd, name, buffer, current_size);
      if (s >= 0 && s < current_size)
        break;

      if (s < 0 && errno != ERANGE)
        break;

      current_size *= 2;
      tmp = realloc (buffer, current_size + 1);
      if (tmp == NULL)
        return -1;

      buffer = tmp;
    }

  if (s <= 0)
    return s;

  buffer[s] = '\0';

  /* Change owner.  */
  *ret = buffer;
  buffer = NULL;

  return s;
}

void initCipherCtx(struct ovl_node *node)
{
    node->block_enc = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(node->block_enc);
    node->block_dec = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(node->block_dec);
    node->stream_enc = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(node->stream_enc);
    node->stream_dec = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(node->stream_dec);

    pthread_mutex_init(&node->mutex, NULL);

    pthread_mutex_lock(&node->mutex);

    // initialize the cipher context once so that we don't have to do it for every block..
    EVP_EncryptInit_ex(node->block_enc, gSSLCipher.blockCipher, NULL, NULL, NULL);
    EVP_DecryptInit_ex(node->block_dec, gSSLCipher.blockCipher, NULL, NULL, NULL);
    EVP_EncryptInit_ex(node->stream_enc, gSSLCipher.streamCipher, NULL, NULL, NULL);
    EVP_DecryptInit_ex(node->stream_dec, gSSLCipher.streamCipher, NULL, NULL, NULL);

    EVP_CIPHER_CTX_set_key_length(node->block_enc, gSSLKey.keySize);
    EVP_CIPHER_CTX_set_key_length(node->block_dec, gSSLKey.keySize);
    EVP_CIPHER_CTX_set_key_length(node->stream_enc, gSSLKey.keySize);
    EVP_CIPHER_CTX_set_key_length(node->stream_dec, gSSLKey.keySize);

    EVP_CIPHER_CTX_set_padding(node->block_enc, 0);
    EVP_CIPHER_CTX_set_padding(node->block_dec, 0);
    EVP_CIPHER_CTX_set_padding(node->stream_enc, 0);
    EVP_CIPHER_CTX_set_padding(node->stream_dec, 0);

    EVP_EncryptInit_ex(node->block_enc, NULL, NULL, gSSLKey.buffer, NULL);
    EVP_DecryptInit_ex(node->block_dec, NULL, NULL, gSSLKey.buffer, NULL);
    EVP_EncryptInit_ex(node->stream_enc, NULL, NULL, gSSLKey.buffer, NULL);
    EVP_DecryptInit_ex(node->stream_dec, NULL, NULL, gSSLKey.buffer, NULL);

    node->cache.data = (unsigned char *)malloc(gBlockSize);
    memset(node->cache.data, 0, gBlockSize);

    pthread_mutex_unlock(&node->mutex);
}


static struct ovl_node *
make_ovl_node (struct ovl_data *lo, const char *path, struct ovl_layer *layer, const char *name, ino_t ino, dev_t dev, bool dir_p, struct ovl_node *parent, bool fast_ino_check)
{
  mode_t mode = 0;
  char *new_name;
  struct ovl_node *ret_xchg;
  bool has_origin = true;
  cleanup_node_init struct ovl_node *ret = NULL;

  ret = calloc (1, sizeof (*ret));
  if (ret == NULL)
    return NULL;

  ret->parent = parent;
  ret->layer = layer;
  ret->tmp_ino = ino;
  ret->tmp_dev = dev;
  ret->hidden_dirfd = -1;
  ret->inodes = lo->inodes;
  ret->next_link = NULL;
  ret->ino = NULL;
  ret->node_lookups = 0;
  initCipherCtx(ret);

  new_name = strdup (name);
  if (new_name == NULL)
    return NULL;
  node_set_name (ret, new_name);

  if (has_prefix (path, "./") && path[2])
    path += 2;

  ret->path = strdup (path);
  if (ret->path == NULL)
    return NULL;

  if (!dir_p)
    ret->children = NULL;
  else
    {
      ret->children = hash_initialize (128, NULL, node_hasher, node_compare, node_free);
      if (ret->children == NULL)
        return NULL;
    }

  if (ret->tmp_ino == 0)
    {
      struct stat st;
      struct ovl_layer *it;
      cleanup_free char *npath = NULL;
      char whiteout_path[PATH_MAX];

      npath = strdup (ret->path);
      if (npath == NULL)
        return NULL;

      if (parent)
        strconcat3 (whiteout_path, PATH_MAX, parent->path, "/.wh.", name);
      else
        strconcat3 (whiteout_path, PATH_MAX, "/.wh.", name, NULL);

      for (it = layer; it; it = it->next)
        {
          ssize_t s;
          cleanup_free char *val = NULL;
          cleanup_free char *origin = NULL;
          cleanup_close int fd = -1;

          if (dir_p)
            {
              int r;

              r = it->ds->file_exists (it, whiteout_path);
              if (r < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
               return NULL;

              if (r == 0)
                break;
            }

          if (! fast_ino_check)
            fd = it->ds->openat (it, npath, O_RDONLY|O_NONBLOCK|O_NOFOLLOW, 0755);

          if (fd < 0)
            {
              if (it->ds->statat (it, npath, &st, AT_SYMLINK_NOFOLLOW, STATX_TYPE|STATX_MODE|STATX_INO) == 0)
                {
                  if (has_origin)
                    {
                      ret->tmp_ino = st.st_ino;
                      ret->tmp_dev = st.st_dev;
                      if (mode == 0)
                        mode = st.st_mode;
                    }
                  ret->last_layer = it;
                }
              has_origin = false;
              goto no_fd;
            }

          /* It is an open FD, stat the file and read the origin xattrs.  */
          if (it->ds->fstat (it, fd, npath, STATX_TYPE|STATX_MODE|STATX_INO, &st) == 0)
            {
              if (has_origin)
                {
                  ret->tmp_ino = st.st_ino;
                  ret->tmp_dev = st.st_dev;
                  if (mode == 0)
                    mode = st.st_mode;
                }
              ret->last_layer = it;
            }

          s = safe_read_xattr (&val, fd, PRIVILEGED_ORIGIN_XATTR, PATH_MAX);
          if (s > 0)
            {
              char buf[512];
              struct ovl_fh *ofh = (struct ovl_fh *) val;
              size_t s = ofh->len - sizeof (*ofh);
              struct file_handle *fh = (struct file_handle *) buf;

              if (s < sizeof (buf) - sizeof(int) * 2)
                {
                  cleanup_close int originfd = -1;

                  /*
                    overlay in the kernel stores a file handle in the .origin xattr.
                    Honor it when present, but don't fail on errors as an unprivileged
                    user cannot open a file handle.
                  */
                  fh->handle_bytes = s;
                  fh->handle_type = ofh->type;
                  memcpy (fh->f_handle, ofh->fid, s);

                  originfd = open_by_handle_at (AT_FDCWD, fh, O_RDONLY);
                  if (originfd >= 0)
                    {
                      if (it->ds->fstat (it, originfd, npath, STATX_TYPE|STATX_MODE|STATX_INO, &st) == 0)
                        {
                          ret->tmp_ino = st.st_ino;
                          ret->tmp_dev = st.st_dev;
                          mode = st.st_mode;
                          break;
                        }
                    }
                }
            }

          /* If an origin is specified, use it for the next layer lookup.  */
          s = safe_read_xattr (&origin, fd, ORIGIN_XATTR, PATH_MAX);
          if (s <= 0)
            has_origin = false;
          else
            {
              free (npath);
              npath = origin;
              origin = NULL;
            }

no_fd:
          if (parent && parent->last_layer == it)
            break;
        }
    }

  ret_xchg = ret;
  ret = NULL;

  stats.nodes++;
  return register_inode (lo, ret_xchg, mode);
}

static struct ovl_node *
insert_node (struct ovl_node *parent, struct ovl_node *item, bool replace)
{
  struct ovl_node *old = NULL, *prev_parent = item->parent;
  int ret;

  if (prev_parent)
    {
      if (hash_lookup (prev_parent->children, item) == item)
        hash_delete (prev_parent->children, item);
    }

  if (replace)
    {
      old = hash_delete (parent->children, item);
      if (old)
        node_free (old);
    }

  ret = hash_insert_if_absent (parent->children, item, (const void **) &old);
  if (ret < 0)
    {
      node_free (item);
      errno = ENOMEM;
      return NULL;
    }
  if (ret == 0)
    {
      node_free (item);
      return old;
    }

  item->parent = parent;

  return item;
}

static const char *
get_whiteout_name (const char *name, struct stat *st)
{
  if (has_prefix (name, ".wh."))
    return name + 4;
  if (st
      && (st->st_mode & S_IFMT) == S_IFCHR
      && major (st->st_rdev) == 0
      && minor (st->st_rdev) == 0)
    return name;
  return NULL;
}

/*
static int hide_lowlayer_path(char *path, char *name, char *home)
{
    int i = 0;
    int len = 0;
    char hide_path[1024] = {0};
    char special_paths[][32] = {
        "Desktop",
        "Documents",
        "Downloads",
        "Music",
        "Pictures",
        "Public",
        "Templates",
        "Videos"
    };

    if (0 == strncmp(path, home, strlen(home))) {
        if (0 == strcmp(name, ".Xauthority")) {
            return 1;
        }
        if (0 == strncmp(name, ".", strlen("."))) {
            return 0;
        }
        for (i = 0; i<sizeof(special_paths)/32; i++) {
            if (0 == strncmp(name, special_paths[i], strlen(special_paths[i]))) {
                return 0;
            }
        }
        syslog(LOG_INFO, "hide_lowlayer_path path=%s name=%s\n", path, name);
        return 1;
    }

    for (i = 0; i<sizeof(special_paths)/32; i++) {
        snprintf(hide_path, sizeof(hide_path), "%s/%s", home, special_paths[i]);
        if (0 == strncmp(path, hide_path, strlen(hide_path))) {
            syslog(LOG_INFO, "hide_lowlayer_path path=%s name=%s\n", path, name);
            return 1;
        }
        memset(hide_path, 0, sizeof(hide_path));
    }

    return 0;
}
*/
char *line_remove_spaces(const char *buf) {
    size_t len = strlen(buf);
    if (len == 0)
        return NULL;

    // allocate memory for the new string
    char *rv = malloc(len + 1);
    if (rv == NULL)
        return NULL;

    // remove space at start of line
    const char *ptr1 = buf;
    while (*ptr1 == ' ' || *ptr1 == '\t')
        ptr1++;

    // copy data and remove additional spaces
    char *ptr2 = rv;
    int state = 0;
    while (*ptr1 != '\0') {
        if (*ptr1 == '\n' || *ptr1 == '\r')
            break;

        if (state == 0) {
            if (*ptr1 != ' ' && *ptr1 != '\t')
                *ptr2++ = *ptr1++;
            else {
                *ptr2++ = ' ';
                ptr1++;
                state = 1;
            }
        }
        else {  // state == 1
            while (*ptr1 == ' ' || *ptr1 == '\t')
                ptr1++;
            state = 0;
        }
    }

    if (ptr2 > rv && *(ptr2 - 1) == ' ')
        --ptr2;
    *ptr2 = '\0';

    return rv;
}

char *gnu_basename(char *path) {
    char *last_slash = strrchr(path, '/');
    if (!last_slash)
        return path;
    return last_slash+1;
}

void profile_add_list(char *str, ProfileEntry **list) {
    ProfileEntry *prf = malloc(sizeof(ProfileEntry));
    if (!prf) {
        printf("prf is NULL\n");
        return;
    }
    memset(prf, 0, sizeof(ProfileEntry));
    prf->next = NULL;
    prf->data = str;

    if (*list == NULL) {
        *list = prf;
        return;
    }
    ProfileEntry *ptr = *list;
    while (ptr->next != NULL)
        ptr = ptr->next;
    ptr->next = prf;
}

void profile_add_globlist(char *str, ProfileEntry **list) {
    size_t i;
    glob_t globbuf;
    char *path = NULL;

    int globerr = glob(str, GLOB_NOCHECK | GLOB_NOSORT | GLOB_PERIOD, NULL, &globbuf);
    if (globerr) {
        printf("Error: failed to glob pattern %s\n", str);
        return;
    }

    for (i = 0; i < globbuf.gl_pathc; i++) {
        path = globbuf.gl_pathv[i];
        //printf("path: %s\n", path);

        const char *base = gnu_basename(path);
        if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
            continue;

        profile_add_list(strdup(path), list);
    }

    globfree(&globbuf);
}

void profile_mergelist(ProfileEntry **includelist, ProfileEntry **excludelist, ProfileEntry **mergelist) {
    ProfileEntry *includeentry;
    ProfileEntry *excludeentry;
    int mergeflag = 1;

    includeentry = *includelist;
    while (includeentry) {
        mergeflag = 1;
        excludeentry = *excludelist;
        while (excludeentry) {
            if (strcmp(includeentry->data, excludeentry->data) == 0) {
                mergeflag = 0;
                break;
            }

            excludeentry = excludeentry->next;
        }

        if(mergeflag) {
            profile_add_list(includeentry->data, mergelist);
        }
        includeentry = includeentry->next;
    }

}

char *expand_macros(char *path) {
    char *new_name = NULL;

    char *uid = getenv("PKEXEC_UID");
    struct passwd *pw = getpwuid(atoi(uid));

    if (strncmp(path, "$HOME", 5) == 0) {
        printf("Error: $HOME is not allowed in profile files, please replace it with ${HOME}\n");
        return NULL;
    }
    else if (strncmp(path, "${HOME}", 7) == 0) {
        asprintf(&new_name, "%s%s", pw->pw_dir, path + 7);
        return new_name;
    }
    else if (*path == '~') {
        asprintf(&new_name, "%s%s", pw->pw_dir, path + 1);
        return new_name;
    }

    return strdup(path);
}

void parse_mergelist() {
    char buf[MAX_READ + 1];
    int lineno = 0;
    ProfileEntry *entry;
    char *ptr = NULL;
    char *new_name = NULL;

    FILE *fp = fopen("/home/jailbox/profile.config", "r");
    if (fp == NULL) {
        syslog(LOG_INFO,"Error: cannot open profile file %s\n", "profile.config");
        return;
    }

    while (fgets(buf, MAX_READ, fp)) {
        ++lineno;
        
        ptr = line_remove_spaces(buf);
        if (ptr == NULL)
            continue;
        
        if (*ptr == '#' || *ptr == '\0') {
            free(ptr);
            continue;
        }

        if (strncmp(ptr, "whitelist ", 10) == 0) {
            new_name = expand_macros(ptr+10);
            if (new_name) {
                profile_add_globlist(new_name, &whitelist);
            }
        } else if (strncmp(ptr, "nowhitelist ", 12) == 0) {
            new_name = expand_macros(ptr+12);
            if (new_name) {
                profile_add_globlist(new_name, &nowhitelist);
            }
        } else if (strncmp(ptr, "blacklist ", 10) == 0) {
            new_name = expand_macros(ptr+10);
            if (new_name) {
                profile_add_globlist(new_name, &blacklist);
            }
        }
        if (new_name)
            free(new_name);
    }

    profile_mergelist(&whitelist, &nowhitelist, &mergewhitelist);
    profile_mergelist(&blacklist, &mergewhitelist, &mergelist);

    syslog(LOG_INFO, "mergelist----------------------\n");
    entry = mergelist;
    while (entry) {
        syslog(LOG_INFO, "mergelist %s\n", entry->data);
        entry = entry->next;
    }
}

static int hide_lowlayer_path(char *path, char *name)
{
    int len = 0;
    ProfileEntry *entry = NULL;
    char *base = NULL;
    char *dir = NULL;
    char *item = NULL;

    entry = mergelist;
    while (entry) {
        len = strlen(entry->data);
        if (entry->data[len-1] == '/') {
            if (0 == strncmp(path, entry->data+1, len-2)) {
                syslog(LOG_INFO, "hide_lowlayer_path path=%s name=%s\n", path, name);
                return 1;
            }
        } else {
            item = strdup(entry->data);
            dir = dirname(item);
            base = gnu_basename(entry->data);
            if (0 == strcmp(path, dir+1) && 0 == strcmp(name, base)) {
                free(item);
                syslog(LOG_INFO, "hide_lowlayer_path path=%s name=%s\n", path, name);
                return 1;
            }
            free(item);
        }

        entry = entry->next;
    }

    return 0;
}

static struct ovl_node *
load_dir (struct ovl_data *lo, struct ovl_node *n, struct ovl_layer *layer, char *path, char *name)
{
  struct dirent *dent;
  bool stop_lookup = false;
  struct ovl_layer *it, *lower_layer = get_lower_layers(lo), *upper_layer = get_upper_layer (lo);
  char parent_whiteout_path[PATH_MAX];

  if (!n)
    {
      n = make_ovl_node (lo, path, layer, name, 0, 0, true, NULL, lo->fast_ino_check);
      if (n == NULL)
        {
          errno = ENOMEM;
          return NULL;
        }
    }

  if (n->parent)
    strconcat3 (parent_whiteout_path, PATH_MAX, n->parent->path, "/.wh.", name);
  else
    strconcat3 (parent_whiteout_path, PATH_MAX, ".wh.", name, NULL);

  for (it = lo->layers; it && !stop_lookup; it = it->next)
    {
      int ret;
      DIR *dp = NULL;

      if (n->last_layer == it)
        stop_lookup = true;

      ret = it->ds->file_exists (it, parent_whiteout_path);
      if (ret < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
        return NULL;

      if (ret == 0)
        break;

      if (0 == checkPath(lo, path))
      {
          continue;
      }
      
      dp = it->ds->opendir (it, path);
      if (dp == NULL)
        continue;

      for (;;)
        {
          struct ovl_node key;
          struct ovl_node *child = NULL;
          char node_path[PATH_MAX];
          char whiteout_path[PATH_MAX];

          errno = 0;
          dent = it->ds->readdir (dp);
          if (dent == NULL)
            {
              if (errno)
                {
                  it->ds->closedir (dp);
                  return NULL;
                }

              break;
            }

          if ((strcmp (dent->d_name, ".") == 0) || strcmp (dent->d_name, "..") == 0)
            continue;

          if (it == lower_layer && hide_lowlayer_path(path, dent->d_name))
          {
              continue;
          }

          node_set_name (&key, dent->d_name);
          child = hash_lookup (n->children, &key);
          if (child)
            {
              child->last_layer = it;
              if (!child->whiteout || it != upper_layer)
                continue;
              else
                {
                  hash_delete (n->children, child);
                  node_free (child);
                  child = NULL;
                }
            }

          strconcat3 (whiteout_path, PATH_MAX, path, "/.wh.", dent->d_name);

          strconcat3 (node_path, PATH_MAX, n->path, "/", dent->d_name);

          ret = it->ds->file_exists (it, whiteout_path);
          if (ret < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
            {
              it->ds->closedir (dp);
              return NULL;
            }

          if (ret == 0)
            {
              child = make_whiteout_node (node_path, dent->d_name);
              if (child == NULL)
                {
                  errno = ENOMEM;
                  it->ds->closedir (dp);
                  return NULL;
                }
            }
          else
            {
              const char *wh = NULL;
              bool dirp = dent->d_type == DT_DIR;

              if ((dent->d_type != DT_CHR) && (dent->d_type != DT_UNKNOWN))
                wh = get_whiteout_name (dent->d_name, NULL);
              else
                {
                  /* A stat is required either if the type is not known, or if it is a character device as it could be
                     a whiteout file.  */
                  struct stat st;

                  ret = it->ds->statat (it, node_path, &st, AT_SYMLINK_NOFOLLOW, STATX_TYPE);
                  if (ret < 0)
                    {
                      it->ds->closedir (dp);
                      return NULL;
                    }

                  dirp = st.st_mode & S_IFDIR;
                  wh = get_whiteout_name (dent->d_name, &st);
                }

              if (wh)
                {
                  child = make_whiteout_node (node_path, wh);
                  if (child == NULL)
                    {
                      errno = ENOMEM;
                      it->ds->closedir (dp);
                      return NULL;
                    }
                }
              else
                {
                  ino_t ino = 0;

                  if (lo->fast_ino_check)
                    ino = dent->d_ino;

                  child = make_ovl_node (lo, node_path, it, dent->d_name, ino, 0, dirp, n, lo->fast_ino_check);
                  if (child == NULL)
                    {
                      errno = ENOMEM;
                      it->ds->closedir (dp);
                      return NULL;
                    }
                  child->last_layer = it;
                }
            }

          if (insert_node (n, child, false) == NULL)
            {
              errno = ENOMEM;
              it->ds->closedir (dp);
              return NULL;
            }
        }

      ret = is_directory_opaque (it, path);
      if (ret < 0)
        {
          it->ds->closedir (dp);
          return NULL;
        }
      if (ret > 0)
        {
          n->last_layer = it;
          stop_lookup = true;
        }
      it->ds->closedir (dp);
    }

  if (get_timeout (lo) > 0)
    n->loaded = 1;
  return n;
}

static struct ovl_node *
reload_dir (struct ovl_data *lo, struct ovl_node *node)
{
  if (! node->loaded)
    node = load_dir (lo, node, node->layer, node->path, node->name);
  return node;
}

static void
free_layers (struct ovl_layer *layers)
{
  if (layers == NULL)
    return;
  free_layers (layers->next);
  free (layers->path);
  if (layers->fd >= 0)
    close (layers->fd);
  free (layers);
}

static void
cleanup_layerp (struct ovl_layer **p)
{
  struct ovl_layer *l = *p;
  free_layers (l);
}

#define cleanup_layer __attribute__((cleanup (cleanup_layerp)))

static struct ovl_layer *
read_dirs (struct ovl_data *lo, char *path, bool low, struct ovl_layer *layers)
{
  char *saveptr = NULL, *it;
  struct ovl_layer *last;
  cleanup_free char *buf = NULL;

  if (path == NULL)
    return NULL;

  buf = strdup (path);
  if (buf == NULL)
    return NULL;

  last = layers;
  while (last && last->next)
    last = last->next;

  for (it = strtok_r (buf, ":", &saveptr); it; it = strtok_r (NULL, ":", &saveptr))
    {
      char *name, *data;
      char *it_path = it;
      int i, n_layers;
      cleanup_layer struct ovl_layer *l = NULL;
      struct data_source *ds;

      if (it[0] != '/' || it[1] != '/')
        {
          /* By default use the direct access data store.  */
          ds = &direct_access_ds;

          data = NULL;
          path = it_path;
        }
      else
        {
          struct ovl_plugin *p;
          char *plugin_data_sep, *plugin_sep;

          if (! low)
            {
              fprintf (stderr, "plugins are supported only with lower layers\n");
              return NULL;
            }

          plugin_sep = strchr (it + 2, '/');
          if (! plugin_sep)
            {
              fprintf (stderr, "invalid separator for plugin\n");
              return NULL;
            }

          *plugin_sep = '\0';

          name = it + 2;
          data = plugin_sep + 1;

          plugin_data_sep = strchr (data, '/');
          if (! plugin_data_sep)
            {
              fprintf (stderr, "invalid separator for plugin\n");
              return NULL;
            }

          *plugin_data_sep = '\0';
          path = plugin_data_sep + 1;

          p = plugin_find (lo->plugins_ctx, name);
          if (! p)
            {
              fprintf (stderr, "cannot find plugin %s\n", name);
              return NULL;
            }

          ds = p->load (data, path);
          if (ds == NULL)
            {
              fprintf (stderr, "cannot load plugin %s\n", name);
              return NULL;
            }
        }

      n_layers = ds->num_of_layers (data, path);
      if (n_layers < 0)
        {
          fprintf (stderr, "cannot retrieve number of layers for %s\n", path);
          return NULL;
        }

      for (i = 0; i < n_layers; i++)
        {
          l = calloc (1, sizeof (*l));
          if (l == NULL)
            return NULL;

          l->ds = ds;

          l->ovl_data = lo;

          l->path = NULL;
          l->fd = -1;

          if (l->ds->load_data_source (l, data, path, i) < 0)
            {
              fprintf (stderr, "cannot load store %s at %s\n", data, path);
              return NULL;
            }

          l->low = low;
          if (low)
            {
              l->next = NULL;
              if (last == NULL)
                last = layers = l;
              else
                {
                  last->next = l;
                  last = l;
                }
            }
          else
            {
              l->next = layers;
              layers = l;
            }
          l = NULL;
        }
    }
  return layers;
}

static struct ovl_node *
do_lookup_file (fuse_req_t req, struct ovl_data *lo, fuse_ino_t parent, const char *name)
{
  struct ovl_node key;
  struct ovl_node *node, *pnode;

  if (parent == FUSE_ROOT_ID)
    pnode = lo->root;
  else
    pnode = inode_to_node (lo, parent);
  
  if (0 == checkPath(lo, pnode->path)) {
    return NULL;
  }

  if (name == NULL)
    return pnode;

  if (has_prefix (name, ".wh."))
    {
      errno = EINVAL;
      return NULL;
    }

  node_set_name (&key, (char *) name);
  node = hash_lookup (pnode->children, &key);
  if (node == NULL && !pnode->loaded)
    {
      int ret;
      struct ovl_layer *it;
      struct stat st;
      bool stop_lookup = false;

      for (it = lo->layers; it && !stop_lookup; it = it->next)
        {
          char path[PATH_MAX];
          char whpath[PATH_MAX];
          const char *wh_name;

          if (pnode->last_layer == it)
            stop_lookup = true;

          strconcat3 (path, PATH_MAX, pnode->path, "/", name);

          ret = it->ds->statat (it, path, &st, AT_SYMLINK_NOFOLLOW, STATX_TYPE|STATX_MODE|STATX_INO);
          if (ret < 0)
            {
              int saved_errno = errno;

              if (errno == ENOENT || errno == ENOTDIR)
                {
                  if (node)
                    continue;

                  strconcat3 (whpath, PATH_MAX, pnode->path, "/.wh.", name);

                  ret = it->ds->file_exists (it, whpath);
                  if (ret < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
                    return NULL;
                  if (ret == 0)
                    {
                      node = make_whiteout_node (path, name);
                      if (node == NULL)
                        {
                          errno = ENOMEM;
                          return NULL;
                        }
                      goto insert_node;
                    }
                  continue;
                }
              errno = saved_errno;
              return NULL;
            }

          /* If we already know the node, simply update the ino.  */
          if (node)
            {
              node->tmp_ino = st.st_ino;
              node->tmp_dev = st.st_dev;
              node->last_layer = it;
              continue;
            }

          strconcat3 (whpath, PATH_MAX, pnode->path, "/.wh.", name);
          ret = it->ds->file_exists (it, whpath);
          if (ret < 0 && errno != ENOENT && errno != ENOTDIR && errno != ENAMETOOLONG)
            return NULL;
          if (ret == 0)
              node = make_whiteout_node (path, name);
          else
            {
              wh_name = get_whiteout_name (name, &st);
              if (wh_name)
                node = make_whiteout_node (path, wh_name);
              else
                node = make_ovl_node (lo, path, it, name, 0, 0, st.st_mode & S_IFDIR, pnode, lo->fast_ino_check);
            }
          if (node == NULL)
            {
              errno = ENOMEM;
              return NULL;
            }

          if (st.st_mode & S_IFDIR)
            {
              ret = is_directory_opaque (it, path);
              if (ret < 0)
                {
                  node_free (node);
                  return NULL;
                }
              if (ret > 0)
                {
                  node->last_layer = it;
                  stop_lookup = true;
                }
            }
insert_node:
          if (insert_node (pnode, node, false) == NULL)
            {
              node_free (node);
              errno = ENOMEM;
              return NULL;
            }
        }
    }

  return node;
}

static void
ovl_lookup (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  cleanup_lock int l = enter_big_lock ();
  struct fuse_entry_param e;
  int err = 0;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_lookup(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);

  if (!checkAuthority(req, parent)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  memset (&e, 0, sizeof (e));

  node = do_lookup_file (req, lo, parent, name);
  if (node == NULL || node->whiteout)
    {
      e.ino = 0;
      e.attr_timeout = get_timeout (lo);
      e.entry_timeout = get_timeout (lo);
      fuse_reply_entry (req, &e);
      return;
    }

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_lookup(child=%" PRIu64 ")\n", node->ino->ino);

  if (!lo->static_nlink && node_dirp (node))
    {
      node = reload_dir (lo, node);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  err = rpl_stat (req, node, -1, NULL, NULL, &e.attr);
  if (err)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = node_to_inode (node);
  node->ino->lookups++;
  e.attr_timeout = get_timeout (lo);
  e.entry_timeout = get_timeout (lo);
  fuse_reply_entry (req, &e);
}

struct ovl_dirp
{
  struct ovl_data *lo;
  struct ovl_node *parent;
  struct ovl_node **tbl;
  size_t tbl_size;
  size_t offset;
};

static struct ovl_dirp *
ovl_dirp (struct fuse_file_info *fi)
{
  return (struct ovl_dirp *) (uintptr_t) fi->fh;
}

static int
reload_tbl (struct ovl_data *lo, struct ovl_dirp *d, struct ovl_node *node)
{
  size_t counter = 0;
  struct ovl_node *it;

  node = reload_dir (lo, node);
  if (node == NULL)
    return -1;

  if (d->tbl)
    free (d->tbl);

  d->offset = 0;
  d->parent = node;
  d->tbl_size = hash_get_n_entries (node->children) + 2;
  d->tbl = calloc (sizeof (struct ovl_node *), d->tbl_size);
  if (d->tbl == NULL)
    {
      errno = ENOMEM;
      return -1;
    }

  d->tbl[counter++] = node;
  d->tbl[counter++] = node->parent;

  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    {
      it->ino->lookups++;
      it->node_lookups++;
      d->tbl[counter++] = it;
    }

  return 0;
}

static void
ovl_opendir (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_dirp *d = calloc (1, sizeof (struct ovl_dirp));
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_opendir(ino=%" PRIu64 ")\n", ino);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if (d == NULL)
    {
      errno = ENOMEM;
      goto out_errno;
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL)
    {
      errno = ENOENT;
      goto out_errno;
    }

  if (! node_dirp (node))
    {
      errno = ENOTDIR;
      goto out_errno;
    }

  d->parent = node;

  fi->fh = (uintptr_t) d;
  if (get_timeout (lo) > 0)
    {
      fi->keep_cache = 1;
#if HAVE_FUSE_CACHE_READDIR
      fi->cache_readdir = 1;
#endif
    }
  node->in_readdir++;
  fuse_reply_open (req, fi);
  return;

out_errno:
  if (d)
    {
      if (d->tbl)
        free (d->tbl);
      free (d);
    }
  fuse_reply_err (req, errno);
}

static int
create_missing_whiteouts (struct ovl_data *lo, struct ovl_node *node, const char *from)
{
  struct ovl_layer *l;

  if (! node_dirp (node))
    return 0;

  node = reload_dir (lo, node);
  if (node == NULL)
    return -1;

  for (l = get_lower_layers (lo); l; l = l->next)
    {
      cleanup_dir DIR *dp = NULL;

      dp = l->ds->opendir (l, from);
      if (dp == NULL)
        {
          if (errno == ENOTDIR)
            break;
          if (errno == ENOENT)
            continue;
          return -1;
        }
      else
        {
          struct dirent *dent;

          for (;;)
            {
              struct ovl_node key;
              struct ovl_node *n;

              errno = 0;
              dent = readdir (dp);
              if (dent == NULL)
                {
                  if (errno)
                    return -1;

                  break;
                }

              if (strcmp (dent->d_name, ".") == 0)
                continue;
              if (strcmp (dent->d_name, "..") == 0)
                continue;
              if (has_prefix (dent->d_name, ".wh."))
                continue;

              node_set_name (&key, (char *) dent->d_name);

              n = hash_lookup (node->children, &key);
              if (n)
                {
                  if (node_dirp (n))
                    {
                      char c[PATH_MAX];

                      n = reload_dir (lo, n);
                      if (n == NULL)
                        return -1;

                      strconcat3 (c, PATH_MAX, from, "/", n->name);

                      if (create_missing_whiteouts (lo, n, c) < 0)
                        return -1;
                    }
                  continue;
                }

              if (create_whiteout (lo, node, dent->d_name, false, true) < 0)
                return -1;
            }
        }
    }
  return 0;
}

static void
ovl_do_readdir (fuse_req_t req, fuse_ino_t ino, size_t size,
	       off_t offset, struct fuse_file_info *fi, int plus)
{
  struct ovl_data *lo = ovl_data (req);
  struct ovl_dirp *d = ovl_dirp (fi);
  size_t remaining = size;
  char *p;
  cleanup_free char *buffer = NULL;

  buffer = malloc (size);
  if (buffer == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (offset == 0 || d->tbl == NULL)
    {
      if (reload_tbl (lo, d, d->parent) < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  p = buffer;
  for (; remaining > 0 && offset < d->tbl_size; offset++)
      {
        int ret;
        size_t entsize;
        const char *name;
        struct ovl_node *node = d->tbl[offset];
        struct fuse_entry_param e;
        struct stat *st = &e.attr;

        if (node == NULL || node->whiteout || node->hidden)
          continue;

        if (offset == 0)
          name = ".";
        else if (offset == 1)
          name = "..";
        else
          {
            if (node->parent != d->parent)
              continue;
            name = node->name;
          }

        if (0 == checkPath(lo, node->path)) {
          continue;
        }
  
        if (!plus)
          {
            /* From the 'stbuf' argument the st_ino field and bits 12-15 of the
             * st_mode field are used.  The other fields are ignored.
             */
            st->st_ino = node->tmp_ino;
            st->st_dev = node->tmp_dev;
            st->st_mode = node->ino->mode;

            entsize = fuse_add_direntry (req, p, remaining, name, st, offset + 1);
          }
        else
          {
            if (!lo->static_nlink && node_dirp (node))
              {
                node = reload_dir (lo, node);
                if (node == NULL)
                  {
                    fuse_reply_err (req, errno);
                    return;
                  }
              }
            memset (&e, 0, sizeof (e));
            ret = rpl_stat (req, node, -1, NULL, NULL, st);
            if (ret < 0)
              {
                fuse_reply_err (req, errno);
                return;
              }

            e.attr_timeout = get_timeout (lo);
            e.entry_timeout = get_timeout (lo);
            e.ino = node_to_inode (node);
            entsize = fuse_add_direntry_plus (req, p, remaining, name, &e, offset + 1);
            if (entsize <= remaining)
              {
                /* First two entries are . and .. */
                if (offset >= 2)
                  node->ino->lookups++;
              }
          }

        if (entsize > remaining)
          break;

        p += entsize;
        remaining -= entsize;
      }
  fuse_reply_buf (req, buffer, size - remaining);
}

static void
ovl_readdir (fuse_req_t req, fuse_ino_t ino, size_t size,
	    off_t offset, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_readdir(ino=%" PRIu64 ", size=%zu, offset=%lo)\n", ino, size, offset);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }
    
  ovl_do_readdir (req, ino, size, offset, fi, 0);
}

static void
ovl_readdirplus (fuse_req_t req, fuse_ino_t ino, size_t size,
		off_t offset, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_readdirplus(ino=%" PRIu64 ", size=%zu, offset=%lo)\n", ino, size, offset);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  ovl_do_readdir (req, ino, size, offset, fi, 1);
}

static void
ovl_releasedir (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();
  size_t s;
  struct ovl_dirp *d = ovl_dirp (fi);
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node = NULL;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_releasedir(ino=%" PRIu64 ")\n", ino);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  for (s = 2; s < d->tbl_size; s++)
    {
      d->tbl[s]->node_lookups--;
      if (! do_forget (lo, (fuse_ino_t) d->tbl[s]->ino, 1))
        {
          if (d->tbl[s]->node_lookups == 0)
            node_free (d->tbl[s]);
        }
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node)
    node->in_readdir--;

  free (d->tbl);
  free (d);
  fuse_reply_err (req, 0);
}

/* in-place filter xattrs that cannot be accessed.  */
static ssize_t
filter_xattrs_list (char *buf, ssize_t len)
{
  ssize_t ret = 0;
  char *it;

  if (buf == NULL)
    return len;

  it = buf;

  while (it < buf + len)
    {
      size_t it_len;

      it_len = strlen (it) + 1;

      if (can_access_xattr (it))
        {
          it += it_len;
          ret += it_len;
        }
      else
        {
          char *next = it + it_len;

          memmove (it, next, buf + len - next);
          len -= it_len;
        }
    }

  return ret;
}

static void
ovl_listxattr (fuse_req_t req, fuse_ino_t ino, size_t size)
{
  cleanup_lock int l = enter_big_lock ();
  ssize_t len;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  cleanup_free char *buf = NULL;
  int ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_listxattr(ino=%" PRIu64 ", size=%zu)\n", ino, size);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if (lo->disable_xattrs)
    {
      fuse_reply_err (req, ENOSYS);
      return;
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (size > 0)
    {
      buf = malloc (size);
      if (buf == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  if (! node->hidden)
    ret = node->layer->ds->listxattr (node->layer, node->path, buf, size);
  else
    {
      char path[PATH_MAX];
      strconcat3 (path, PATH_MAX, lo->workdir, "/", node->path);
      ret = listxattr (path, buf, size);
    }
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  len = filter_xattrs_list (buf, ret);

  if (size == 0)
    fuse_reply_xattr (req, len);
  else if (len <= size)
    fuse_reply_buf (req, buf, len);
}

static void
ovl_getxattr (fuse_req_t req, fuse_ino_t ino, const char *name, size_t size)
{
  cleanup_lock int l = enter_big_lock ();
  ssize_t len;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  cleanup_free char *buf = NULL;
  int ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_getxattr(ino=%" PRIu64 ", name=%s, size=%zu)\n", ino, name, size);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if (lo->disable_xattrs)
    {
      fuse_reply_err (req, ENOSYS);
      return;
    }

  if (! can_access_xattr (name))
    {
      fuse_reply_err (req, ENODATA);
      return;
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (size > 0)
    {
      buf = malloc (size);
      if (buf == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  if (! node->hidden)
    ret = node->layer->ds->getxattr (node->layer, node->path, name, buf, size);
  else
    {
      char path[PATH_MAX];
      strconcat3 (path, PATH_MAX, lo->workdir, "/", node->path);
      ret = getxattr (path, name, buf, size);
    }

  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  len = ret;

  if (size == 0)
    fuse_reply_xattr (req, len);
  else
    fuse_reply_buf (req, buf, len);
}

static void
ovl_access (fuse_req_t req, fuse_ino_t ino, int mask)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *n = do_lookup_file (req, lo, ino, NULL);

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_access(ino=%" PRIu64 ", mask=%d)\n",
	     ino, mask);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if ((mask & n->ino->mode) == mask)
    fuse_reply_err (req, 0);
  else
    fuse_reply_err (req, EPERM);
}

static int
copy_xattr (int sfd, int dfd, char *buf, size_t buf_size)
{
  ssize_t xattr_len;

  xattr_len = flistxattr (sfd, buf, buf_size);
  if (xattr_len > 0)
    {
      char *it;
      for (it = buf; it - buf < xattr_len; it += strlen (it) + 1)
        {
          cleanup_free char *v = NULL;
          ssize_t s;

          if (! can_access_xattr (it))
            continue;

          s = safe_read_xattr (&v, sfd, it, 256);
          if (s < 0)
            return -1;

          if (fsetxattr (dfd, it, v, s, 0) < 0)
            {
              if (errno == EINVAL || errno == EOPNOTSUPP)
                continue;
              return -1;
            }
        }
    }
  return 0;
}

static int
empty_dirfd (int fd)
{
  cleanup_dir DIR *dp = NULL;
  struct dirent *dent;

  dp = fdopendir (fd);
  if (dp == NULL)
    {
      close (fd);
      return -1;
    }

  for (;;)
    {
      int ret;

      errno = 0;
      dent = readdir (dp);
      if (dent == NULL)
        {
          if (errno)
            return -1;

          break;
        }
      if (strcmp (dent->d_name, ".") == 0)
        continue;
      if (strcmp (dent->d_name, "..") == 0)
        continue;

      ret = unlinkat (dirfd (dp), dent->d_name, 0);
      if (ret < 0 && errno == EISDIR)
        {
          ret = unlinkat (dirfd (dp), dent->d_name, AT_REMOVEDIR);
          if (ret < 0 && errno == ENOTEMPTY)
            {
              int dfd;

              dfd = safe_openat (dirfd (dp), dent->d_name, O_DIRECTORY, 0);
              if (dfd < 0)
                return -1;

              ret = empty_dirfd (dfd);
              if (ret < 0)
                return -1;

              ret = unlinkat (dirfd (dp), dent->d_name, AT_REMOVEDIR);
              if (ret < 0)
                return -1;

              continue;
            }
        }
      if (ret < 0)
        return ret;
    }

  return 0;
}

static int create_node_directory (struct ovl_data *lo, struct ovl_node *src);

static int
create_directory (struct ovl_data *lo, int dirfd, const char *name, const struct timespec *times,
                  struct ovl_node *parent, int xattr_sfd, uid_t uid, gid_t gid, mode_t mode, bool set_opaque, struct stat *st_out)
{
  int ret;
  int saved_errno;
  cleanup_close int dfd = -1;
  cleanup_free char *buf = NULL;
  char wd_tmp_file_name[32];
  bool need_rename;

  if (lo->xattr_permissions)
    mode |= 0755;

  need_rename = set_opaque || times || xattr_sfd >= 0 || uid != lo->uid || gid != lo->gid;
  if (!need_rename)
    {
      /* mkdir can be used directly without a temporary directory in the working directory.  */
      ret = mkdirat (dirfd, name, mode);
      if (ret < 0)
        {
          if (errno == EEXIST)
            {
              unlinkat (dirfd, name, 0);
              ret = mkdirat (dirfd, name, mode);
            }
          if (ret < 0)
            return ret;
        }
      if (st_out)
        return fstatat (dirfd, name, st_out, AT_SYMLINK_NOFOLLOW);
      return 0;
    }

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  ret = mkdirat (lo->workdir_fd, wd_tmp_file_name, mode);
  if (ret < 0)
    goto out;

  ret = dfd = TEMP_FAILURE_RETRY (safe_openat (lo->workdir_fd, wd_tmp_file_name, O_RDONLY, 0));
  if (ret < 0)
    goto out;

  if (uid != lo->uid || gid != lo->gid || get_upper_layer (lo)->stat_override_mode != STAT_OVERRIDE_NONE)
    {
      ret = do_fchown (lo, dfd, uid, gid, mode);
      if (ret < 0)
        goto out;
    }

  if (times)
    {
      ret = futimens (dfd, times);
      if (ret < 0)
        goto out;
    }

  if (ret == 0 && xattr_sfd >= 0)
    {
      const size_t buf_size = 1 << 20;
      buf = malloc (buf_size);
      if (buf == NULL)
        {
          ret = -1;
          goto out;
        }

      ret = copy_xattr (xattr_sfd, dfd, buf, buf_size);
      if (ret < 0)
        goto out;
    }

  if (set_opaque)
    {
      ret = set_fd_opaque (dfd);
      if (ret < 0)
        goto out;
    }

  if (st_out)
    {
      ret = fstat (dfd, st_out);
      if (ret < 0)
        goto out;
    }

  ret = renameat (lo->workdir_fd, wd_tmp_file_name, dirfd, name);
  if (ret < 0)
    {
      if (errno == EEXIST)
        {
          int dfd = -1;

          ret = direct_renameat2 (lo->workdir_fd, wd_tmp_file_name, dirfd, name, RENAME_EXCHANGE);
          if (ret < 0)
            goto out;

          dfd = TEMP_FAILURE_RETRY (safe_openat (lo->workdir_fd, wd_tmp_file_name, O_DIRECTORY, 0));
          if (dfd < 0)
            return -1;

          ret = empty_dirfd (dfd);
          if (ret < 0)
            goto out;

          return unlinkat (lo->workdir_fd, wd_tmp_file_name, AT_REMOVEDIR);
        }
      if (errno == ENOTDIR)
        unlinkat (dirfd, name, 0);
      if (errno == ENOENT && parent)
        {
          ret = create_node_directory (lo, parent);
          if (ret != 0)
            goto out;
        }

      ret = renameat (lo->workdir_fd, wd_tmp_file_name, dirfd, name);
    }
out:
  saved_errno = errno;
  if (ret < 0)
      unlinkat (lo->workdir_fd, wd_tmp_file_name, AT_REMOVEDIR);
  errno = saved_errno;

  return ret;
}

static int
create_node_directory (struct ovl_data *lo, struct ovl_node *src)
{
  int ret;
  struct stat st;
  cleanup_close int sfd = -1;
  struct timespec times[2];

  if (src == NULL)
    return 0;

  if (src->layer == get_upper_layer (lo))
    return 0;

  ret = sfd = src->layer->ds->openat (src->layer, src->path, O_RDONLY|O_NONBLOCK, 0755);
  if (ret < 0)
    return ret;

  ret = TEMP_FAILURE_RETRY (fstat (sfd, &st));
  if (ret < 0)
    return ret;

  times[0] = st.st_atim;
  times[1] = st.st_mtim;

  ret = create_directory (lo, get_upper_layer (lo)->fd, src->path, times, src->parent, sfd, st.st_uid, st.st_gid, st.st_mode, false, NULL);
  if (ret == 0)
    {
      src->layer = get_upper_layer (lo);

      if (src->parent)
        delete_whiteout (lo, -1, src->parent, src->name);
    }

  return ret;
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
			     off_t off, size_t maxsize)
{
	if (off < bufsize)
		return fuse_reply_buf(req, buf + off,
				      min(bufsize - off, maxsize));
	else
		return fuse_reply_buf(req, NULL, 0);
}

void setIVec(unsigned char *ivec, uint64_t seed)
{
    int i = 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdLen = EVP_MAX_MD_SIZE;

    memcpy(ivec, gSSLKey.buffer+gSSLKey.keySize, gSSLKey.ivLength);

    for (i; i < 8; ++i) {
        md[i] = (unsigned char)(seed & 0xff);
        seed >>= 8;
    }

    // combine ivec and seed with HMAC
    HMAC_Init_ex(gSSLKey.mac_ctx, NULL, 0, NULL, NULL);
    HMAC_Update(gSSLKey.mac_ctx, ivec, gSSLKey.ivLength);
    HMAC_Update(gSSLKey.mac_ctx, md, 8);
    HMAC_Final(gSSLKey.mac_ctx, md, &mdLen);

    memcpy(ivec, md, gSSLKey.ivLength);
}

static void flipBytes(unsigned char *buf, int size)
{
    int i = 0;
    unsigned char revBuf[64];

    int bytesLeft = size;
    while (bytesLeft != 0) {
        int toFlip = min(sizeof(revBuf), bytesLeft);

        for (i = 0; i < toFlip; ++i) {
            revBuf[i] = buf[toFlip - (i + 1)];
        }

        memcpy(buf, revBuf, toFlip);
        bytesLeft -= toFlip;
        buf += toFlip;
    }
    memset(revBuf, 0, sizeof(revBuf));
}

static void shuffleBytes(unsigned char *buf, int size)
{
    int i = 0;

    for (i = 0; i < size - 1; ++i) {
        buf[i + 1] ^= buf[i];
    }
}

static void unshuffleBytes(unsigned char *buf, int size)
{
    int i = 0;

    for (i = size - 1; i != 0; --i) {
        buf[i] ^= buf[i - 1];
    }
}

/** Partial blocks are encoded with a stream cipher.  We make multiple passes on
 the data to ensure that the ends of the data depend on each other.
*/
bool streamEncode(fuse_req_t req, struct ovl_node *node, unsigned char *buf, int size, uint64_t iv64)
{
  unsigned char ivec[MAX_IVLENGTH];
  int dstLen = 0, tmpLen = 0;

  pthread_mutex_lock(&node->mutex);

  shuffleBytes(buf, size);

  setIVec(ivec, iv64);
  EVP_EncryptInit_ex(node->stream_enc, NULL, NULL, NULL, ivec);
  EVP_EncryptUpdate(node->stream_enc, buf, &dstLen, buf, size);
  EVP_EncryptFinal_ex(node->stream_enc, buf + dstLen, &tmpLen);

  flipBytes(buf, size);
  shuffleBytes(buf, size);

  setIVec(ivec, iv64 + 1);
  EVP_EncryptInit_ex(node->stream_enc, NULL, NULL, NULL, ivec);
  EVP_EncryptUpdate(node->stream_enc, buf, &dstLen, buf, size);
  EVP_EncryptFinal_ex(node->stream_enc, buf + dstLen, &tmpLen);

  dstLen += tmpLen;

  pthread_mutex_unlock(&node->mutex);
  if (dstLen != size)
  {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr,  "encoding %d bytes, got back %d (%d in final_ex)\n", size, dstLen, tmpLen);
    }
    return false;
  }

  return true;
}

bool streamDecode(fuse_req_t req, struct ovl_node *node, unsigned char *buf, int size, uint64_t iv64)
{
  unsigned char ivec[MAX_IVLENGTH];
  int dstLen = 0, tmpLen = 0;

  pthread_mutex_lock(&node->mutex);

  setIVec(ivec, iv64 + 1);
  EVP_DecryptInit_ex(node->stream_dec, NULL, NULL, NULL, ivec);
  EVP_DecryptUpdate(node->stream_dec, buf, &dstLen, buf, size);
  EVP_DecryptFinal_ex(node->stream_dec, buf + dstLen, &tmpLen);

  unshuffleBytes(buf, size);
  flipBytes(buf, size);

  setIVec(ivec, iv64);
  EVP_DecryptInit_ex(node->stream_dec, NULL, NULL, NULL, ivec);
  EVP_DecryptUpdate(node->stream_dec, buf, &dstLen, buf, size);
  EVP_DecryptFinal_ex(node->stream_dec, buf + dstLen, &tmpLen);

  unshuffleBytes(buf, size);
  dstLen += tmpLen;

  pthread_mutex_unlock(&node->mutex);
  
  if (dstLen != size)
  {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr,  "decoding %d bytes, got back %d (%d in final_ex)\n", size, dstLen, tmpLen);
    }
    return false;
  }

  return true;
}

bool blockEncode(fuse_req_t req, struct ovl_node *node, unsigned char *buf, int size, uint64_t iv64)
{
  unsigned char ivec[MAX_IVLENGTH];
  int dstLen = 0, tmpLen = 0;

  // data must be integer number of blocks
  const int blockMod = size % EVP_CIPHER_CTX_block_size(node->block_enc);
  if (blockMod != 0) {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr, "Invalid data size, not multiple of block size\n");
    }
    return false;
  }

  pthread_mutex_lock(&node->mutex);
  setIVec(ivec, iv64);

  EVP_EncryptInit_ex(node->block_enc, NULL, NULL, NULL, ivec);
  EVP_EncryptUpdate(node->block_enc, buf, &dstLen, buf, size);
  EVP_EncryptFinal_ex(node->block_enc, buf + dstLen, &tmpLen);
  dstLen += tmpLen;

  pthread_mutex_unlock(&node->mutex);

  if (dstLen != size)
  {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr,  "encoding %d bytes, got back %d (%d in final_ex)\n", size, dstLen, tmpLen);
    }
    return false;
  }

  return true;
}

bool blockDecode(fuse_req_t req, struct ovl_node *node, unsigned char *buf, int size, uint64_t iv64)
{
  unsigned char ivec[MAX_IVLENGTH];
  int dstLen = 0, tmpLen = 0;

  // data must be integer number of blocks
  const int blockMod = size % EVP_CIPHER_CTX_block_size(node->block_dec);
  if (blockMod != 0) {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr, "Invalid data size, not multiple of block size\n");
    }
    return false;
  }

  pthread_mutex_lock(&node->mutex);
  setIVec(ivec, iv64);

  EVP_DecryptInit_ex(node->block_dec, NULL, NULL, NULL, ivec);
  EVP_DecryptUpdate(node->block_dec, buf, &dstLen, buf, size);
  EVP_DecryptFinal_ex(node->block_dec, buf + dstLen, &tmpLen);
  dstLen += tmpLen;

  pthread_mutex_unlock(&node->mutex);

  if (dstLen != size)
  {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr,  "decoding %d bytes, got back %d (%d in final_ex)\n", size, dstLen, tmpLen);
    }
    return false;
  }

  return true;
}

void printhex(unsigned char *src,int len)
{
    int i=0;

    if(src==NULL)
    {
        return;
    }

    for(i=0;i<len;i++)
    {
        fprintf (stderr, "%02X", src[i]);
    }

    return;
}

/**
 * Read block from backing ciphertext file, decrypt it (normal mode)
 * or
 * Read block from backing plaintext file, then encrypt it (reverse mode)
 */
ssize_t readOneBlock(fuse_req_t req, struct ovl_node *node, const struct IORequest *blockReq)
{
  bool ok;
  int i = 0;
  off_t blockNum = blockReq->offset / gBlockSize;

  ssize_t readSize = pread(blockReq->fd, blockReq->data, blockReq->dataLen, blockReq->offset);
  if (readSize < 0) {
    int eno = errno;
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr, "read failed at fd %d offset %ld for %ld bytes: %s\n", blockReq->fd, blockReq->offset, blockReq->dataLen, strerror(eno));
    }
    readSize = -eno;
  }

  if (UNLIKELY (ovl_debug (req)))
  {
    fprintf (stderr, "readOneBlock raw(%ld)", readSize);
    //printhex((unsigned char*)blockReq->data, readSize);
    fprintf (stderr, "\n");
  }

  if (readSize > 0)
  {
    if (readSize != gBlockSize)
    {
      ok = streamDecode(req, node, blockReq->data, (int)readSize, blockNum ^ 0);
      // cast works because we work on a block and blocksize fit an int
    }
    else
    {
      if (gAllowHoles) {
        // special case - leave all 0's alone
        for (i = 0; i < readSize; ++i) {
          if (blockReq->data[i] != 0) {
            ok = blockDecode(req, node, blockReq->data, (int)readSize, blockNum ^ 0);
            break;
          }
        }
        ok = true;
      }
      else
      {
        ok = blockDecode(req, node, blockReq->data, (int)readSize, blockNum ^ 0);
      }
    }

    if (!ok) {
      if (UNLIKELY (ovl_debug (req)))
      {
        fprintf (stderr, "decodeBlock failed for block %ld, size %ld\n", blockNum, readSize);
      }
      readSize = -EBADMSG;
    }
  }
  else if (readSize == 0)
  {
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf (stderr, "readSize zero for offset %ld\n", blockReq->offset);
    }
  }

  if (UNLIKELY (ovl_debug (req)))
  {
    fprintf (stderr, "readOneBlock decode ok=%d(%ld)", ok, readSize);
    //printhex((unsigned char*)blockReq->data, readSize);
    fprintf (stderr, "\n");
  }

  return readSize;
}

ssize_t writeOneBlock(fuse_req_t req, struct ovl_node *node, const struct IORequest *blockReq)
{
    bool ok;
    ssize_t res = 0;
    void *buf = blockReq->data;
    ssize_t bytes = blockReq->dataLen;
    off_t offset = blockReq->offset;
    off_t blockNum = blockReq->offset / gBlockSize;

    if (blockReq->dataLen != gBlockSize) {
        ok = streamEncode(req, node, blockReq->data, (int)blockReq->dataLen, blockNum ^ 0);
        // cast works because we work on a block and blocksize fit an int
    } else {
        ok = blockEncode(req, node, blockReq->data, (int)blockReq->dataLen, blockNum ^ 0);
        // cast works because we work on a block and blocksize fit an int
    }
    
    if (UNLIKELY (ovl_debug (req)))
    {
        fprintf (stderr, "writeOneBlock encode ok=%d(%ld):", ok, blockReq->dataLen);
        //printhex((unsigned char*)blockReq->data, blockReq->dataLen);
        fprintf (stderr, "\n");
    }

    if (ok)
    {
        while (bytes != 0) {
            if (UNLIKELY (ovl_debug (req)))
            {
                fprintf (stderr, "pwrite at offset %ld for %ld bytes:%X\n", offset, bytes, *blockReq->data);
            }
            ssize_t writeSize = pwrite(blockReq->fd, buf, bytes, offset);

            if (writeSize < 0) {
                int eno = errno;
                if (UNLIKELY (ovl_debug (req)))
                {
                    fprintf (stderr, "pwrite failed at offset %ld for %ld bytes: %s\n", offset, bytes, strerror(eno));
                }
                // pwrite is not expected to return 0, so eno should always be set, but we never know...
                return -eno;
            }
            if (writeSize == 0) {
                return -EIO;
            }

            bytes -= writeSize;
            offset += writeSize;
            buf = (void *)((char *)buf + writeSize);
        }
        return blockReq->dataLen;
    } else {
        if (UNLIKELY (ovl_debug (req)))
        {
            fprintf (stderr, "encodeBlock failed for block %ld, size %ld\n", blockNum, blockReq->dataLen);
        }
        res = -EBADMSG;
    }

    return res;
}

static void clearCache(struct IORequest *req, unsigned int blockSize)
{
  memset(req->data, 0, blockSize);
  req->dataLen = 0;
}

/**
 * Serve a read request for the size of one block or less,
 * at block-aligned offsets.
 * Always requests full blocks form the lower layer, truncates the
 * returned data as neccessary.
 */
ssize_t cacheReadOneBlock(fuse_req_t req, struct ovl_node *node, const struct IORequest *blockReq)
{
  /* we can satisfy the request even if _cache.dataLen is too short, because
   * we always request a full block during reads. This just means we are
   * in the last block of a file, which may be smaller than the blocksize.
   * For reverse encryption, the cache must not be used at all, because
   * the lower file may have changed behind our back. */
  if ((blockReq->offset == node->cache.offset) && (node->cache.dataLen != 0)) {
    // satisfy request from cache
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf(stderr, "Read from cache offset=%ld, dataLen=%ld\n", node->cache.offset, node->cache.dataLen);
    }
    size_t len = blockReq->dataLen;
    if (node->cache.dataLen < len) {
      len = node->cache.dataLen;  // Don't read past EOF
    }
    memcpy(blockReq->data, node->cache.data, len);
    return len;
  }
  
  if (node->cache.dataLen > 0)
  {
    clearCache(&node->cache, gBlockSize);
  }

  // cache results of read -- issue reads for full blocks
  struct IORequest tmp;
  tmp.fd = blockReq->fd;
  tmp.offset = blockReq->offset;
  tmp.data = node->cache.data;
  tmp.dataLen = gBlockSize;
  ssize_t result = readOneBlock(req, node, &tmp);
  if (result > 0) {
    node->cache.offset = blockReq->offset;
    node->cache.dataLen = result;  // the amount we really have
    if ((size_t)result > blockReq->dataLen) {
      result = blockReq->dataLen;  // only as much as requested
    }
    memcpy(blockReq->data, node->cache.data, result);
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf(stderr, "cacheReadOneBlock save cache: offset=%ld, dataLen=%ld data=%s\n", blockReq->offset, blockReq->dataLen, blockReq->data);
    }
  }
  return result;
}

ssize_t cacheWriteOneBlock(fuse_req_t req, struct ovl_node *node, const struct IORequest *blockReq)
{
  // Let's point request buffer to our own buffer, as it may be modified by
  // encryption : originating process may not like to have its buffer modified
  memcpy(node->cache.data, blockReq->data, blockReq->dataLen);
  struct IORequest tmp;
  tmp.fd = blockReq->fd;
  tmp.offset = blockReq->offset;
  tmp.data = node->cache.data;
  tmp.dataLen = blockReq->dataLen;
  ssize_t res = writeOneBlock(req, node, &tmp);
  if (res < 0) {
    clearCache(&node->cache, gBlockSize);
  }
  else {
    // And now we can cache the write buffer from the request
    memcpy(node->cache.data, blockReq->data, blockReq->dataLen);
    node->cache.offset = blockReq->offset;
    node->cache.dataLen = blockReq->dataLen;
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf(stderr, "cacheWriteOneBlock save cache: offset=%ld, dataLen=%ld\n", blockReq->offset, blockReq->dataLen);
    }
  }
  return res;
}

ssize_t readBlocks(fuse_req_t req, struct ovl_node *node, const struct IORequest *blockReq)
{
    int partialOffset = blockReq->offset % gBlockSize;  // can be int as _blockSize is int
    off_t blockNum = blockReq->offset / gBlockSize;
    ssize_t result = 0;
    BUF_MEM *data = NULL;

    if (partialOffset == 0 && blockReq->dataLen <= gBlockSize)
    {
        // read completely within a single block -- can be handled as-is by readOneBlock().
        return cacheReadOneBlock(req, node, blockReq);
    }
    size_t size = blockReq->dataLen;

    // if the request is larger than a block, then request each block individually
    struct IORequest tmp;  // for requests we may need to make
    tmp.fd = blockReq->fd;
    tmp.dataLen = gBlockSize;
    tmp.data = NULL;

    unsigned char *out = blockReq->data;
    while (size != 0u)
    {
        tmp.offset = blockNum * gBlockSize;
        if (UNLIKELY (ovl_debug (req)))
        {
            fprintf(stderr, "readBlocks: offset=%ld, dataLen=%ld\n", tmp.offset, tmp.dataLen);
        }
        
        // if we're reading a full block, then read directly into the
        // result buffer instead of using a temporary
        if (partialOffset == 0 && size >= gBlockSize)
        {
            tmp.data = out;
        }
        else
        {
            if (data == NULL)
            {
                data = BUF_MEM_new();
                BUF_MEM_grow(data, gBlockSize);
            }
            tmp.data = (unsigned char *)(data->data);
        }

        ssize_t readSize = cacheReadOneBlock(req, node, &tmp);
        if (readSize < 0) {
            result = readSize;
            break;
        }
        if (readSize <= partialOffset) {
            break;  // didn't get enough bytes
        }

        size_t cpySize = min((size_t)readSize - (size_t)partialOffset, size);
        CHECK(cpySize <= (size_t)readSize);

        // if we read to a temporary buffer, then move the data
        if (tmp.data != out) {
            memcpy(out, tmp.data + partialOffset, cpySize);
        }

        result += cpySize;
        size -= cpySize;
        out += cpySize;
        ++blockNum;
        partialOffset = 0;

        if ((size_t)readSize < gBlockSize) {
            break;
        }
    }

    if (data != NULL) {
        BUF_MEM_free(data);
    }

    return result;
}

int padFile(fuse_req_t req, struct ovl_node *node, int fd, off_t oldSize, off_t newSize, bool forceWrite)
{
  struct IORequest blockReq;
  ssize_t res = 0;
  BUF_MEM *data = NULL;

  off_t oldLastBlock = oldSize / gBlockSize;
  off_t newLastBlock = newSize / gBlockSize;
  int newBlockSize = newSize % gBlockSize;  // can be int as _blockSize is int

  if (oldLastBlock == newLastBlock) {
    // when the real write occurs, it will have to read in the existing
    // data and pad it anyway, so we won't do it here (unless we're
    // forced).
    if (UNLIKELY (ovl_debug (req)))
    {
      fprintf(stderr, "optimization: not padding last block\n");
    }
  } else {
    data = BUF_MEM_new();
    BUF_MEM_grow(data, gBlockSize);
    blockReq.data = (unsigned char *)(data->data);

    // 1. extend the first block to full length
    // 2. write the middle empty blocks
    // 3. write the last block

    blockReq.fd = fd;
    blockReq.offset = oldLastBlock * gBlockSize;
    blockReq.dataLen = oldSize % gBlockSize;

    // 1. req.dataLen == 0, iff oldSize was already a multiple of blocksize
    if (blockReq.dataLen != 0) {
      if (UNLIKELY (ovl_debug (req)))
      {
        fprintf (stderr, "padding block %ld\n", oldLastBlock);
      }
      memset(data, 0, gBlockSize);
      if ((res = cacheReadOneBlock(req, node, &blockReq)) >= 0) {
        blockReq.dataLen = gBlockSize;  // expand to full block size
        res = cacheWriteOneBlock(req, node, &blockReq);
      }
      ++oldLastBlock;
    }

    // 2, pad zero blocks unless holes are allowed
    if (!gAllowHoles) {
      for (; (res >= 0) && (oldLastBlock != newLastBlock); ++oldLastBlock) {
        if (UNLIKELY (ovl_debug (req)))
        {
          fprintf (stderr, "padding block %ld\n", oldLastBlock);
        }
        blockReq.offset = oldLastBlock * gBlockSize;
        blockReq.dataLen = gBlockSize;
        memset(data, 0, blockReq.dataLen);
        res = cacheWriteOneBlock(req, node, &blockReq);
      }
    }

    // 3. only necessary if write is forced and block is non 0 length
    if ((res >= 0) && forceWrite && (newBlockSize != 0)) {
      blockReq.offset = newLastBlock * gBlockSize;
      blockReq.dataLen = newBlockSize;
      memset(data, 0, blockReq.dataLen);
      res = cacheWriteOneBlock(req, node, &blockReq);
    }
  }

  if (data != NULL) {
    BUF_MEM_free(data);
  }

  if (res < 0) {
    return res;
  }
  return 0;
}

/**
 * Returns the number of bytes written, or -errno in case of failure.
 */
ssize_t writeBlocks(fuse_req_t req, struct ovl_node *node, off_t fileSize, const struct IORequest *blockReq)
{
  BUF_MEM *data = NULL;
  
  // where write request begins
  off_t blockNum = blockReq->offset / gBlockSize;
  int partialOffset = blockReq->offset % gBlockSize;  // can be int as _blockSize is int

  // last block of file (for testing write overlaps with file boundary)
  off_t lastFileBlock = fileSize / gBlockSize;
  size_t lastBlockSize = fileSize % gBlockSize;

  if (UNLIKELY (ovl_debug (req)))
  {
    fprintf(stderr, "writeBlocks:fd=%d fileSize=%ld lastBlockSize=%ld\n", blockReq->fd, fileSize, lastBlockSize);
  }

  off_t lastNonEmptyBlock = lastFileBlock;
  if (lastBlockSize == 0) {
    --lastNonEmptyBlock;
  }

  if (blockReq->offset > fileSize) {
    // extend file first to fill hole with 0's..
    int res = padFile(req, node, blockReq->fd, fileSize, blockReq->offset, false);
    if (res < 0) {
      return res;
    }
  }

  // check against edge cases where we can just let the base class handle the request as-is..
  if (partialOffset == 0 && blockReq->dataLen <= gBlockSize) {
    // if writing a full block.. pretty safe..
    if (blockReq->dataLen == gBlockSize) {
      return cacheWriteOneBlock(req, node, blockReq);
    }

    // if writing a partial block, but at least as much as what is already there..
    if (blockNum == lastFileBlock && blockReq->dataLen >= lastBlockSize) {
      return cacheWriteOneBlock(req, node, blockReq);
    }
  }

  // have to merge data with existing block(s)..
  struct IORequest tmp;
  tmp.fd = blockReq->fd;
  tmp.data = NULL;
  tmp.dataLen = gBlockSize;

  ssize_t res = 0;
  size_t size = blockReq->dataLen;
  unsigned char *inPtr = blockReq->data;
  while (size != 0u) {
    tmp.offset = blockNum * gBlockSize;
    size_t toCopy = min((size_t)gBlockSize - (size_t)partialOffset, size);

    // if writing an entire block, or writing a partial block that requires no merging with existing data..
    if ((toCopy == gBlockSize) || (partialOffset == 0 && tmp.offset + (off_t)toCopy >= fileSize))
    {
      // write directly from buffer
      tmp.data = inPtr;
      tmp.dataLen = toCopy;
    } else {
      // need a temporary buffer, since we have to either merge or pad the data.
      if (data == NULL)
      {
          data = BUF_MEM_new();
          BUF_MEM_grow(data, gBlockSize);
      }
      memset(data->data, 0, gBlockSize);
      tmp.data = (unsigned char *)(data->data);

      if (blockNum > lastNonEmptyBlock) {
        // just pad..
        tmp.dataLen = partialOffset + toCopy;
      } else {
        // have to merge with existing block data..
        tmp.dataLen = gBlockSize;
        ssize_t readSize = cacheReadOneBlock(req, node, &tmp);
        if (readSize < 0) {
          res = readSize;
          break;
        }
        tmp.dataLen = readSize;

        // extend data if necessary..
        if (partialOffset + toCopy > tmp.dataLen) {
          tmp.dataLen = partialOffset + toCopy;
        }
      }
      // merge in the data to be written..
      memcpy(tmp.data + partialOffset, inPtr, toCopy);
    }

    // Finally, write the damn thing!
    res = cacheWriteOneBlock(req, node, &tmp);
    if (res < 0) {
      break;
    }

    // prepare to start all over with the next block..
    size -= toCopy;
    inPtr += toCopy;
    ++blockNum;
    partialOffset = 0;
  }

  if (data != NULL) {
      BUF_MEM_free(data);
  }

  if (res < 0) {
    return res;
  }
  return blockReq->dataLen;
}

ssize_t fileEncode(fuse_req_t req, struct ovl_node *node, int sfd, int dfd, off_t fileSize)
{
    BUF_MEM *data = NULL;
    size_t blockIndex = 0;
    ssize_t res = 0;
    ssize_t total = 0;
    struct IORequest tmp;

    if (fileSize <= 0) {
      return fileSize;
    }

    // last block of file (for testing write overlaps with file boundary)
    off_t lastFileBlock = fileSize / gBlockSize;
    size_t lastBlockSize = fileSize % gBlockSize;

    if (UNLIKELY (ovl_debug (req)))
    {
        fprintf(stderr, "fileEncode:dfd=%d fileSize=%ld lastBlockSize=%ld\n", dfd, fileSize, lastBlockSize);
    }

    off_t lastNonEmptyBlock = lastFileBlock;
    if (lastBlockSize == 0) {
        --lastNonEmptyBlock;
    }

    data = BUF_MEM_new();
    BUF_MEM_grow(data, gBlockSize);
    tmp.fd = dfd;
    tmp.data = (unsigned char *)(data->data);
    for (blockIndex = 0; blockIndex <= lastNonEmptyBlock; blockIndex++)
    {
        tmp.offset = blockIndex * gBlockSize;
        if (blockIndex == lastNonEmptyBlock && lastBlockSize>0)
        {
            tmp.dataLen = lastBlockSize;
        }
        else
        {
            tmp.dataLen = gBlockSize;
        }
        memset(tmp.data, 0, gBlockSize);
        ssize_t readSize = pread(sfd, tmp.data, tmp.dataLen, tmp.offset);
        if (readSize < 0) {
            int eno = errno;
            if (UNLIKELY (ovl_debug (req)))
            {
                fprintf (stderr, "read failed at fd %d offset %ld for %ld bytes: %s\n", sfd, tmp.offset, tmp.dataLen, strerror(eno));
            }
            readSize = -eno;
        }
        assert(readSize==tmp.dataLen);
        res = writeOneBlock(req, node, &tmp);
        if (res < 0) {
            break;
        }
        total += tmp.dataLen;
    }

    assert(total==fileSize);

    if (data != NULL) {
        BUF_MEM_free(data);
    }

    if (res < 0) {
        return res;
    }
    
    return total;
}

static int encode_fd_to_fd (fuse_req_t req, struct ovl_node *node, int sfd, int dfd, char *buf, size_t buf_size, off_t fileSize)
{
    int ret;
    int nread = 0;
    int written = 0;
    ssize_t total = 0;
    struct IORequest tmp;

    tmp.fd = dfd;
    tmp.data = (unsigned char *)buf;
    tmp.offset = 0;
    for (;;)
    {
        memset(buf, 0, buf_size);
        nread = TEMP_FAILURE_RETRY (read (sfd, buf, gBlockSize));
        if (nread < 0)
            return nread;

        if (nread == 0)
            break;

        tmp.dataLen = nread;
        written = writeOneBlock(req, node, &tmp);
        if (written < 0) {
            break;
        }
        assert(written==nread);
        total += written;
        tmp.offset += nread;
    }

    assert(total==fileSize);
    return 0;
}

static int copy_fd_to_fd (int sfd, int dfd, char *buf, size_t buf_size)
{
  int ret;

  for (;;)
    {
      int written;
      int nread;

      nread = TEMP_FAILURE_RETRY (read (sfd, buf, buf_size));
      if (nread < 0)
        return nread;

      if (nread == 0)
        break;

      written = 0;
      do
        {
          ret = TEMP_FAILURE_RETRY (write (dfd, buf + written, nread));
          if (ret < 0)
            return ret;
          nread -= ret;
          written += ret;
        }
      while (nread);
    }
  return 0;
}

static int
copyup (fuse_req_t req, struct ovl_data *lo, struct ovl_node *node)
{
  int saved_errno;
  int ret = -1;
  cleanup_close int dfd = -1;
  cleanup_close int sfd = -1;
  cleanup_close int ufd = -1;
  struct stat st;
  const size_t buf_size = 1 << 20;
  cleanup_free char *buf = NULL;
  struct timespec times[2];
  char wd_tmp_file_name[32];
  //static bool support_reflinks = true;
  //bool data_copied = false;
  mode_t mode;
  struct ovl_layer *l;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "copyup(ino=%" PRIu64 ", name=%s)\n", node->ino->ino, node->path);

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  ret = node->layer->ds->statat (node->layer, node->path, &st, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS);
  if (ret < 0)
    return ret;

  if (node->parent)
    {
      ret = create_node_directory (lo, node->parent);
      if (ret < 0)
        return ret;
    }

  mode = st.st_mode;
  if (lo->xattr_permissions)
    mode |= 0755;
  if (lo->euid > 0)
    mode |= 0200;

  if ((mode & S_IFMT) == S_IFDIR)
    {
      ret = create_node_directory (lo, node);
      if (ret < 0)
        goto exit;
      goto success;
    }

  if ((mode & S_IFMT) == S_IFLNK)
    {
      size_t current_size = PATH_MAX + 1;
      cleanup_free char *p = malloc (current_size);

      while (1)
        {
          char *new;

          ret = node->layer->ds->readlinkat (node->layer, node->path, p, current_size - 1);
          if (ret < 0)
            goto exit;
          if (ret < current_size - 1)
            break;

          current_size = current_size * 2;
          new = realloc (p, current_size);
          if (new == NULL)
            goto exit;
          p = new;
        }
      p[ret] = '\0';
      ret = symlinkat (p, get_upper_layer (lo)->fd, node->path);
      if (ret < 0)
        goto exit;
      goto success;
    }

  ret = sfd = node->layer->ds->openat (node->layer, node->path, O_RDONLY|O_NONBLOCK, 0);
  if (sfd < 0)
    goto exit;

  ret = dfd = TEMP_FAILURE_RETRY (safe_openat (lo->workdir_fd, wd_tmp_file_name, O_CREAT|O_RDWR, mode));
  if (dfd < 0)
    goto exit;

  if (st.st_uid != lo->uid || st.st_gid != lo->gid || get_upper_layer (lo)->stat_override_mode != STAT_OVERRIDE_NONE)
    {
      ret = do_fchown (lo, dfd, st.st_uid, st.st_gid, mode);
      if (ret < 0)
        goto exit;
    }

  buf = malloc (buf_size);
  if (buf == NULL)
    goto exit;

  /*
  if (support_reflinks)
    {
      if (ioctl (dfd, FICLONE, sfd) >= 0)
        data_copied = true;
      else if (errno == ENOTSUP || errno == EINVAL)
        {
          // Fallback to data copy and don't attempt again FICLONE.
          support_reflinks = false;
        }
    }

#ifdef HAVE_SYS_SENDFILE_H
  if (! data_copied)
    {
      off_t copied = 0;

      while (copied < st.st_size)
        {
          off_t tocopy = st.st_size - copied;
          ssize_t n = TEMP_FAILURE_RETRY (sendfile (dfd, sfd, NULL, tocopy > SIZE_MAX ? SIZE_MAX : (size_t) tocopy));
          if (n < 0)
            {
              // On failure, fallback to the read/write loop.
              ret = copy_fd_to_fd (sfd, dfd, buf, buf_size);
              if (ret < 0)
                goto exit;
              break;
            }
          copied += n;
	}
      data_copied = true;
    }
#endif

  if (! data_copied)
    {
      ret = copy_fd_to_fd (sfd, dfd, buf, buf_size);
      if (ret < 0)
        goto exit;
    }
  */
  ret = encode_fd_to_fd (req, node, sfd, dfd, buf, buf_size, st.st_size);
  if (ret < 0)
    goto exit;

  times[0] = st.st_atim;
  times[1] = st.st_mtim;
  ret = futimens (dfd, times);
  if (ret < 0)
    goto exit;

  ret = copy_xattr (sfd, dfd, buf, buf_size);
  if (ret < 0)
    goto exit;

  ret = set_fd_origin (dfd, node->path);
  if (ret < 0)
    goto exit;

  /* Finally, move the file to its destination.  */
  /*
  l = get_upper_layer (lo);
  ufd = l->ds->openat (l, node->path, O_CREAT|O_WRONLY, mode);
  ret = fileEncode(req, node, dfd, dfd, st.st_size);
  if (ret < 0)
    goto exit;
  */

  ret = renameat (lo->workdir_fd, wd_tmp_file_name, get_upper_layer (lo)->fd, node->path);
  if (ret < 0)
    goto exit;

  if (node->parent)
    {
      char whpath[PATH_MAX];

      strconcat3 (whpath, PATH_MAX, node->parent->path, "/.wh.", node->name);

      if (unlinkat (get_upper_layer (lo)->fd, whpath, 0) < 0 && errno != ENOENT)
        goto exit;
    }

 success:
  ret = 0;

  node->layer = get_upper_layer (lo);

 exit:
  saved_errno = errno;
  if (ret < 0)
    unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
  errno = saved_errno;

  return ret;
}

static struct ovl_node *
get_node_up (fuse_req_t req, struct ovl_data *lo, struct ovl_node *node)
{
  int ret;

  if (lo->upperdir == NULL)
    {
      errno = EROFS;
      return NULL;
    }

  if (node->layer == get_upper_layer (lo))
    return node;

  ret = copyup (req, lo, node);
  if (ret < 0)
    return NULL;

  assert (node->layer == get_upper_layer (lo));

  return node;
}

static size_t
count_dir_entries (struct ovl_node *node, size_t *whiteouts)
{
  size_t c = 0;
  struct ovl_node *it;

  if (whiteouts)
    *whiteouts = 0;

  for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
    {
      if (it->whiteout)
        {
          if (whiteouts)
            (*whiteouts)++;
          continue;
        }
      if (strcmp (it->name, ".") == 0)
        continue;
      if (strcmp (it->name, "..") == 0)
        continue;
      c++;
    }
  return c;
}

static int
update_paths (struct ovl_node *node)
{
  struct ovl_node *it;

  if (node == NULL)
    return 0;

  if (node->parent)
    {
      free (node->path);
      if (asprintf (&node->path, "%s/%s", node->parent->path, node->name) < 0)
        {
          node->path = NULL;
          return -1;
        }
    }

  if (node->children)
    {
      for (it = hash_get_first (node->children); it; it = hash_get_next (node->children, it))
        {
          if (update_paths (it) < 0)
            return -1;
        }
    }

  return 0;
}

static int
empty_dir (struct ovl_layer *l, const char *path)
{
  cleanup_close int cleanup_fd = -1;
  int ret;

  cleanup_fd = TEMP_FAILURE_RETRY (safe_openat (l->fd, path, O_DIRECTORY, 0));
  if (cleanup_fd < 0)
    return -1;

  if (set_fd_opaque (cleanup_fd) < 0)
    return -1;

  ret = empty_dirfd (cleanup_fd);

  cleanup_fd = -1;

  return ret;
}

static void
do_rm (fuse_req_t req, fuse_ino_t parent, const char *name, bool dirp)
{
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode;
  int ret = 0;
  size_t whiteouts = 0;
  struct ovl_node key, *rm;

  node = do_lookup_file (req, lo, parent, name);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (dirp)
    {
      size_t c;

      /* Re-load the directory.  */
      node = reload_dir (lo, node);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      c = count_dir_entries (node, &whiteouts);
      if (c)
        {
          fuse_reply_err (req, ENOTEMPTY);
          return;
        }
    }

  if (node->layer == get_upper_layer (lo))
    {
      if (! dirp)
        node->do_unlink = 1;
      else
        {
          if (whiteouts > 0)
            {
              if (empty_dir (get_upper_layer (lo), node->path) < 0)
                {
                  fuse_reply_err (req, errno);
                  return;
                }
            }

          node->do_rmdir = 1;
        }
    }

  pnode = do_lookup_file (req, lo, parent, NULL);
  if (pnode == NULL || pnode->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  /* If the node is still accessible then be sure we
     can write to it.  Fix it to be done when a write is
     really done, not now.  */
  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  node_set_name (&key, (char *) name);

  rm = hash_delete (pnode->children, &key);
  if (rm)
    {
      ret = hide_node (lo, rm, true);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }

      node_free (rm);
    }

  fuse_reply_err (req, ret);
}

static void
ovl_unlink (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_unlink(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);
  
  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  do_rm (req, parent, name, false);
}

static void
ovl_rmdir (fuse_req_t req, fuse_ino_t parent, const char *name)
{
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_rmdir(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);

  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  do_rm (req, parent, name, true);
}

static int
direct_setxattr (struct ovl_layer *l, const char *path, const char *name, const char *buf, size_t size, int flags)
{
  cleanup_close int fd = -1;
  char full_path[PATH_MAX];
  int ret;

  full_path[0] = '\0';
  ret = open_fd_or_get_path (l, path, full_path, &fd, O_WRONLY);
  if (ret < 0)
    return ret;

  if (fd >= 0)
    return fsetxattr (fd, name, buf, size, flags);

  return setxattr (full_path, name, buf, size, flags);
}

static void
ovl_setxattr (fuse_req_t req, fuse_ino_t ino, const char *name,
             const char *value, size_t size, int flags)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  int ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_setxattr(ino=%" PRIu64 ", name=%s, value=%s, size=%zu, flags=%d)\n", ino, name,
             value, size, flags);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if (lo->disable_xattrs)
    {
      fuse_reply_err (req, ENOSYS);
      return;
    }

  if (has_prefix (name, PRIVILEGED_XATTR_PREFIX) || has_prefix (name, XATTR_PREFIX)  || has_prefix (name, XATTR_CONTAINERS_PREFIX))
    {
      fuse_reply_err (req, EPERM);
      return;
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (! node->hidden)
    ret = direct_setxattr (node->layer, node->path, name, value, size, flags);
  else
    {
      char path[PATH_MAX];
      strconcat3 (path, PATH_MAX, lo->workdir, "/", node->path);
      ret = setxattr (path, name, value, size, flags);
    }
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fuse_reply_err (req, 0);
}

static int
direct_removexattr (struct ovl_layer *l, const char *path, const char *name)
{
  cleanup_close int fd = -1;
  char full_path[PATH_MAX];
  int ret;

  full_path[0] = '\0';
  ret = open_fd_or_get_path (l, path, full_path, &fd, O_WRONLY);
  if (ret < 0)
    return ret;

  if (fd >= 0)
    return fremovexattr (fd, name);

  return lremovexattr (full_path, name);
}

static void
ovl_removexattr (fuse_req_t req, fuse_ino_t ino, const char *name)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  int ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_removexattr(ino=%" PRIu64 ", name=%s)\n", ino, name);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (! node->hidden)
    ret = direct_removexattr (node->layer, node->path, name);
  else
    {
      char path[PATH_MAX];
      strconcat3 (path, PATH_MAX, lo->workdir, "/", node->path);
      ret = removexattr (path, name);
    }

  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fuse_reply_err (req, 0);
}

static int
direct_create_file (struct ovl_layer *l, int dirfd, const char *path, uid_t uid, gid_t gid, int flags, mode_t mode)
{
  struct ovl_data *lo = l->ovl_data;
  cleanup_close int fd = -1;
  char wd_tmp_file_name[32];
  int ret;

  /* try to create directly the file if it doesn't need to be chowned.  */
  if (uid == lo->uid && gid == lo->gid && l->stat_override_mode == STAT_OVERRIDE_NONE)
    {
      ret = TEMP_FAILURE_RETRY (safe_openat (get_upper_layer (lo)->fd, path, flags, mode));
      if (ret >= 0)
        return ret;
      /* if it fails (e.g. there is a whiteout) then fallback to create it in
         the working dir + rename.  */
    }

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  fd = TEMP_FAILURE_RETRY (safe_openat (lo->workdir_fd, wd_tmp_file_name, flags, mode));
  if (fd < 0)
    return -1;
  if (uid != lo->uid || gid != lo->gid || l->stat_override_mode != STAT_OVERRIDE_NONE)
    {
      if (do_fchown (lo, fd, uid, gid, mode) < 0)
        {
          unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
          return -1;
        }
    }

  if (renameat (lo->workdir_fd, wd_tmp_file_name, get_upper_layer (lo)->fd, path) < 0)
    {
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      return -1;
    }

  ret = fd;
  fd = -1;
  return ret;
}

static int
ovl_do_open (fuse_req_t req, fuse_ino_t parent, const char *name, int flags, mode_t mode, struct ovl_node **retnode, struct stat *st)
{
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *n;
  bool readonly = (flags & (O_APPEND | O_RDWR | O_WRONLY | O_CREAT | O_TRUNC)) == 0;
  cleanup_free char *path = NULL;
  cleanup_close int fd = -1;
  uid_t uid;
  gid_t gid;
  bool need_delete_whiteout = true;
  bool is_whiteout = false;

  flags |= O_NOFOLLOW;

  flags &= ~O_DIRECT;

  if (lo->writeback)
    {
      if ((flags & O_ACCMODE) == O_WRONLY)
        {
          flags &= ~O_ACCMODE;
          flags |= O_RDWR;
        }
      if (flags & O_APPEND)
        flags &= ~O_APPEND;
    }

  if (name && has_prefix (name, ".wh."))
    {
      errno = EINVAL;
      return - 1;
    }

  n = do_lookup_file (req, lo, parent, name);
  if (n && n->hidden)
    {
      if (retnode)
        *retnode = n;

      return openat (n->hidden_dirfd, n->path, flags, mode);
    }
  if (n && !n->whiteout && (flags & O_CREAT))
    {
      errno = EEXIST;
      return -1;
    }
  if (n && n->whiteout)
    {
      n = NULL;
      is_whiteout = true;
    }

  if (!n)
    {
      int ret;
      struct ovl_node *p;
      const struct fuse_ctx *ctx = fuse_req_ctx (req);
      char wd_tmp_file_name[32];
      struct stat st_tmp;

      if ((flags & O_CREAT) == 0)
        {
          errno = ENOENT;
          return -1;
        }

      p = do_lookup_file (req, lo, parent, NULL);
      if (p == NULL)
        {
          errno = ENOENT;
          return -1;
        }

      p = get_node_up (req, lo, p);
      if (p == NULL)
        return -1;

      if (p->loaded && !is_whiteout)
        need_delete_whiteout = false;

      sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

      ret = asprintf (&path, "%s/%s", p->path, name);
      if (ret < 0)
        return ret;

      uid = get_uid (lo, ctx->uid);
      gid = get_gid (lo, ctx->gid);

      fd = direct_create_file (get_upper_layer (lo), get_upper_layer (lo)->fd, path, uid, gid, flags, (mode & ~ctx->umask) | (lo->xattr_permissions ? 0755 : 0));
      if (fd < 0)
        return fd;

      if (need_delete_whiteout && delete_whiteout (lo, -1, p, name) < 0)
        return -1;

      if (st == NULL)
        st = &st_tmp;

      if (get_upper_layer (lo)->ds->fstat (get_upper_layer (lo), fd, path, STATX_BASIC_STATS, st) < 0)
        return -1;

      n = make_ovl_node (lo, path, get_upper_layer (lo), name, st->st_ino, st->st_dev, false, p, lo->fast_ino_check);
      if (n == NULL)
        {
          errno = ENOMEM;
          return -1;
        }
      if (!is_whiteout)
        n->last_layer = get_upper_layer (lo);

      n = insert_node (p, n, true);
      if (n == NULL)
        {
          errno = ENOMEM;
          return -1;
        }
      ret = fd;
      fd = -1; /*  We use a temporary variable so we don't close it at cleanup.  */
      if (retnode)
        *retnode = n;
      return ret;
    }

  /* readonly, we can use both lowerdir and upperdir.  */
  if (readonly)
    {
      struct ovl_layer *l = n->layer;
      if (retnode)
        *retnode = n;
      return l->ds->openat (l, n->path, flags, mode);
    }
  else
    {
      struct ovl_layer *l;

      n = get_node_up (req, lo, n);
      if (n == NULL)
        return -1;

      if (retnode)
        *retnode = n;

      l = n->layer;

      return l->ds->openat (l, n->path, flags, mode);
    }
}

static void ovl_read (fuse_req_t req, fuse_ino_t ino, size_t size,
	 off_t offset, struct fuse_file_info *fi)
{
    ssize_t res = 0;
    uint64_t fh;
    char* buffer = NULL;
    struct IORequest blockReq;
    struct ovl_node *node;
    struct ovl_data *lo = ovl_data (req);

    node = inode_to_node (lo, ino);
    if (UNLIKELY (ovl_debug (req)))
        fprintf (stderr, "ovl_read(ino=%" PRIu64 ", path=%s, size=%zd, off=%lu)\n", ino, node->path, size, (unsigned long) offset);

    if (!checkAuthority(req, ino)) {
        fuse_reply_err (req, ENOENT);
        return;
    }

    //fprintf (stderr, "path=%s name=%s layer=%p last_layer=%p lower=%p upper=%p\n", node->path, node->name, node->layer, node->last_layer, get_lower_layers (lo), get_upper_layer (lo));
    if (node->layer == get_upper_layer (lo) || node->last_layer == get_upper_layer (lo))
    {
        buffer = (char *)malloc(size);
        memset(buffer, 0 , size);
        
        blockReq.fd = fi->fh;
        blockReq.offset = offset;
        blockReq.dataLen = size;
        blockReq.data = buffer;
        pthread_mutex_lock (&lock);
        readBlocks(req, node, &blockReq);
        pthread_mutex_unlock (&lock);

        if (UNLIKELY (ovl_debug (req)))
        {
            fprintf (stderr, "ovl_read decode(%ld):%s\n", size, buffer);
        }
        
        //reply_buf_limited(req, buffer, size, offset, size);
        fuse_reply_buf(req, buffer, size);
    }
    else
    {
        struct fuse_bufvec buf = FUSE_BUFVEC_INIT (size);

        buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK | FUSE_BUF_FD_RETRY;
        buf.buf[0].fd = fi->fh;
        buf.buf[0].pos = offset;

        fuse_reply_data (req, &buf, 0);
    }

    free(buffer);
}

static void ovl_write_buf (fuse_req_t req, fuse_ino_t ino,
	      struct fuse_bufvec *in_buf, off_t off,
	      struct fuse_file_info *fi)
{
    size_t len;
    size_t total = 0;
    struct ovl_data *lo = ovl_data (req);
    ssize_t res;
    struct ovl_node *node;
    struct ovl_ino *inode;
    int saved_errno;
    struct fuse_buf *buf = NULL;
    struct IORequest blockReq;
    struct stat s;
    int ret = 0;

    struct fuse_bufvec out_buf = FUSE_BUFVEC_INIT (fuse_buf_size (in_buf));

    out_buf.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK | FUSE_BUF_FD_RETRY;
    out_buf.buf[0].fd = fi->fh;
    out_buf.buf[0].pos = off;

    node = inode_to_node (lo, ino);
    if (node == NULL || node->whiteout)
    {
        fuse_reply_err (req, ENOENT);
        return;
    }

    if (UNLIKELY (ovl_debug (req)))
    {
        fprintf (stderr, "ovl_write_buf(ino=%" PRIu64 ", size=%zd, off=%lu, fd=%d, path=%s)\n",
            ino, out_buf.buf[0].size, (unsigned long) off, (int) fi->fh, node->path);
    }

    if (!checkAuthority(req, ino)) {
        fuse_reply_err (req, ENOENT);
        return;
    }

    //fprintf (stderr, "path=%s name=%s layer=%p last_layer=%p lower=%p upper=%p\n", node->path, node->name, node->layer, node->last_layer, get_lower_layers (lo), get_upper_layer (lo));
    if (node->layer == get_upper_layer (lo) || node->last_layer == get_upper_layer (lo))
    {
        buf = &in_buf->buf[0];
        blockReq.fd = node->layer->ds->openat (node->layer, node->path, O_RDWR, node->ino->mode);
        blockReq.offset = off;
        blockReq.dataLen = buf->size;

        if(buf->flags & FUSE_BUF_IS_FD)
        {
            res = read(buf->fd, buf->mem+in_buf->off, buf->size);
            if (res < 0) {
                saved_errno = errno;
                if (UNLIKELY (ovl_debug (req)))
                {
                    fprintf (stderr, "read failed at fd %d offset %ld for %ld bytes: %s\n", buf->fd, in_buf->off, buf->size, strerror(saved_errno));
                }
                fuse_reply_err (req, saved_errno);
                return;
            }
        }
        blockReq.data = (unsigned char *)buf->mem;

        if (UNLIKELY (ovl_debug (req)))
        {
            fprintf (stderr, "ovl_write_buf(%ld, %ld, %ld):", buf->size, in_buf->off, buf->pos);
            //printhex((unsigned char*)buf->mem, buf->size);
            fprintf (stderr, "\n");
        }

        pthread_mutex_lock (&lock);
        ret = rpl_stat (req, node, -1, NULL, NULL, &s);
        if (ret)
        {
          fuse_reply_err (req, errno);
          return;
        }

        res = writeBlocks(req, node, s.st_size, &blockReq);
        saved_errno = errno;
        pthread_mutex_unlock (&lock);
        
        close(blockReq.fd);
    }
    else
    {
        errno = 0;
        res = fuse_buf_copy (&out_buf, in_buf, 0);
        saved_errno = errno;
    }

    inode = lookup_inode (lo, ino);
    /* if it is a writepage request, make sure to restore the setuid bit.  */
    if (fi->writepage && (inode->mode & (S_ISUID|S_ISGID)))
    {
        if (do_fchmod (lo, fi->fh, inode->mode) < 0)
        {
            fuse_reply_err (req, errno);
            return;
        }
    }

    fprintf (stderr, "ovl_write_buf(res=%ld)", res);
    if (res < 0)
    {
        fprintf (stderr, "ovl_write_buf(res=%ld, saved_errno=%d)", res, saved_errno);
        fuse_reply_err (req, saved_errno);
    }
    else
        fuse_reply_write (req, (size_t) res);
}

static void
ovl_release (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  int ret;
  (void) ino;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_release(ino=%" PRIu64 ")\n", ino);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  ret = close (fi->fh);
  fuse_reply_err (req, ret == 0 ? 0 : errno);
}

static int
do_getattr (fuse_req_t req, struct fuse_entry_param *e, struct ovl_node *node, int fd, const char *path)
{
  struct ovl_data *lo = ovl_data (req);
  int err = 0;

  memset (e, 0, sizeof (*e));

  err = rpl_stat (req, node, fd, path, NULL, &e->attr);
  if (err < 0)
    return err;

  e->ino = node_to_inode (node);
  e->attr_timeout = get_timeout (lo);
  e->entry_timeout = get_timeout (lo);

  return 0;
}

static int
do_statfs (struct ovl_data *lo, struct statvfs *sfs)
{
  int ret, fd;

  fd = get_first_layer (lo)->fd;

  if (fd >= 0)
    ret = fstatvfs (fd, sfs);
  else
    ret = statvfs (lo->mountpoint, sfs);
  if (ret < 0)
    return ret;

  sfs->f_namemax -= WHITEOUT_MAX_LEN;
  return 0;
}

static short
get_fs_namemax (struct ovl_data *lo)
{
  static short namemax = 0;
  if (namemax == 0)
    {
      struct statvfs sfs;
      int ret;

      ret = do_statfs (lo, &sfs);
      /* On errors use a sane default.  */
      if (ret < 0)
        namemax = 255 - WHITEOUT_MAX_LEN;
      else
        namemax = sfs.f_namemax;
    }
  return namemax;
}

static void
ovl_create (fuse_req_t req, fuse_ino_t parent, const char *name,
	   mode_t mode, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();
  cleanup_close int fd = -1;
  struct fuse_entry_param e;
  struct ovl_node *p, *node = NULL;
  struct ovl_data *lo = ovl_data (req);
  struct stat st;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_create(parent=%" PRIu64 ", name=%s)\n",
	     parent, name);

  if (!checkAuthority(req, parent)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  if (strlen (name) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }

  fi->flags = fi->flags | O_CREAT;

  if (lo->xattr_permissions)
    mode |= 0755;

  fd = ovl_do_open (req, parent, name, fi->flags, mode, &node, &st);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  p = do_lookup_file (req, lo, parent, NULL);
  /* Make sure the cache is invalidated, if the parent is in the middle of a readdir. */
  if (p && p->in_readdir)
    fuse_lowlevel_notify_inval_inode (lo->se, parent, 0, 0);

  if (node == NULL || do_getattr (req, &e, node, fd, NULL) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fi->fh = fd;
  fd = -1;  /* Do not clean it up.  */

  node->ino->lookups++;
  fuse_reply_create (req, &e, fi);
}

static void
ovl_open (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  struct ovl_data *lo = ovl_data (req);
  cleanup_lock int l = enter_big_lock ();
  cleanup_close int fd = -1;
  int flags = 0;
  struct ovl_node *node;

  if(fi->flags&O_WRONLY)
  {
    //readOneBlock need the O_RDWR to pread
    flags = fi->flags&(~O_WRONLY)&(~O_APPEND)|O_RDWR;
  }

  node = inode_to_node (lo, ino);
  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_open(ino=%" PRIu64 ", path=%s, flags=%d)\n", ino, node->path, fi->flags);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  fd = ovl_do_open (req, ino, NULL, fi->flags, 0700, NULL, NULL);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }
  fi->fh = fd;
  if (get_timeout (lo) > 0)
    fi->keep_cache = 1;
  fd = -1;  /* Do not clean it up.  */
  fuse_reply_open (req, fi);
}

static void
ovl_getattr (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node;
  struct fuse_entry_param e;

  node = do_lookup_file (req, lo, ino, NULL);
  if (UNLIKELY (ovl_debug (req)))
  {
    fprintf (stderr, "ovl_getattr(ino=%" PRIu64 ", path=%s)\n", ino, node->path);
    syslog(LOG_INFO, "ovl_getattr(ino=%" PRIu64 ", path=%s)\n", ino, node->path);
  }


  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }
  
  if (node == NULL || node->whiteout)
  {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (do_getattr (req, &e, node, -1, NULL) < 0)
  {
    fuse_reply_err (req, errno);
    return;
  }

  fuse_reply_attr (req, &e.attr, get_timeout (lo));
}

static void
ovl_setattr (fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  cleanup_close int cleaned_up_fd = -1;
  struct ovl_node *node;
  struct fuse_entry_param e;
  struct timespec times[2];
  uid_t uid;
  gid_t gid;
  int ret;
  int fd = -1;
  char path[PATH_MAX];

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_setattr(ino=%" PRIu64 ", to_set=%d)\n", ino, to_set);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (to_set & FUSE_SET_ATTR_CTIME)
    {
      /* Ignore request.  */
    }

  uid = -1;
  gid = -1;
  if (to_set & FUSE_SET_ATTR_UID)
    uid = get_uid (lo, attr->st_uid);
  if (to_set & FUSE_SET_ATTR_GID)
    gid = get_gid (lo, attr->st_gid);

  if (fi != NULL)
    fd = fi->fh;  // use existing fd if fuse_file_info is available
  else
    {
      mode_t mode = node->ino->mode;
      int dirfd = node_dirfd (node);

      if (mode == 0)
        {
          struct stat st;

          ret = fstatat (dirfd, node->path, &st, AT_SYMLINK_NOFOLLOW);
          if (ret < 0)
            {
              fuse_reply_err (req, errno);
              return;
            }
          node->ino->mode = mode = st.st_mode;
        }

      switch (mode & S_IFMT)
        {
        case S_IFREG:
          cleaned_up_fd = fd = TEMP_FAILURE_RETRY (safe_openat (dirfd, node->path, O_NOFOLLOW|O_NONBLOCK|(to_set & FUSE_SET_ATTR_SIZE ? O_WRONLY : 0), 0));
          if (fd < 0)
            {
              fuse_reply_err (req, errno);
              return;
            }
          break;

        case S_IFDIR:
          cleaned_up_fd = fd = TEMP_FAILURE_RETRY (safe_openat (dirfd, node->path, O_NOFOLLOW|O_NONBLOCK, 0));
          if (fd < 0)
            {
              if (errno != ELOOP)
                {
                  fuse_reply_err (req, errno);
                  return;
                }
            }
          break;

        case S_IFLNK:
          cleaned_up_fd = TEMP_FAILURE_RETRY (safe_openat (dirfd, node->path, O_PATH|O_NOFOLLOW|O_NONBLOCK, 0));
          if (cleaned_up_fd < 0)
            {
              fuse_reply_err (req, errno);
              return;
            }
          sprintf (path, "/proc/self/fd/%d", cleaned_up_fd);
          break;

        default:
          strconcat3 (path, PATH_MAX, get_upper_layer (lo)->path, "/", node->path);
          break;
        }
    }

  l = release_big_lock ();

  memset (times, 0, sizeof (times));
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_nsec = UTIME_OMIT;
  if (to_set & FUSE_SET_ATTR_ATIME)
    {
      times[0] = attr->st_atim;
      if (to_set & FUSE_SET_ATTR_ATIME_NOW)
        times[0].tv_nsec = UTIME_NOW;
    }

  if (to_set & FUSE_SET_ATTR_MTIME)
    {
      times[1] = attr->st_mtim;
      if (to_set & FUSE_SET_ATTR_MTIME_NOW)
        times[1].tv_nsec = UTIME_NOW;
    }

  if (times[0].tv_nsec != UTIME_OMIT || times[1].tv_nsec != UTIME_OMIT)
    {
      if (fd >= 0)
        ret = futimens (fd, times);
      else
        ret = utimensat (AT_FDCWD, path, times, 0);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  if (to_set & FUSE_SET_ATTR_MODE)
    {
      if (fd >= 0)
        ret = do_fchmod (lo, fd, attr->st_mode);
      else
        ret = do_chmod (lo, path, attr->st_mode);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
      node->ino->mode = attr->st_mode;
    }

  if (to_set & FUSE_SET_ATTR_SIZE)
    {
      if (fd >= 0)
        ret = ftruncate (fd, attr->st_size);
      else
        ret = truncate (path, attr->st_size);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  if (uid != -1 || gid != -1)
    {
      if (fd >= 0)
        ret = do_fchown (lo, fd, uid, gid, node->ino->mode);
      else
        ret = do_chown (lo, path, uid, gid, node->ino->mode);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  if (do_getattr (req, &e, node, fd, path) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fuse_reply_attr (req, &e.attr, get_timeout (lo));
}

static int
direct_linkat (struct ovl_layer *l, const char *oldpath, const char *newpath, int flags)
{
  return linkat (l->fd, oldpath, l->fd, newpath, 0);
}

static void
ovl_link (fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node, *newparentnode, *destnode;
  cleanup_free char *path = NULL;
  int ret;
  struct fuse_entry_param e;
  char wd_tmp_file_name[32];

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_link(ino=%" PRIu64 ", newparent=%" PRIu64 ", newname=%s)\n", ino, newparent, newname);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (strlen (newname) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  newparentnode = do_lookup_file (req, lo, newparent, NULL);
  if (newparentnode == NULL || newparentnode->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  destnode = do_lookup_file (req, lo, newparent, newname);
  if (destnode && !destnode->whiteout)
    {
      fuse_reply_err (req, EEXIST);
      return;
    }

  newparentnode = get_node_up (req, lo, newparentnode);
  if (newparentnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (delete_whiteout (lo, -1, newparentnode, newname) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  ret = asprintf (&path, "%s/%s", newparentnode->path, newname);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  ret = direct_linkat (get_upper_layer (lo), node->path, path, 0);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  node = make_ovl_node (lo, path, get_upper_layer (lo), newname, node->tmp_ino, node->tmp_dev, false, newparentnode, lo->fast_ino_check);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }
  if (destnode && !destnode->whiteout)
    node->last_layer = get_upper_layer (lo);

  node = insert_node (newparentnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, -1, NULL, NULL, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = node_to_inode (node);
  node->ino->lookups++;
  e.attr_timeout = get_timeout (lo);
  e.entry_timeout = get_timeout (lo);
  fuse_reply_entry (req, &e);
}

static int
direct_symlinkat (struct ovl_layer *l, const char *target, const char *linkpath, uid_t uid, gid_t gid)
{
  struct ovl_data *lo = l->ovl_data;
  char wd_tmp_file_name[32];
  int ret;

  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());

  unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
  ret = symlinkat (linkpath, lo->workdir_fd, wd_tmp_file_name);
  if (ret < 0)
    return ret;

  if (uid != lo->uid || gid != lo->gid || l->stat_override_mode != STAT_OVERRIDE_NONE)
    {
      ret = do_fchownat (lo, lo->workdir_fd, wd_tmp_file_name, uid, gid, 0755, AT_SYMLINK_NOFOLLOW);
      if (ret < 0)
        {
          unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
          return ret;
        }
    }

  ret = renameat (lo->workdir_fd, wd_tmp_file_name, get_upper_layer (lo)->fd, target);
  if (ret < 0)
    {
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      return ret;
    }

  return 0;
}

static void
ovl_symlink (fuse_req_t req, const char *link, fuse_ino_t parent, const char *name)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode, *node;
  int ret;
  struct fuse_entry_param e;
  const struct fuse_ctx *ctx = fuse_req_ctx (req);
  char wd_tmp_file_name[32];
  bool need_delete_whiteout = true;
  cleanup_free char *path = NULL;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_symlink(link=%s, ino=%" PRIu64 ", name=%s)\n", link, parent, name);

  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (strlen (name) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }

  pnode = do_lookup_file (req, lo, parent, NULL);
  if (pnode == NULL || pnode->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  node = do_lookup_file (req, lo, parent, name);
  if (node != NULL && !node->whiteout)
    {
      fuse_reply_err (req, EEXIST);
      return;
    }

  if (pnode->loaded && node == NULL)
    need_delete_whiteout = false;

  ret = asprintf (&path, "%s/%s", pnode->path, name);
  if (ret < 0)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  ret = direct_symlinkat (get_upper_layer (lo), path, link, get_uid (lo, ctx->uid), get_gid (lo, ctx->gid));
  if (ret < 0)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  if (need_delete_whiteout && delete_whiteout (lo, -1, pnode, name) < 0)
    {
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      fuse_reply_err (req, errno);
      return;
    }

  node = make_ovl_node (lo, path, get_upper_layer (lo), name, 0, 0, false, pnode, lo->fast_ino_check);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, -1, NULL, NULL, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = node_to_inode (node);
  node->ino->lookups++;
  e.attr_timeout = get_timeout (lo);
  e.entry_timeout = get_timeout (lo);
  fuse_reply_entry (req, &e);
}

static void
ovl_rename_exchange (fuse_req_t req, fuse_ino_t parent, const char *name,
                     fuse_ino_t newparent, const char *newname,
                     unsigned int flags)
{
  struct ovl_node *pnode, *node, *destnode, *destpnode;
  struct ovl_data *lo = ovl_data (req);
  int ret;
  cleanup_close int srcfd = -1;
  cleanup_close int destfd = -1;
  struct ovl_node *rm1, *rm2;
  char *tmp;

  node = do_lookup_file (req, lo, parent, name);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (node_dirp (node))
    {
      node = reload_dir (lo, node);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      if (node->layer != get_upper_layer (lo) || node->last_layer != get_upper_layer (lo))
        {
          fuse_reply_err (req, EXDEV);
          return;
        }
    }
  pnode = node->parent;

  destpnode = do_lookup_file (req, lo, newparent, NULL);
  destnode = NULL;

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (safe_openat (node_dirfd (pnode), pnode->path, O_DIRECTORY, 0));
  if (ret < 0)
    goto error;
  srcfd = ret;

  destpnode = get_node_up (req, lo, destpnode);
  if (destpnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (safe_openat (node_dirfd (destpnode), destpnode->path, O_DIRECTORY, 0));
  if (ret < 0)
    goto error;
  destfd = ret;

  destnode = do_lookup_file (req, lo, newparent, newname);

  node = get_node_up (req, lo, node);
  if (node == NULL)
    goto error;

  if (destnode == NULL)
    {
      errno = ENOENT;
      goto error;
    }
  if (node_dirp (node) && destnode->last_layer != get_upper_layer (lo))
    {
      fuse_reply_err (req, EXDEV);
      return;
    }
  destnode = get_node_up (req, lo, destnode);
  if (destnode == NULL)
    goto error;


  ret = direct_renameat2 (srcfd, name, destfd, newname, flags);
  if (ret < 0)
    goto error;

  rm1 = hash_delete (destpnode->children, destnode);
  rm2 = hash_delete (pnode->children, node);

  tmp = node->path;
  node->path = destnode->path;
  destnode->path = tmp;

  tmp = node->name;
  node_set_name (node, destnode->name);
  node_set_name (destnode, tmp);

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    {
      node_free (rm1);
      node_free (rm2);
      goto error;
    }
  destnode = insert_node (pnode, destnode, true);
  if (destnode == NULL)
    {
      node_free (rm1);
      node_free (rm2);
      goto error;
    }
  if ((update_paths (node) < 0) || (update_paths (destnode) < 0))
    goto error;

  if (delete_whiteout (lo, destfd, NULL, newname) < 0)
    goto error;

  ret = 0;
  goto cleanup;

 error:
  ret = -1;

 cleanup:
  fuse_reply_err (req, ret == 0 ? 0 : errno);
}

static void
ovl_rename_direct (fuse_req_t req, fuse_ino_t parent, const char *name,
                   fuse_ino_t newparent, const char *newname,
                   unsigned int flags)
{
  struct ovl_node *pnode, *node, *destnode, *destpnode;
  struct ovl_data *lo = ovl_data (req);
  int ret;
  cleanup_close int srcfd = -1;
  cleanup_close int destfd = -1;
  struct ovl_node key;
  bool destnode_is_whiteout = false;

  node = do_lookup_file (req, lo, parent, name);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (node_dirp (node))
    {
      node = reload_dir (lo, node);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }

      if (node->layer != get_upper_layer (lo) || node->last_layer != get_upper_layer (lo))
        {
          fuse_reply_err (req, EXDEV);
          return;
        }
    }
  pnode = node->parent;

  destpnode = do_lookup_file (req, lo, newparent, NULL);
  destnode = NULL;

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (safe_openat (node_dirfd (pnode), pnode->path, O_DIRECTORY, 0));
  if (ret < 0)
    goto error;
  srcfd = ret;

  destpnode = get_node_up (req, lo, destpnode);
  if (destpnode == NULL)
    goto error;

  ret = TEMP_FAILURE_RETRY (safe_openat (node_dirfd (destpnode), destpnode->path, O_DIRECTORY, 0));
  if (ret < 0)
    goto error;
  destfd = ret;

  node_set_name (&key, (char *) newname);
  destnode = hash_lookup (destpnode->children, &key);

  node = get_node_up (req, lo, node);
  if (node == NULL)
    goto error;

  if (flags & RENAME_NOREPLACE && destnode && !destnode->whiteout)
    {
      errno = EEXIST;
      goto error;
    }

  if (destnode)
    {
      size_t destnode_whiteouts = 0;

      if (!destnode->whiteout && destnode->tmp_ino == node->tmp_ino && destnode->tmp_dev == node->tmp_dev)
        goto error;

      destnode_is_whiteout = destnode->whiteout;

      if (!destnode->whiteout && node_dirp (destnode))
        {
          destnode = reload_dir (lo, destnode);
          if (destnode == NULL)
            goto error;

          if (count_dir_entries (destnode, &destnode_whiteouts) > 0)
            {
              errno = ENOTEMPTY;
              goto error;
            }
          if (destnode_whiteouts && empty_dir (get_upper_layer (lo), destnode->path) < 0)
            goto error;
        }

      if (node_dirp (node) && create_missing_whiteouts (lo, node, destnode->path) < 0)
        goto error;

      if (destnode->ino->lookups > 0)
        node_free (destnode);
      else
        {
          node_free (destnode);
          destnode = NULL;
        }

      if (destnode && !destnode_is_whiteout)
        {
          /* If the node is still accessible then be sure we
             can write to it.  Fix it to be done when a write is
             really done, not now.  */
          destnode = get_node_up (req, lo, destnode);
          if (destnode == NULL)
            {
              fuse_reply_err (req, errno);
              return;
            }

          if (hide_node (lo, destnode, true) < 0)
            goto error;
        }
    }

  /* If the destnode is a whiteout, first attempt to EXCHANGE the source and the destination,
   so that with one operation we get both the rename and the whiteout created.  */
  if (destnode_is_whiteout)
    {
      ret = direct_renameat2 (srcfd, name, destfd, newname, flags|RENAME_EXCHANGE);
      if (ret == 0)
        goto done;

      /* If it fails for any reason, fallback to the more articulated method.  */
    }

  /* If the node is a directory we must ensure there is no whiteout at the
     destination, otherwise the renameat2 will fail.  Create a .wh.$NAME style
     whiteout file until the renameat2 is completed.  */
  if (node_dirp (node))
    {
      ret = create_whiteout (lo, destpnode, newname, true, true);
      if (ret < 0)
        goto error;
      unlinkat (destfd, newname, 0);
    }

  /* Try to create the whiteout atomically, if it fails do the
     rename+mknod separately.  */
  if (! can_mknod)
    {
      ret = -1;
      errno = EPERM;
    }
  else
    {
      ret = direct_renameat2 (srcfd, name, destfd,
                              newname, flags|RENAME_WHITEOUT);
    }
      /* If the destination is a whiteout, just overwrite it.  */
  if (ret < 0 && errno == EEXIST)
    ret = direct_renameat2 (srcfd, name, destfd, newname, flags & ~RENAME_NOREPLACE);
  if (ret < 0)
    {
      ret = direct_renameat2 (srcfd, name, destfd,
                              newname, flags);
      if (ret < 0)
        goto error;

      ret = create_whiteout (lo, pnode, name, false, true);
      if (ret < 0)
        goto error;

      pnode->loaded = 0;
    }

  if (delete_whiteout (lo, destfd, NULL, newname) < 0)
    goto error;

 done:
  hash_delete (pnode->children, node);

  free (node->name);
  node_set_name (node, strdup (newname));
  if (node->name == NULL)
    goto error;

  node = insert_node (destpnode, node, true);
  if (node == NULL)
    goto error;
  if (update_paths (node) < 0)
    goto error;

  node->loaded = 0;

  ret = 0;
  fuse_reply_err (req, 0);
  return;

 error:
  ret = -1;
  fuse_reply_err (req, errno);
}

static void
ovl_rename (fuse_req_t req, fuse_ino_t parent, const char *name,
           fuse_ino_t newparent, const char *newname,
           unsigned int flags)
{
  struct ovl_node *p;
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_rename(ino=%" PRIu64 ", name=%s , ino=%" PRIu64 ", name=%s)\n", parent, name, newparent, newname);

  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (strlen (newname) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }

  if (flags & RENAME_EXCHANGE)
    ovl_rename_exchange (req, parent, name, newparent, newname, flags);
  else
    ovl_rename_direct (req, parent, name, newparent, newname, flags);

  /* Make sure the cache is invalidated, if the parent is in the middle of a readdir. */
  p = do_lookup_file (req, lo, parent, NULL);
  if (p && p->in_readdir)
    fuse_lowlevel_notify_inval_inode (lo->se, parent, 0, 0);
  p = do_lookup_file (req, lo, newparent, NULL);
  if (p && p->in_readdir)
    fuse_lowlevel_notify_inval_inode (lo->se, newparent, 0, 0);
}

static void
ovl_statfs (fuse_req_t req, fuse_ino_t ino)
{
  int ret;
  struct statvfs sfs;
  struct ovl_data *lo = ovl_data (req);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  ret = do_statfs (lo, &sfs);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fuse_reply_statfs (req, &sfs);
}

static void
ovl_readlink (fuse_req_t req, fuse_ino_t ino)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  cleanup_free char *buf = NULL;
  struct ovl_node *node;
  size_t current_size;
  int ret = 0;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_readlink(ino=%" PRIu64 ")\n", ino);

  if (!checkAuthority(req, ino)) {
      fuse_reply_err (req, ENOENT);
      return;
  }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  current_size = PATH_MAX + 1;
  buf = malloc (current_size);
  if (buf == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  while (1)
    {
      char *tmp;

      ret = node->layer->ds->readlinkat (node->layer, node->path, buf, current_size - 1);
      if (ret == -1)
        {
          fuse_reply_err (req, errno);
          return;
        }
      if (ret < current_size - 1)
        break;

      current_size *= 2;
      tmp = realloc (buf, current_size);
      if (tmp == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }
      buf = tmp;
    }

  buf[ret] = '\0';
  fuse_reply_readlink (req, buf);
}

static int
hide_all (struct ovl_data *lo, struct ovl_node *node)
{
  struct ovl_node **nodes;
  size_t i, nodes_size;

  node = reload_dir (lo, node);
  if (node == NULL)
    return -1;

  nodes_size = hash_get_n_entries (node->children) + 2;
  nodes = malloc (sizeof (struct ovl_node *) * nodes_size);
  if (nodes == NULL)
    return -1;

  nodes_size = hash_get_entries (node->children, (void **) nodes, nodes_size);
  for (i = 0; i < nodes_size; i++)
    {
      struct ovl_node *it;
      int ret;

      it = nodes[i];
      ret = create_whiteout (lo, node, it->name, false, true);
      node_free (it);

      if (ret < 0)
        {
          free(nodes);
          return ret;
        }
    }

  free (nodes);
  return 0;
}

static void
ovl_mknod (fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *pnode;
  int ret = 0;
  cleanup_free char *path = NULL;
  struct fuse_entry_param e;
  const struct fuse_ctx *ctx = fuse_req_ctx (req);
  char wd_tmp_file_name[32];

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_mknod(ino=%" PRIu64 ", name=%s, mode=%d, rdev=%lu)\n",
	     parent, name, mode, rdev);

  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (strlen (name) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }

  mode = mode & ~ctx->umask;

  if (lo->xattr_permissions)
    mode |= 0755;

  node = do_lookup_file (req, lo, parent, name);
  if (node != NULL && !node->whiteout)
    {
      fuse_reply_err (req, EEXIST);
      return;
    }

  pnode = do_lookup_file (req, lo, parent, NULL);
  if (pnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }
  sprintf (wd_tmp_file_name, "%lu", get_next_wd_counter ());
  ret = mknodat (lo->workdir_fd, wd_tmp_file_name, mode, rdev);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (do_fchownat (lo, lo->workdir_fd, wd_tmp_file_name, get_uid (lo, ctx->uid), get_gid (lo, ctx->gid), mode, 0) < 0)
    {
      fuse_reply_err (req, errno);
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      return;
    }

  ret = asprintf (&path, "%s/%s", pnode->path, name);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      return;
    }

  ret = renameat (lo->workdir_fd, wd_tmp_file_name, get_upper_layer (lo)->fd, path);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      unlinkat (lo->workdir_fd, wd_tmp_file_name, 0);
      return;
    }

  node = make_ovl_node (lo, path, get_upper_layer (lo), name, 0, 0, false, pnode, lo->fast_ino_check);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  if (delete_whiteout (lo, -1, pnode, name) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, -1, NULL, NULL, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      return;
    }

  /* Make sure the cache is invalidated, if the parent is in the middle of a readdir. */
  if (pnode->in_readdir)
    fuse_lowlevel_notify_inval_inode (lo->se, parent, 0, 0);

  e.ino = node_to_inode (node);
  e.attr_timeout = get_timeout (lo);
  e.entry_timeout = get_timeout (lo);
  node->ino->lookups++;
  fuse_reply_entry (req, &e);
}

static void
ovl_mkdir (fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
  const struct fuse_ctx *ctx = fuse_req_ctx (req);
  struct ovl_data *lo = ovl_data (req);
  struct fuse_entry_param e;
  bool parent_upperdir_only;
  struct ovl_node *pnode;
  struct ovl_node *node;
  struct stat st;
  ino_t ino = 0;
  dev_t dev = 0;
  int ret = 0;
  cleanup_free char *path = NULL;
  bool need_delete_whiteout = true;
  cleanup_lock int l = enter_big_lock ();

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_mkdir(ino=%" PRIu64 ", name=%s, mode=%d)\n",
	     parent, name, mode);

  if (!checkAuthority(req, parent)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  if (strlen (name) > get_fs_namemax (lo))
    {
      fuse_reply_err (req, ENAMETOOLONG);
      return;
    }
  if (lo->xattr_permissions)
    mode |= 0755;

  node = do_lookup_file (req, lo, parent, name);
  if (node != NULL && !node->whiteout)
    {
      fuse_reply_err (req, EEXIST);
      return;
    }

  pnode = do_lookup_file (req, lo, parent, NULL);
  if (pnode == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  pnode = get_node_up (req, lo, pnode);
  if (pnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (pnode->loaded && node == NULL)
    need_delete_whiteout = false;

  parent_upperdir_only = pnode->last_layer == get_upper_layer (lo);

  ret = asprintf (&path, "%s/%s", pnode->path, name);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  ret = create_directory (lo, get_upper_layer (lo)->fd, path, NULL, pnode, -1,
                          get_uid (lo, ctx->uid), get_gid (lo, ctx->gid), mode & ~ctx->umask,
                          true, &st);
  if (ret < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  if (need_delete_whiteout && delete_whiteout (lo, -1, pnode, name) < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  /* if the parent is on the upper layer, it doesn't need to lookup the ino in the lower layers.  */
  if (parent_upperdir_only)
    {
      ino = st.st_ino;
      dev = st.st_dev;
    }

  node = make_ovl_node (lo, path, get_upper_layer (lo), name, ino, dev, true, pnode, lo->fast_ino_check);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  node = insert_node (pnode, node, true);
  if (node == NULL)
    {
      fuse_reply_err (req, ENOMEM);
      return;
    }

  if (parent_upperdir_only)
    {
      node->last_layer = pnode->last_layer;
      if (get_timeout (lo) > 0)
        node->loaded = 1;
    }
  else
    {
      ret = hide_all (lo, node);
      if (ret < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  memset (&e, 0, sizeof (e));

  ret = rpl_stat (req, node, -1, NULL, parent_upperdir_only ? &st : NULL, &e.attr);
  if (ret)
    {
      fuse_reply_err (req, errno);
      return;
    }

  e.ino = node_to_inode (node);
  e.attr_timeout = get_timeout (lo);
  e.entry_timeout = get_timeout (lo);
  node->ino->lookups++;
  fuse_reply_entry (req, &e);
}

static int
direct_fsync (struct ovl_layer *l, int fd, const char *path, int datasync)
{
  cleanup_close int cfd = -1;

  if (fd < 0)
    {
      cfd = safe_openat (l->fd, path, O_NOFOLLOW|O_DIRECTORY, 0);
      if (cfd < 0)
          return cfd;
      fd = cfd;
    }

  return datasync ? fdatasync (fd) : fsync (fd);
}

static void
do_fsync (fuse_req_t req, fuse_ino_t ino, int datasync, int fd)
{
  int ret = 0;
  bool do_fsync;
  struct ovl_node *node;
  struct ovl_data *lo = ovl_data (req);
  cleanup_lock int l = 0;

  if (!lo->fsync)
    {
      fuse_reply_err (req, ENOSYS);
      return;
    }

  l = enter_big_lock ();

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  /* Skip fsync for lower layers.  */
  do_fsync = node && node->layer == get_upper_layer (lo);

  if (node->layer == NULL)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  if (! do_fsync)
    {
      fuse_reply_err (req, 0);
      return;
    }

  if (do_fsync)
    ret = direct_fsync (node->layer, fd, node->path, datasync);

  fuse_reply_err (req, ret == 0 ? 0 : errno);
}

static void
ovl_fsync (fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_fsync(ino=%" PRIu64 ", datasync=%d, fi=%p)\n",
             ino, datasync, fi);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  return do_fsync (req, ino, datasync, fi->fh);
}

static void
ovl_fsyncdir (fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi)
{
  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_fsyncdir(ino=%" PRIu64 ", datasync=%d, fi=%p)\n",
             ino, datasync, fi);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  return do_fsync (req, ino, datasync, -1);
}

static int
direct_ioctl (struct ovl_layer *l, int fd, int cmd, unsigned long *r)
{
  return ioctl (fd, cmd, &r);
}

static void
ovl_ioctl (fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
           struct fuse_file_info *fi, unsigned int flags,
           const void *in_buf, size_t in_bufsz, size_t out_bufsz)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  cleanup_close int cleaned_fd = -1;
  struct ovl_node *node;
  int fd = -1;
  unsigned long r;

  if (flags & FUSE_IOCTL_COMPAT)
    {
      fuse_reply_err (req, ENOSYS);
      return;
    }

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_ioctl(ino=%" PRIu64 ", cmd=%d, arg=%p, fi=%p, flags=%d, buf=%p, in_bufsz=%zu, out_bufsz=%zu)\n",
             ino, cmd, arg, fi, flags, in_buf, in_bufsz, out_bufsz);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  switch (cmd)
    {
    case FS_IOC_GETVERSION:
    case FS_IOC_GETFLAGS:
      if (! node_dirp (node))
        fd = fi->fh;
      break;

    case FS_IOC_SETVERSION:
    case FS_IOC_SETFLAGS:
      node = get_node_up (req, lo, node);
      if (node == NULL)
        {
          fuse_reply_err (req, errno);
          return;
        }
      if (in_bufsz >= sizeof (r))
          r = *(unsigned long *) in_buf;
      break;

    default:
        fuse_reply_err (req, ENOSYS);
        return;
    }

  if (fd < 0)
    {
      fd = cleaned_fd = node->layer->ds->openat (node->layer, node->path, O_RDONLY|O_NONBLOCK, 0755);
      if (fd < 0)
        {
          fuse_reply_err (req, errno);
          return;
        }
    }

  l = release_big_lock ();

  if (direct_ioctl (node->layer, fd, cmd, &r) < 0)
    fuse_reply_err (req, errno);
  else
    fuse_reply_ioctl (req, 0, &r, out_bufsz ? sizeof (r) : 0);
}

static int
direct_fallocate (struct ovl_layer *l, int fd, int mode, off_t offset, off_t len)
{
  return fallocate (fd, mode, offset, len);
}

static void
ovl_fallocate (fuse_req_t req, fuse_ino_t ino, int mode, off_t offset, off_t length, struct fuse_file_info *fi)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  cleanup_close int fd = -1;
  struct ovl_node *node;
  int dirfd;
  int ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_fallocate(ino=%" PRIu64 ", mode=%d, offset=%lo, length=%lu, fi=%p)\n",
             ino, mode, offset, length, fi);

  if (!checkAuthority(req, ino)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  node = do_lookup_file (req, lo, ino, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  node = get_node_up (req, lo, node);
  if (node == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  dirfd = node_dirfd (node);
  fd = safe_openat (dirfd, node->path, O_NONBLOCK|O_NOFOLLOW|O_WRONLY, 0);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  l = release_big_lock ();

  ret = direct_fallocate (node->layer, fd, mode, offset, length);
  fuse_reply_err (req, ret < 0 ? errno : 0);
}

#ifdef HAVE_COPY_FILE_RANGE

static ssize_t
direct_copy_file_range (struct ovl_layer *l, int fd_in, off_t *off_in,
                        int fd_out, off_t *off_out,
                        size_t len, unsigned int flags)
{
  return copy_file_range (fd_in, off_in, fd_out, off_out, len, flags);
}

static void
ovl_copy_file_range (fuse_req_t req, fuse_ino_t ino_in, off_t off_in, struct fuse_file_info *fi_in, fuse_ino_t ino_out, off_t off_out, struct fuse_file_info *fi_out, size_t len, int flags)
{
  cleanup_lock int l = enter_big_lock ();
  struct ovl_data *lo = ovl_data (req);
  struct ovl_node *node, *dnode;
  cleanup_close int fd_dest = -1;
  cleanup_close int fd = -1;
  ssize_t ret;

  if (UNLIKELY (ovl_debug (req)))
    fprintf (stderr, "ovl_copy_file_range(ino_in=%" PRIu64 ", off_in=%lo, fi_in=%p, ino_out=%" PRIu64 ", off_out=%lo, fi_out=%p, size=%zu, flags=%d)\n",
             ino_in, off_in, fi_in, ino_out, off_out, fi_out, len, flags);

  if (!checkAuthority(req, ino_in)) {
    fuse_reply_err (req, ENOENT);
    return;
  }

  node = do_lookup_file (req, lo, ino_in, NULL);
  if (node == NULL || node->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  dnode = do_lookup_file (req, lo, ino_out, NULL);
  if (dnode == NULL || dnode->whiteout)
    {
      fuse_reply_err (req, ENOENT);
      return;
    }

  dnode = get_node_up (req, lo, dnode);
  if (dnode == NULL)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fd = node->layer->ds->openat (node->layer, node->path, O_NONBLOCK|O_NOFOLLOW|O_RDONLY, 0755);
  if (fd < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  fd_dest = TEMP_FAILURE_RETRY (safe_openat (node_dirfd (dnode), dnode->path, O_NONBLOCK|O_NOFOLLOW|O_WRONLY, 0));
  if (fd_dest < 0)
    {
      fuse_reply_err (req, errno);
      return;
    }

  l = release_big_lock ();

  ret = direct_copy_file_range (node->layer, fd, &off_in, fd_dest, &off_out, len, flags);
  if (ret < 0)
    fuse_reply_err (req, errno);
  else
    fuse_reply_write (req, ret);
}
#endif

static struct fuse_lowlevel_ops ovl_oper =
  {
   .statfs = ovl_statfs,
   .access = ovl_access,
   .getxattr = ovl_getxattr,
   .removexattr = ovl_removexattr,
   .setxattr = ovl_setxattr,
   .listxattr = ovl_listxattr,
   .init = ovl_init,
   .lookup = ovl_lookup,
   .forget = ovl_forget,
   .forget_multi = ovl_forget_multi,
   .getattr = ovl_getattr,
   .readlink = ovl_readlink,
   .opendir = ovl_opendir,
   .readdir = ovl_readdir,
   .readdirplus = ovl_readdirplus,
   .releasedir = ovl_releasedir,
   .create = ovl_create,
   .open = ovl_open,
   .release = ovl_release,
   .read = ovl_read,
   .write_buf = ovl_write_buf,
   .unlink = ovl_unlink,
   .rmdir = ovl_rmdir,
   .setattr = ovl_setattr,
   .symlink = ovl_symlink,
   .rename = ovl_rename,
   .mkdir = ovl_mkdir,
   .mknod = ovl_mknod,
   .link = ovl_link,
   .fsync = ovl_fsync,
   .fsyncdir = ovl_fsyncdir,
   .ioctl = ovl_ioctl,
   .fallocate = ovl_fallocate,
#if HAVE_COPY_FILE_RANGE && HAVE_FUSE_COPY_FILE_RANGE
   .copy_file_range = ovl_copy_file_range,
#endif
  };

static int
fuse_opt_proc (void *data, const char *arg, int key, struct fuse_args *outargs)
{
  struct ovl_data *ovl_data = data;

  if (strcmp (arg, "-f") == 0)
    return 1;
  if (strcmp (arg, "--help") == 0)
    return 1;
  if (strcmp (arg, "-h") == 0)
    return 1;
  if (strcmp (arg, "--version") == 0)
    return 1;
  if (strcmp (arg, "-V") == 0)
    return 1;
  if ((strcmp (arg, "--debug") == 0) || (strcmp (arg, "-d") == 0) ||
      (strcmp (arg, "debug") == 0))
    {
      ovl_data->debug = 1;
      return 1;
    }

  if (strcmp (arg, "allow_root") == 0)
    return 1;
  if (strcmp (arg, "default_permissions") == 0)
    return 1;
  if (strcmp (arg, "allow_other") == 0)
    return 1;
  if (strcmp (arg, "suid") == 0)
    return 1;
  if (strcmp (arg, "dev") == 0)
    return 1;
  if (strcmp (arg, "nosuid") == 0)
    return 1;
  if (strcmp (arg, "nodev") == 0)
    return 1;
  if (strcmp (arg, "exec") == 0)
    return 1;
  if (strcmp (arg, "noexec") == 0)
    return 1;
  if (strcmp (arg, "atime") == 0)
    return 1;
  if (strcmp (arg, "noatime") == 0)
    return 1;
  if (strcmp (arg, "diratime") == 0)
    return 1;
  if (strcmp (arg, "nodiratime") == 0)
    return 1;
  if (strcmp (arg, "splice_write") == 0)
    return 1;
  if (strcmp (arg, "splice_read") == 0)
    return 1;
  if (strcmp (arg, "splice_move") == 0)
    return 1;
  if (strcmp (arg, "kernel_cache") == 0)
    return 1;
  if (strcmp (arg, "max_write") == 0)
    return 1;
  if (strcmp (arg, "ro") == 0)
    return 1;

  if (key == FUSE_OPT_KEY_NONOPT)
    {
      if (ovl_data->mountpoint)
        free (ovl_data->mountpoint);

      ovl_data->mountpoint = strdup (arg);
      return 0;
    }
  /* Ignore unknown arguments.  */
  if (key == -1)
    return 0;

  return 1;
}

char **
get_new_args (int *argc, char **argv)
{
  int i;
  char **newargv;

  newargv = malloc (sizeof (char *) * (*argc + 2));
  if (newargv == NULL)
    error (EXIT_FAILURE, 0, "error allocating memory");

  newargv[0] = argv[0];
  if (geteuid() == 0)
    newargv[1] = "-odefault_permissions,allow_other,suid,noatime,lazytime";
  else
    newargv[1] = "-odefault_permissions,noatime=1";
  for (i = 1; i < *argc; i++)
    newargv[i + 1] = argv[i];
  (*argc)++;
  return newargv;
}

static void
set_limits ()
{
  struct rlimit l;

  if (getrlimit (RLIMIT_NOFILE, &l) < 0)
    error (EXIT_FAILURE, errno, "cannot read nofile rlimit");

  /* Set the soft limit to the hard limit.  */
  l.rlim_cur = l.rlim_max;

  if (setrlimit (RLIMIT_NOFILE, &l) < 0)
    error (EXIT_FAILURE, errno, "cannot set nofile rlimit");
}

static char *
load_default_plugins ()
{
  DIR *dp = NULL;
  char *plugins = strdup ("");

  dp = opendir (PKGLIBEXECDIR);
  if (dp == NULL)
    return plugins;

  for (;;)
    {
      struct dirent *dent;

      dent = readdir (dp);
      if (dent == NULL)
        break;
      if (dent->d_type != DT_DIR)
        {
          char *new_plugins = NULL;
          if (asprintf (&new_plugins, "%s/%s:%s", PKGLIBEXECDIR, dent->d_name, plugins) < 0)
            {
              free (plugins);
              closedir (dp);
              return NULL;
            }
          free (plugins);
          plugins = new_plugins;
        }
    }
  closedir (dp);

  return plugins;
}

void newAESCipher(   int keyLen)
{
    const EVP_CIPHER *blockCipher = NULL;
    const EVP_CIPHER *streamCipher = NULL;

    if (keyLen <= 0) {
      keyLen = 192;
    }
    
    //keyLen = AESKeyRange.closest(keyLen);
    
    switch (keyLen) {
      case 128:
        blockCipher = EVP_aes_128_cbc();
        streamCipher = EVP_aes_128_cfb();
        break;
    
      case 192:
        blockCipher = EVP_aes_192_cbc();
        streamCipher = EVP_aes_192_cfb();
        break;
    
      case 256:
      default:
        blockCipher = EVP_aes_256_cbc();
        streamCipher = EVP_aes_256_cfb();
        break;
    }

    gSSLCipher.blockCipher = blockCipher;
    gSSLCipher.streamCipher = streamCipher;
    gSSLCipher.keySize = keyLen / 8;
    gSSLCipher.ivLength = EVP_CIPHER_iv_length(blockCipher);
}

void newKey(const char *password, int passwdLength, int keySize, int ivLength) {
    gSSLKey.keySize = keySize;
    gSSLKey.ivLength = ivLength;
    
    gSSLKey.buffer = (unsigned char *)OPENSSL_malloc(gSSLKey.keySize + gSSLKey.ivLength);
    memset(gSSLKey.buffer, 0, (size_t)gSSLKey.keySize + (size_t)gSSLKey.ivLength);

    // most likely fails unless we're running as root, or a user-page-lock
    // kernel patch is applied..
    //mlock(buffer, (size_t)gSSLKey.keySize + (size_t)gSSLKey.ivLength);

    /*
    gSSLKey.block_enc = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(gSSLKey.block_enc);
    gSSLKey.block_dec = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(gSSLKey.block_dec);
    gSSLKey.stream_enc = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(gSSLKey.stream_enc);
    gSSLKey.stream_dec = EVP_CIPHER_CTX_new();
    EVP_CIPHER_CTX_init(gSSLKey.stream_dec);
    %*/
    
    gSSLKey.mac_ctx = HMAC_CTX_new();
    HMAC_CTX_reset(gSSLKey.mac_ctx);

    EVP_BytesToKey(gSSLCipher.blockCipher, EVP_sha1(), NULL, password, passwdLength, 16, gSSLKey.buffer, gSSLKey.buffer+gSSLKey.keySize);

    HMAC_Init_ex(gSSLKey.mac_ctx, gSSLKey.buffer, keySize, EVP_sha1(), NULL);
}

bool init_ovl_pidinfo()
{
    gOvlPid = getpid();
    gManagePid = getppid();

    if (gOvlPid < 0 || gManagePid < 0)
        return false;

    return true;
}

static void *watch_thread(void *arg)
{
    int res = 0;
    int ppid = getppid();

    while(1)
    {
        if (kill(ppid, 0) == -1)
        {
            syslog(LOG_INFO, "watch thread exit\n");
            if (g_fuse_se != NULL) {
                fuse_session_unmount(g_fuse_se);
            }
            error (EXIT_FAILURE, 0, "exit of parent exit");
            _exit(1);
        }

        syslog(LOG_INFO, "watch thread loop ppid = %d, get ppid = %d\n", ppid, getppid());

        sleep(1);
    }
}


static void  parent_exit_watch()
{    
    int err;
    pthread_t tid;

    int ppid = getppid();

    if (ppid == 1)
    {
        error (EXIT_FAILURE, 0, "exit parent is init");
        _exit(1);
    }

    err = pthread_create(&tid, NULL, (void*)watch_thread, NULL);
    if(err)
    {
        error (EXIT_FAILURE, 0, "pthread_create parent watch failed");
        _exit(1);
    }
    pthread_detach(tid);
}

int main (int argc, char *argv[])
{
  unsigned char password[] = "darkforest";
  struct fuse_session *se = NULL;
  struct fuse_cmdline_opts opts;
  char **newargv = get_new_args (&argc, argv);
  struct ovl_data lo = {.debug = 0,
                        .uid_mappings = NULL,
                        .gid_mappings = NULL,
                        .uid_str = NULL,
                        .gid_str = NULL,
                        .root = NULL,
                        .lowerdir = NULL,
                        .redirect_dir = NULL,
                        .mountpoint = NULL,
                        .fsync = 1,
                        .squash_to_uid = -1,
                        .squash_to_gid = -1,
                        .static_nlink = 0,
                        .xattr_permissions = 0,
                        .euid = geteuid (),
                        .timeout = 1000000000.0,
                        .timeout_str = NULL,
                        .writeback = 1,
  };
  struct fuse_loop_config fuse_conf = {
                                       .clone_fd = 1,
                                       .max_idle_threads = 10,
  };
  int ret = -1;
  cleanup_layer struct ovl_layer *layers = NULL;
  struct ovl_layer *tmp_layer = NULL;
  struct fuse_args args = FUSE_ARGS_INIT (argc, newargv);

  fprintf (stderr, "fuse-overlayfs start");
  parent_exit_watch();
  newAESCipher(gKeyLen);
  newKey(password, strlen(password), gSSLCipher.keySize, gSSLCipher.ivLength);
  parse_mergelist();

  memset (&opts, 0, sizeof (opts));
  if (fuse_opt_parse (&args, &lo, ovl_opts, fuse_opt_proc) == -1)
    error (EXIT_FAILURE, 0, "error parsing options");
  if (fuse_parse_cmdline (&args, &opts) != 0)
    error (EXIT_FAILURE, 0, "error parsing cmdline");

  if (opts.mountpoint)
    free (opts.mountpoint);

  read_overflowids ();

  pthread_mutex_init (&lock, PTHREAD_MUTEX_DEFAULT);

  if (opts.show_help)
    {
      printf ("usage: %s [options] <mountpoint>\n\n", argv[0]);
      fuse_cmdline_help ();
      fuse_lowlevel_help ();
      exit (EXIT_SUCCESS);
    }
  else if (opts.show_version)
    {
      printf ("fuse-overlayfs: version %s\n", PACKAGE_VERSION);
      printf ("FUSE library version %s\n", fuse_pkgversion ());
      fuse_lowlevel_version ();
      exit (EXIT_SUCCESS);
    }

  if(!init_ovl_pidinfo())
  {
    error (EXIT_FAILURE, 0, "get fuse-overlayfs pid fail");
  }

  lo.uid = geteuid ();
  lo.gid = getegid ();

  if (lo.redirect_dir && strcmp (lo.redirect_dir, "off"))
    error (EXIT_FAILURE, 0, "fuse-overlayfs only supports redirect_dir=off");

  if (lo.mountpoint == NULL)
    error (EXIT_FAILURE, 0, "no mountpoint specified");

  if (lo.upperdir != NULL)
    {
      cleanup_free char *full_path = NULL;

      full_path = realpath (lo.upperdir, NULL);
      if (full_path == NULL)
        error (EXIT_FAILURE, errno, "cannot retrieve path for %s", lo.upperdir);

      lo.upperdir = strdup (full_path);
      if (lo.upperdir == NULL)
        error (EXIT_FAILURE, errno, "cannot allocate memory");
    }

  set_limits ();
  check_can_mknod (&lo);

  if (lo.debug)
    {
      fprintf (stderr, "uid=%s\n", lo.uid_str ? : "unchanged");
      fprintf (stderr, "gid=%s\n", lo.gid_str ? : "unchanged");
      fprintf (stderr, "upperdir=%s\n", lo.upperdir ? lo.upperdir : "NOT USED");
      fprintf (stderr, "workdir=%s\n", lo.workdir ? lo.workdir : "NOT USED");
      fprintf (stderr, "lowerdir=%s\n", lo.lowerdir);
      fprintf (stderr, "mountpoint=%s\n", lo.mountpoint);
      fprintf (stderr, "plugins=%s\n", lo.plugins ? lo.plugins : "<none>");
      fprintf (stderr, "fsync=%s\n", lo.fsync ? "enabled" : "disabled");
    }

  lo.uid_mappings = lo.uid_str ? read_mappings (lo.uid_str) : NULL;
  lo.gid_mappings = lo.gid_str ? read_mappings (lo.gid_str) : NULL;

  errno = 0;
  if (lo.timeout_str)
    {
      lo.timeout = strtod (lo.timeout_str, NULL);
      if (errno == ERANGE)
        error (EXIT_FAILURE, errno, "cannot convert %s", lo.timeout_str);
    }

  if (lo.plugins == NULL)
    lo.plugins = load_default_plugins ();

  lo.plugins_ctx = load_plugins (lo.plugins);

  layers = read_dirs (&lo, lo.lowerdir, true, NULL);
  if (layers == NULL)
    {
      error (EXIT_FAILURE, errno, "cannot read lower dirs");
    }

  if (lo.upperdir != NULL)
    {
      tmp_layer = read_dirs (&lo, lo.upperdir, false, layers);
      if (tmp_layer == NULL)
        error (EXIT_FAILURE, errno, "cannot read upper dir");
      layers = tmp_layer;
    }

  lo.layers = layers;

  if (lo.upperdir)
    {
      if (lo.xattr_permissions)
        {
          const char *name = NULL;
          char data[64];
          ssize_t s;
          if (lo.xattr_permissions == 1)
            {
              get_upper_layer (&lo)->stat_override_mode = STAT_OVERRIDE_PRIVILEGED;
              name = XATTR_PRIVILEGED_OVERRIDE_STAT;
            }
          else if (lo.xattr_permissions == 2)
            {
              get_upper_layer (&lo)->stat_override_mode = STAT_OVERRIDE_USER;
              name = XATTR_OVERRIDE_STAT;
            }
          else
            error (EXIT_FAILURE, 0, "invalid value for xattr_permissions");

          s = fgetxattr (get_upper_layer (&lo)->fd, name, data, sizeof (data));
          if (s < 0)
            {
              if (errno != ENODATA)
                error (EXIT_FAILURE, errno, "read xattr `%s` from upperdir", name);
              else
                {
                  struct stat st;
                  ret = fstat (get_upper_layer (&lo)->fd, &st);
                  if (ret < 0)
                    error (EXIT_FAILURE, errno, "stat upperdir");

                  ret = write_permission_xattr (&lo, get_upper_layer (&lo)->fd,
                                                lo.upperdir,
                                                st.st_uid, st.st_gid, st.st_mode);
                  if (ret < 0)
                    error (EXIT_FAILURE, errno, "write xattr `%s` to upperdir", name);
                }
            }
        }
    }

  lo.inodes = hash_initialize (2048, NULL, node_inode_hasher, node_inode_compare, inode_free);

  lo.root = load_dir (&lo, NULL, lo.layers, ".", "");
  if (lo.root == NULL)
    error (EXIT_FAILURE, errno, "cannot read upper dir");
  lo.root->ino->lookups = 2;

  if (lo.workdir == NULL && lo.upperdir != NULL)
    error (EXIT_FAILURE, 0, "workdir not specified");

  if (lo.workdir)
    {
      int dfd;
      cleanup_free char *path = NULL;

      path = realpath (lo.workdir, NULL);
      if (path == NULL)
        goto err_out1;
      mkdir (path, 0700);
      path = realloc(path, strlen(path)+strlen("/work")+1);
      if (!path)
        error (EXIT_FAILURE, errno, "allocating workdir path");
      strcat (path, "/work");
      mkdir (path, 0700);
      free (lo.workdir);
      lo.workdir = strdup (path);
      if (lo.workdir == NULL)
        error (EXIT_FAILURE, errno, "allocating workdir path");

      lo.workdir_fd = open (lo.workdir, O_DIRECTORY);
      if (lo.workdir_fd < 0)
        error (EXIT_FAILURE, errno, "cannot open workdir");

      dfd = dup (lo.workdir_fd);
      if (dfd < 0)
        error (EXIT_FAILURE, errno, "dup workdir file descriptor");
      empty_dirfd (dfd);
    }

  umask (0);
  disable_locking = !lo.threaded;

  se = fuse_session_new (&args, &ovl_oper, sizeof (ovl_oper), &lo);
  lo.se = se;
  g_fuse_se = se;
  if (se == NULL)
    {
      error (0, errno, "cannot create FUSE session");
      goto err_out1;
    }
  if (fuse_set_signal_handlers (se) != 0)
    {
      error (0, errno, "cannot set signal handler");
      goto err_out2;
    }

  signal(SIGUSR1, SIGUSR1_handle);
  signal(SIGUSR2, SIGUSR2_handle);

  if (fuse_session_mount (se, lo.mountpoint) != 0)
    {
      error (0, errno, "cannot mount");
      goto err_out3;
    }
  fuse_daemonize (opts.foreground);

  if (lo.threaded)
    ret = fuse_session_loop_mt (se, &fuse_conf);
  else
    ret = fuse_session_loop (se);

  fuse_session_unmount (se);
err_out3:
  fuse_remove_signal_handlers (se);
err_out2:
  fuse_session_destroy (se);
err_out1:

  for (tmp_layer = lo.layers; tmp_layer; tmp_layer = tmp_layer->next)
    tmp_layer->ds->cleanup (tmp_layer);

  node_mark_all_free (lo.root);

  hash_free (lo.inodes);

  plugin_free_all (lo.plugins_ctx);

  free_mapping (lo.uid_mappings);
  free_mapping (lo.gid_mappings);

  close (lo.workdir_fd);

  fuse_opt_free_args (&args);

  OPENSSL_free(gSSLKey.buffer);
  HMAC_CTX_free(gSSLKey.mac_ctx);

  exit (ret ? EXIT_FAILURE : EXIT_SUCCESS);
  return 1;
}
