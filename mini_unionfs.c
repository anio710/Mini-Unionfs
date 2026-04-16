#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <limits.h>   /* FIX 3: PATH_MAX */

struct unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct unionfs_state *) fuse_get_context()->private_data)

// ---------------- COPY FILE ----------------
int copy_file(const char *src, const char *dest)
{
    int in = open(src, O_RDONLY);
    if (in == -1) return -errno;

    int out = open(dest, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out == -1) {
        close(in);
        return -errno;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);

    close(in);
    close(out);
    return 0;
}

// ---------------- RESOLVE PATH ----------------
int resolve_path(const char *path, char *resolved)
{
    char upper[PATH_MAX], lower[PATH_MAX], whiteout[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
             UNIONFS_DATA->upper_dir,
             path[0] == '/' ? path + 1 : path);

    if (access(whiteout, F_OK) == 0)
        return -ENOENT;

    if (access(upper, F_OK) == 0) {
        strcpy(resolved, upper);
        return 0;
    }

    if (access(lower, F_OK) == 0) {
        strcpy(resolved, lower);
        return 0;
    }

    return -ENOENT;
}

// ---------------- GETATTR ----------------
static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[PATH_MAX];  /* FIX 3 */

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    if (lstat(resolved, stbuf) == -1)
        return -errno;

    return 0;
}

// ---------------- READDIR ----------------
static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    (void) offset; (void) fi; (void) flags;

    DIR *dp;
    struct dirent *de;

    char upper[PATH_MAX], lower[PATH_MAX];  /* FIX 3 */

    /* FIX 2: dynamic seen list instead of fixed seen[100][256] */
    int capacity = 128;
    int count = 0;
    char **seen = malloc(capacity * sizeof(char *));
    if (!seen) return -ENOMEM;

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    dp = opendir(upper);
    if (dp) {
        while ((de = readdir(dp))) {
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            filler(buf, de->d_name, NULL, 0, 0);

            /* grow if needed */
            if (count == capacity) {
                capacity *= 2;
                char **tmp = realloc(seen, capacity * sizeof(char *));
                if (!tmp) { closedir(dp); goto cleanup; }
                seen = tmp;
            }
            seen[count++] = strdup(de->d_name);
        }
        closedir(dp);
    }

    dp = opendir(lower);
    if (dp) {
        while ((de = readdir(dp))) {
            int found = 0;

            for (int i = 0; i < count; i++)
                if (strcmp(seen[i], de->d_name) == 0) { found = 1; break; }

            /* FIX 1 (readdir whiteout): build whiteout path relative to
               the directory being listed, not always upper_dir root */
            char whiteout[PATH_MAX];
            if (strcmp(path, "/") == 0)
                snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
                         UNIONFS_DATA->upper_dir, de->d_name);
            else
                snprintf(whiteout, sizeof(whiteout), "%s%s/.wh.%s",
                         UNIONFS_DATA->upper_dir, path, de->d_name);

            if (access(whiteout, F_OK) == 0)
                found = 1;

            if (!found)
                filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

cleanup:
    for (int i = 0; i < count; i++) free(seen[i]);
    free(seen);
    return 0;
}

// ---------------- OPEN ----------------
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char upper[PATH_MAX], lower[PATH_MAX], resolved[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    // Handle CoW
    if ((fi->flags & (O_WRONLY | O_RDWR)) &&
        access(upper, F_OK) != 0 &&
        access(lower, F_OK) == 0) {

        if (copy_file(lower, upper) != 0)
            return -errno;
    }

    // Handle create (safe)
    if ((fi->flags & O_CREAT) && access(upper, F_OK) != 0) {
        int fd = open(upper, O_CREAT | O_WRONLY, 0644);
        if (fd == -1) return -errno;
        close(fd);
    }

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, fi->flags);
    if (fd == -1)
        return -errno;

    close(fd);
    return 0;
}

