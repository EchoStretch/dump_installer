#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/_iovec.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <ps5/kernel.h>
#include <sys/mdioctl.h>
#include <time.h>
#include <stdbool.h>
#include <sys/ioctl.h>

#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))
#define MAX_PATH PATH_MAX

typedef struct notify_request {
    char unused[45];
    char message[3075];
} notify_request_t;

extern "C" {
    int sceAppInstUtilInitialize(void);
    int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
    int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
}

// NOTIFY
static void notify(const char* fmt, ...) {
    notify_request_t req = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message) - 1, fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// MOUNT HELPERS
static int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

static int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    return nmount(iov, IOVEC_SIZE(iov), 0);
}

static int is_mounted(const char* path) {
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) return 0;
    return strcmp(sfs.f_fstypename, "nullfs") == 0;
}

// COPY HELPERS
static int copy_file(const char* src, const char* dst) {
    FILE* fs = fopen(src, "rb"); if (!fs) return -1;
    FILE* fd = fopen(dst, "wb"); if (!fd) { fclose(fs); return -1; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) fwrite(buf, 1, n, fd);
    fclose(fs); fclose(fd);
    return 0;
}

static int copy_dir(const char* src, const char* dst) {
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;
    DIR* d = opendir(src); if (!d) return -1;
    struct dirent* e; char ss[MAX_PATH], dd[MAX_PATH]; struct stat st;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
        if (stat(ss, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) copy_dir(ss, dd);
        else copy_file(ss, dd);
    }
    closedir(d);
    return 0;
}

static int is_appmeta_file(const char* name) {
    if (!strcasecmp(name, "param.json") || !strcasecmp(name, "param.sfo")) return 1;
    const char* ext = strrchr(name, '.');
    if (!ext) return 0;
    return !strcasecmp(ext, ".png") || !strcasecmp(ext, ".dds") || !strcasecmp(ext, ".at9");
}

static int copy_sce_sys_to_appmeta(const char* src, const char* title_id) {
    char dst[MAX_PATH];
    snprintf(dst, sizeof(dst), "/user/appmeta/%s", title_id);
    mkdir("/user/appmeta", 0777);
    mkdir(dst, 0755);
    DIR* d = opendir(src); if (!d) return -1;
    struct dirent* e; char ss[MAX_PATH], dd[MAX_PATH]; struct stat st;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!is_appmeta_file(e->d_name)) continue;
        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);
        if (stat(ss, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        copy_file(ss, dd);
    }
    closedir(d);
    return 0;
}

// UPDATE SND0
static int update_snd0info(const char* title_id) {
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    int ret = -1;
    char db_path[] = "/system_data/priv/mms/app.db";
    const char* sql = "UPDATE tbl_contentinfo SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' WHERE titleId = ?1;";
    if (sqlite3_open(db_path, &db) != SQLITE_OK) goto out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto out;
    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {}
    ret = sqlite3_changes(db);
out:
    if (stmt) sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return ret;
}

// JSON & SFO
static int extract_json_string(const char* json, const char* key, char* out, size_t out_size) {
    char search[64]; snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search); if (!p) return -1;
    p = strchr(p + strlen(search), ':'); if (!p) return -1;
    while (*++p && isspace(*p));
    if (*p != '"') return -1; p++;
    size_t i=0;
    while (i < out_size-1 && p[i] && p[i] != '"') { out[i] = p[i]; i++; }
    out[i] = '\0';
    return 0;
}

typedef struct {
    uint16_t key_offset;
    uint16_t type;
    uint32_t size;
    uint32_t max_size;
    uint32_t data_offset;
} sfo_entry_t;

static int read_title_id_from_sfo(const char* path, char* title_id, size_t size) {
    FILE* f = fopen(path, "rb"); if (!f) return -1;
    uint32_t magic, version, key_off, data_off, count;
    if (fread(&magic,4,1,f)!=1 || fread(&version,4,1,f)!=1 ||
        fread(&key_off,4,1,f)!=1 || fread(&data_off,4,1,f)!=1 ||
        fread(&count,4,1,f)!=1) { fclose(f); return -1; }
    if (magic != 0x46535000) { fclose(f); return -1; }
    for (uint32_t i=0; i<count; i++) {
        sfo_entry_t entry;
        if (fseek(f, 0x14 + i*sizeof(sfo_entry_t), SEEK_SET)!=0) continue;
        if (fread(&entry, sizeof(sfo_entry_t),1,f)!=1) continue;
        char key[128]={0};
        if (fseek(f, key_off + entry.key_offset, SEEK_SET)!=0) continue;
        if (fread(key,1,sizeof(key)-1,f)<=0) continue;
        for (int k=0; k<128; k++) if (!isprint(key[k])) { key[k]='\0'; break; }
        if (strncmp(key,"TITLE_ID",8)==0) {
            if (fseek(f, data_off + entry.data_offset, SEEK_SET)!=0) continue;
            size_t rlen = (entry.size < size-1)?entry.size:size-1;
            if (fread(title_id,1,rlen,f)<=0) continue;
            title_id[rlen]='\0';
            fclose(f); return 0;
        }
    }
    fclose(f);
    return -1;
}