// ---------------- READ ----------------
static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[PATH_MAX];  /* FIX 3 */

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, O_RDONLY);
    if (fd == -1)
        return -errno;

    int res = pread(fd, buf, size, offset);
    close(fd);
    return res;
}

// ---------------- WRITE ----------------
static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    char upper[PATH_MAX], lower[PATH_MAX], resolved[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    if (access(upper, F_OK) != 0 && access(lower, F_OK) == 0)
        copy_file(lower, upper);

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, O_WRONLY);
    if (fd == -1)
        return -errno;

    int res = pwrite(fd, buf, size, offset);
    close(fd);
    return res;
}

// ---------------- CREATE ----------------
static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    char upper[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);

    int fd = open(upper, O_CREAT | O_WRONLY, mode);
    if (fd == -1)
        return -errno;

    close(fd);
    return 0;
}

// ---------------- UNLINK ----------------
static int unionfs_unlink(const char *path)
{
    char upper[PATH_MAX], lower[PATH_MAX], whiteout[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
             UNIONFS_DATA->upper_dir,
             path[0] == '/' ? path + 1 : path);

    if (access(upper, F_OK) == 0)
        return unlink(upper);

    if (access(lower, F_OK) == 0) {
        int fd = open(whiteout, O_CREAT | O_WRONLY, 0644);
        if (fd == -1)
            return -errno;
        close(fd);
        return 0;
    }

    return -ENOENT;
}

// ---------------- MKDIR ----------------
static int unionfs_mkdir(const char *path, mode_t mode)
{
    char upper[PATH_MAX];  /* FIX 3 */
    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);

    if (mkdir(upper, mode) == -1)
        return -errno;

    return 0;
}

// ---------------- RMDIR ----------------
/* FIX 5: mirror unlink behaviour — if directory only exists in lower,
   create a whiteout marker so it is hidden from the merged view */
static int unionfs_rmdir(const char *path)
{
    char upper[PATH_MAX], lower[PATH_MAX], whiteout[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
             UNIONFS_DATA->upper_dir,
             path[0] == '/' ? path + 1 : path);

    if (access(upper, F_OK) == 0)
        return rmdir(upper);

    if (access(lower, F_OK) == 0) {
        int fd = open(whiteout, O_CREAT | O_WRONLY, 0644);
        if (fd == -1)
            return -errno;
        close(fd);
        return 0;
    }

    return -ENOENT;
}

// ---------------- TRUNCATE ----------------
/* FIX 1 (truncate): required by text editors (vim, nano, etc.) that
   truncate a file to 0 before rewriting it */
static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi)
{
    (void) fi;
    char upper[PATH_MAX], lower[PATH_MAX], resolved[PATH_MAX];  /* FIX 3 */

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    /* CoW: if file only exists in lower, copy it up before truncating */
    if (access(upper, F_OK) != 0 && access(lower, F_OK) == 0) {
        if (copy_file(lower, upper) != 0)
            return -errno;
    }

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    if (truncate(resolved, size) == -1)
        return -errno;

    return 0;
}

// ---------------- OPERATIONS ----------------
static struct fuse_operations unionfs_oper = {
    .getattr  = unionfs_getattr,
    .readdir  = unionfs_readdir,
    .open     = unionfs_open,
    .read     = unionfs_read,
    .write    = unionfs_write,
    .create   = unionfs_create,
    .unlink   = unionfs_unlink,
    .mkdir    = unionfs_mkdir,
    .rmdir    = unionfs_rmdir,
    .truncate = unionfs_truncate,   /* FIX 1 */
};

// ---------------- MAIN ----------------
int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <lower> <upper> <mountpoint>\n", argv[0]);
        return 1;
    }

    struct unionfs_state *state = malloc(sizeof(struct unionfs_state));
    if (!state) {
        perror("malloc");
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        perror("realpath");
        return 1;
    }

    char *fuse_argv[3];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = "-f";
    fuse_argv[2] = argv[3];

    return fuse_main(3, fuse_argv, &unionfs_oper, state);
}