static int get_title_id(const char* base_path, char* title_id, size_t size) {
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
        if (len>0 && len<1024*1024) {
            char* buf = (char*)malloc(len+1);
            if (buf) {
                fread(buf,1,len,f); buf[len]='\0';
                if (extract_json_string(buf,"titleId",title_id,size)==0 ||
                    extract_json_string(buf,"title_id",title_id,size)==0) {
                    free(buf); fclose(f); return 0;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", base_path);
    return read_title_id_from_sfo(path, title_id, size);
}

// FFPKG HEADER PARSER
// Trailer layout from EOF:
//   ffpkg(5) | version(2) | TITLE_ID(9) | file_count(4) | [entries...] | img_data
//
// Each entry from EOF:
//   path_len(2) | path+null(path_len) | file_size(8) | file_data(file_size)

static int read_title_id_from_ffpkg(const char* file_path, char* title_id, size_t size) {
    FILE* f = fopen(file_path, "rb");
    if (!f) { notify("Failed to open image: %s", strerror(errno)); return -1; }

    // Verify magic at last 5 bytes
    char magic[5];
    if (fseek(f, -5, SEEK_END) != 0 || fread(magic, 1, 5, f) != 5) {
        notify("Failed to read magic bytes");
        fclose(f); return -1;
    }
    if (memcmp(magic, "ffpkg", 5) != 0) {
        notify("Invalid magic - not an ffpkg file");
        fclose(f); return -1;
    }

    // TITLE_ID is at -(5 magic + 2 version + 9 title) = -16 from end
    if (fseek(f, -16, SEEK_END) != 0) {
        notify("Failed to seek to TITLE_ID");
        fclose(f); return -1;
    }
    size_t copy = (9 < size - 1) ? 9 : size - 1;
    if (fread(title_id, 1, copy, f) != copy) {
        notify("Failed to read TITLE_ID");
        fclose(f); return -1;
    }
    title_id[copy] = '\0';

    fclose(f);
    return 0;
}

// Extract all sce_sys files from the ffpkg header into dst_dir
// Returns number of files extracted, or -1 on error
static int extract_sce_sys_from_ffpkg(const char* file_path, const char* dst_dir) {
    FILE* f = fopen(file_path, "rb");
    if (!f) { notify("Failed to open image: %s", strerror(errno)); return -1; }

    // Verify magic
    char magic[5];
    if (fseek(f, -5, SEEK_END) != 0 || fread(magic, 1, 5, f) != 5) {
        notify("Failed to read magic bytes");
        fclose(f); return -1;
    }
    if (memcmp(magic, "ffpkg", 5) != 0) {
        notify("Invalid magic - not an ffpkg file");
        fclose(f); return -1;
    }

    // file_count is at -(5 magic + 2 version + 9 title + 4 file_count) = -20 from end
    uint32_t file_count;
    if (fseek(f, -20, SEEK_END) != 0 || fread(&file_count, 4, 1, f) != 1) {
        notify("Failed to read file count");
        fclose(f); return -1;
    }

    // Cursor starts just before file_count, reading entries backwards
    long cursor = -20;

    for (uint32_t i = 0; i < file_count; i++) {
        // Read path_len (2 bytes)
        uint16_t path_len;
        cursor -= (long)sizeof(path_len);
        if (fseek(f, cursor, SEEK_END) != 0 || fread(&path_len, sizeof(path_len), 1, f) != 1) {
            notify("Failed to read path length (entry %u)", i);
            fclose(f); return -1;
        }

        // Read path + null (path_len bytes)
        cursor -= (long)path_len;
        char path_buf[MAX_PATH] = {};
        if (fseek(f, cursor, SEEK_END) != 0 || fread(path_buf, 1, path_len, f) != (size_t)path_len) {
            notify("Failed to read path (entry %u)", i);
            fclose(f); return -1;
        }
        path_buf[path_len - 1] = '\0'; // ensure null terminated

        // Read file_size (8 bytes)
        uint64_t file_size;
        cursor -= (long)sizeof(file_size);
        if (fseek(f, cursor, SEEK_END) != 0 || fread(&file_size, sizeof(file_size), 1, f) != 1) {
            notify("Failed to read file size for %s", path_buf);
            fclose(f); return -1;
        }

        // Read file data
        cursor -= (long)file_size;
        uint8_t* buf = (uint8_t*)malloc(file_size);
        if (!buf) {
            notify("Out of memory for %s", path_buf);
            fclose(f); return -1;
        }
        if (fseek(f, cursor, SEEK_END) != 0 || fread(buf, 1, file_size, f) != file_size) {
            notify("Failed to read file data for %s", path_buf);
            free(buf); fclose(f); return -1;
        }

        // Build destination path: replace "sce_sys/" prefix with dst_dir
        const char* rel = strchr(path_buf, '/');
        char out_path[MAX_PATH];
        if (rel) snprintf(out_path, sizeof(out_path), "%s%s", dst_dir, rel);
        else     snprintf(out_path, sizeof(out_path), "%s/%s", dst_dir, path_buf);

        // Create parent directories if needed
        char tmp[MAX_PATH]; strncpy(tmp, out_path, sizeof(tmp)-1);
        char* slash = tmp;
        while ((slash = strchr(slash + 1, '/')) != NULL) {
            *slash = '\0';
            mkdir(tmp, 0755);
            *slash = '/';
        }

        // Write file
        FILE* out = fopen(out_path, "wb");
        if (out) {
            fwrite(buf, 1, file_size, out);
            fclose(out);
        } else {
            notify("Failed to write %s: %s", out_path, strerror(errno));
        }
        free(buf);
    }

    fclose(f);
    return (int)file_count;
}

// IMAGE TYPE DETECTION
// Read first 16 bytes of the image file — if all are 0x00 it is a UFS image, otherwise PFS
static bool detect_is_ufs(const char* file_path) {
    FILE* f = fopen(file_path, "rb");
    if (!f) return false;
    uint8_t buf[16] = {};
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 16) return false;
    for (int i = 0; i < 16; i++) {
        if (buf[i] != 0x00) return false;
    }
    return true;
}

static bool is_image_file(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".ffpkg");
}

static int find_image_in_dir(const char* dir, char* out, size_t out_sz, bool* is_ufs_out) {
    DIR* d = opendir(dir); if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (!is_image_file(e->d_name)) continue;
        snprintf(out, out_sz, "%s/%s", dir, e->d_name);
        *is_ufs_out = detect_is_ufs(out);
        closedir(d); return 1;
    }
    closedir(d); return 0;
}

// UFS MOUNT — mount ffpkg image directly to the provided mount_point
static bool mount_ufs_image(const char* file_path, const char* mount_point) {
    struct stat st;
    if (stat(file_path, &st) != 0) {
        notify("stat failed: %s", strerror(errno));
        return false;
    }

    time_t now = time(NULL);
    if (difftime(now, st.st_mtime) < 12.0) {
        notify("Image too new (%.0fs) - skipping", difftime(now, st.st_mtime));
        return false;
    }

    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "ufs") == 0) {
        notify("UFS already mounted at %s", mount_point);
        return true;
    }

    if (mkdir(mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    int mdctl = open("/dev/mdctl", O_RDWR);
    if (mdctl < 0) {
        notify("/dev/mdctl open failed: %s", strerror(errno));
        return false;
    }

    struct md_ioctl mdio = {0};
    mdio.md_version    = MDIOVERSION;
    mdio.md_type       = MD_VNODE;
    mdio.md_file       = (char*)file_path;
    mdio.md_mediasize  = st.st_size;
    mdio.md_sectorsize = 512;
    mdio.md_options    = MD_AUTOUNIT | MD_READONLY;

    int ret = ioctl(mdctl, (unsigned long)MDIOCATTACH, &mdio);
    if (ret != 0) {
        mdio.md_options = MD_AUTOUNIT;
        ret = ioctl(mdctl, (unsigned long)MDIOCATTACH, &mdio);
        if (ret != 0) {
            notify("MDIOCATTACH failed: %s (errno %d)", strerror(errno), errno);
            close(mdctl);
            return false;
        }
    }

    char devname[32];
    snprintf(devname, sizeof(devname), "/dev/md%u", mdio.md_unit);
    close(mdctl);

    notify("UFS attached as %s", devname);

    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("ufs"),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(mount_point),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(devname),
    };
    int iov_count = sizeof(iov) / sizeof(iov[0]);

    // Prefer RW first for install compatibility
    ret = nmount(iov, iov_count, 0);
    if (ret != 0) {
        notify("nmount rw failed: %s - falling back to rdonly", strerror(errno));
        ret = nmount(iov, iov_count, MNT_RDONLY);
        if (ret != 0) {
            notify("nmount ufs failed: %s", strerror(errno));
            return false;
        }
    }

    notify("UFS mounted OK → %s (rw preferred)", mount_point);
    return true;
}

// MAIN
int main(void) {
    char cwd[PATH_MAX];
    char title_id[32] = {};
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char user_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];

    notify("Dump Installer 1.02 Beta - UFS Support");
    printf("Dump Installer 1.02 Beta - UFS Support\n");

    if (!getcwd(cwd, sizeof(cwd))) {
        printf("Error: Unable to determine working directory\n");
        return -1;
    }

    kernel_set_ucred_authid(getpid(), 0x4800000000000010ULL);

    char image_file[MAX_PATH] = {};
    bool is_ufs = false;
    bool has_image = find_image_in_dir(cwd, image_file, sizeof(image_file), &is_ufs);

    if (has_image) {
        notify("ffpkg detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);

        // Always read TITLE_ID from our custom ffpkg header
        if (read_title_id_from_ffpkg(image_file, title_id, sizeof(title_id)) != 0) {
            notify("Failed to read TITLE_ID from ffpkg header");
            return -1;
        }
    } else {
        // Folder mode — derive title_id from sce_sys on disk
        notify("No image found - using folder mode");
        if (get_title_id(cwd, title_id, sizeof(title_id)) != 0) {
            notify("Failed to read Title ID from %s/sce_sys", cwd);
            return -1;
        }
    }

    notify("Installing %s, please wait...", title_id);
    printf("Installing %s, please wait...\n", title_id);

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    mkdir(system_ex_app, 0755);

    if (has_image && is_ufs) {
        // UFS — mount ffpkg image directly to /system_ex/app/<title_id>
        if (!mount_ufs_image(image_file, system_ex_app)) {
            notify("UFS mount failed → %s (errno %d)", system_ex_app, errno);
            printf("Error: Failed to mount application (errno %d)\n", errno);
            return -1;
        }
    } else if (has_image && !is_ufs) {
        notify("Error: Only UFS images are supported");
        printf("Error: Only UFS images are supported\n");
        return -1;
    } else {
        // Folder mode — mount cwd via nullfs to /system_ex/app/<title_id>
        if (is_mounted(system_ex_app)) {
            unmount(system_ex_app, 0);
        }
        if (mount_nullfs(cwd, system_ex_app) != 0) {
            notify("nullfs mount failed → %s (errno %d)", system_ex_app, errno);
            printf("Error: Failed to mount application (errno %d)\n", errno);
            return -1;
        }
        notify("nullfs mounted OK → %s", system_ex_app);
    }

    remount_system_ex();

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    mkdir(user_app_dir, 0755);

    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0755);

    if (has_image) {
        // Extract sce_sys files from the ffpkg header into /user/app/<title_id>/sce_sys
        int extracted = extract_sce_sys_from_ffpkg(image_file, user_sce_sys);
        if (extracted < 0) {
            notify("Failed to extract sce_sys from ffpkg header");
        } else {
            notify("Extracted %d sce_sys files from header", extracted);
            copy_sce_sys_to_appmeta(user_sce_sys, title_id);
        }
    } else {
        // Folder mode — copy sce_sys from cwd
        char src_sce_sys[PATH_MAX];
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
        copy_dir(src_sce_sys, user_sce_sys);
        copy_sce_sys_to_appmeta(src_sce_sys, title_id);
    }

    // Use stronger authid for install
    kernel_set_ucred_authid(getpid(), 0x4801000000000013ULL);

    sceAppInstUtilInitialize();
    int install_ret = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
    if (install_ret != 0) {
        notify("sceAppInstUtilAppInstallTitleDir failed: ret=0x%08x errno=%d (%s)",
               install_ret, errno, strerror(errno));
        printf("Install failed: ret=0x%08x errno=%d (%s)\n",
               install_ret, errno, strerror(errno));
        return -1;
    }

    // Restore original authid
    kernel_set_ucred_authid(getpid(), 0x4800000000000010ULL);

    snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);
    FILE* f = fopen(mount_lnk_path, "w");
    if (f) {
        fprintf(f, "%s", system_ex_app);
        fclose(f);
        notify("mount.lnk created pointing to %s", system_ex_app);
    } else {
        notify("Failed to create mount.lnk");
    }

    notify("Fixing Config (snd0), please wait...");
    printf("Fixing Config, please wait...\n");

    sleep(3);
    update_snd0info(title_id);

    notify("%s installed and ready to use!", title_id);
    printf("%s installed and ready to use!\n", title_id);
    printf("The icon should now appear on the home screen.\n");

    return 0;
}