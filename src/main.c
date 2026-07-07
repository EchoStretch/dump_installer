#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include "types.h"
#include "image.h"
#include "utils.h"
#include "mount_helpers.h"
#include "pfs_mount.h"
#include "pfsc_mount.h"
#include "ufs_mount.h"
#include "exfat_mount.h"
#include "install.h"

#include <ps5/kernel.h>

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void*);
int sceAppInstUtilAppUnInstall(const char*);

#define SHADOWMOUNTPLUS_NOAUTOMOUNT "/data/.kstuff_noautomount"
#define SHADOWMOUNTPLUS_DIR "/data/shadowmount"
#define SHADOWMOUNTPLUS_MANUAL_LIST "/data/shadowmount/manual.lst"

static bool shadowmountplus_enabled(void) {
    struct stat st;
    return access(SHADOWMOUNTPLUS_NOAUTOMOUNT, F_OK) == 0 &&
           stat(SHADOWMOUNTPLUS_DIR, &st) == 0 &&
           S_ISDIR(st.st_mode);
}

static int close_with_errno(FILE* f, int ret) {
    int saved_errno = errno;
    fclose(f);
    errno = saved_errno;
    return ret;
}

static int add_shadowmountplus_link(const char* link_path) {
    FILE* f = fopen(SHADOWMOUNTPLUS_MANUAL_LIST, "a+");
    if (!f) {
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        return close_with_errno(f, -1);
    }

    bool has_content = false;
    bool ends_with_newline = true;
    char line[PATH_MAX + 4];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        has_content = true;
        ends_with_newline = len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r');
        line[strcspn(line, "\r\n")] = '\0';

        if (!strcmp(line, link_path)) {
            fclose(f);
            return 1;
        }
    }

    if (ferror(f) || fseek(f, 0, SEEK_END) != 0) {
        return close_with_errno(f, -1);
    }

    int ret = 0;
    if (has_content && !ends_with_newline && fputc('\n', f) == EOF) {
        ret = -1;
    }
    if (ret == 0 && fprintf(f, "%s\n", link_path) < 0) {
        ret = -1;
    }
    if (fclose(f) != 0) {
        ret = -1;
    }

    return ret;
}

static int install_app(const char* title_id, const char* dir) {
    int (*sceAppInstUtilAppInstallTitleDir)(const char*, const char*, void*) = 0;
    const char* nid = "Wudg3Xe3heE";
    uint32_t handle;
    int ret;

	if (!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) {
		sceAppInstUtilAppInstallTitleDir =
			(int (*)(const char*, const char*, void*))
			(uintptr_t)kernel_dynlib_resolve(-1, handle, nid);
	}

    if (sceAppInstUtilAppInstallTitleDir) {
        ret = sceAppInstUtilAppInstallTitleDir(title_id, dir, 0);
        if (ret == 0) {
            printf("Used AppInstallTitleDir\n");
            return 0;
        }

        printf("AppInstallTitleDir failed: 0x%X\n", ret);
    } else {
        printf("AppInstallTitleDir not available\n");
    }

    // Fallback → required for 12.00+
	printf("Falling back to AppInstallAll...\n");
    ret = sceAppInstUtilAppInstallAll(0);
    if (ret == 0) {
        printf("Used AppInstallAll\n");
        return 0;
    }

    printf("AppInstallAll failed: 0x%X\n", ret);
    return ret;
}

int main(void) {
    char cwd[PATH_MAX];
    char title_id[32] = {};
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char user_sce_sys[PATH_MAX];
    char src_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];
	
    notify("Dump Installer 1.07 Beta - PFS + UFS + exFAT");

    if (!getcwd(cwd, sizeof(cwd))) {
        printf("Error: Unable to determine working directory\n");
        return -1;
    }

    char image_file[MAX_PATH] = {};
    bool is_ufs = false;
    bool is_pfs = false;
    bool is_pfsc = false;
    bool is_exfat = false;
    bool has_image = find_image_in_dir(cwd, image_file, sizeof(image_file),
                                       &is_ufs, &is_pfs, &is_pfsc, &is_exfat);
    char mount_point[MAX_PATH] = {0};
    const char* nullfs_src = cwd;

    if (shadowmountplus_enabled()) {
        const char* link_path = has_image ? image_file : cwd;
        int ret = add_shadowmountplus_link(link_path);
        if (ret == 1) {
            notify("Game already installed in ShadowMountPlus: %s", link_path);
            return 0;
        }
        if (ret != 0) {
            notify("Failed to update ShadowMountPlus: %s", strerror(errno));
            return -1;
        }

        notify("Added to ShadowMountPlus: %s", link_path);
        return 0;
    }

    const char *dirs[] = {
        "/data/imgmnt",
        "/data/imgmnt/exfatmnt",
        "/data/imgmnt/pfsmnt",
        "/data/imgmnt/pfscmnt",
        "/data/imgmnt/ufsmnt"
    };

    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0755) != 0) {
            if (errno != EEXIST) {
                // Try to create parent directory first
                char parent[PATH_MAX];
                strncpy(parent, dirs[i], sizeof(parent) - 1);
                parent[sizeof(parent)-1] = '\0';

                char *last_slash = strrchr(parent, '/');
                if (last_slash && last_slash != parent) {
                    *last_slash = '\0';
                    mkdir(parent, 0755);   // ignore error, we'll try again
                }

                if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) {
                    printf("Failed to create %s (errno %d: %s)\n",
                           dirs[i], errno, strerror(errno));
                    return 1;
                }
            }
        }
    }

    if (has_image) {      
        if (is_ufs) {
            notify("UFS image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (mount_ufs_image(image_file, mount_point)) {
                nullfs_src = mount_point;
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys extracted from UFS mount");
					nullfs_src = mount_point;
                } else {
                    notify("Warning: sce_sys copy from UFS failed");
					unmount_ufs(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("UFS mount failed - falling back to folder mode");
            }
        } else if (is_pfs) {
            notify("PFS image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (strlen(mount_point) > 0) unmount_pfs(mount_point);

            if (mount_pfs_image(image_file, mount_point)) {
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys copied from PFS");
                    nullfs_src = mount_point;
                } else {
                    notify("Failed to copy sce_sys from PFS - fallback to folder mode");
                    unmount_pfs(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("PFS mount failed - falling back to folder mode");
                nullfs_src = cwd;
            }
		} else if (is_pfsc) {
            notify("PFSC image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (strlen(mount_point) > 0) unmount_pfsc(mount_point);

            char pfsc_mount_point[MAX_PATH] = {0};
            if (mount_pfsc_image(image_file, pfsc_mount_point)) {
                // Point nullfs_src to outer mount point initially
                nullfs_src = pfsc_mount_point;
                strncpy(mount_point, pfsc_mount_point, sizeof(mount_point) - 1);

                // --- HARDENED NESTED IMAGE CHECK ---
                char nested_image_file[MAX_PATH] = {0};
                bool nested_ufs = false;
                bool nested_pfs = false;
                bool nested_pfsc = false;
                bool nested_exfat = false;
                
                notify("Scanning inside PFSC mount...");
                bool has_nested = find_image_in_dir(pfsc_mount_point, nested_image_file, sizeof(nested_image_file),
                                                   &nested_ufs, &nested_pfs, &nested_pfsc, &nested_exfat);

                if (has_nested) {
                    // Reset global target filesystem flags so they don't corrupt/mix up
                    is_ufs = false;
                    is_pfs = false;
                    is_exfat = false;
                    is_pfsc = true; // Keep true so parent cleanup triggers

                    char nested_mount_point[MAX_PATH] = {0};
                    bool nested_mount_success = false;

                    if (nested_ufs) {
                        notify("Nested UFS detected");
                        nested_mount_success = mount_ufs_image(nested_image_file, nested_mount_point);
                        if (nested_mount_success) is_ufs = true;
                    } else if (nested_pfs) {
                        notify("Nested PFS detected");
                        nested_mount_success = mount_pfs_image(nested_image_file, nested_mount_point);
                        if (nested_mount_success) is_pfs = true;
                    } else if (nested_exfat) {
                        notify("Nested exFAT detected");
                        nested_mount_success = mount_exfat_image(nested_image_file, nested_mount_point);
                        if (nested_mount_success) is_exfat = true;
                    }

                    if (nested_mount_success) {
                        // Safely redirect to internal filesystem
                        static char static_nested_point[MAX_PATH];
                        strncpy(static_nested_point, nested_mount_point, sizeof(static_nested_point) - 1);
                        nullfs_src = static_nested_point;
                        
                        // Overwrite mount_point so the core engine tracks the active runtime directory
                        strncpy(mount_point, nested_mount_point, sizeof(mount_point) - 1);
                    } else {
                        notify("Nested mount failed - using raw PFSC layout");
                    }
                }
                // --- END NESTED IMAGE CHECK ---

                // Safely allocate clean buffers for parsing UTF-8 structures
                char src_sce_sys_tmp[MAX_PATH] = {0};
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", nullfs_src);
                
                memset(src_sce_sys, 0, sizeof(src_sce_sys));
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    // Sce_sys verified and ready for extraction
                } else {
                    notify("Failed to extract game layout metadata");
                    if (has_nested && strlen(nullfs_src) > 0) {
                        if (nested_ufs) unmount_ufs(nullfs_src);
                        else if (nested_pfs) unmount_pfs(nullfs_src);
                        else if (nested_exfat) unmount_exfat(nullfs_src);
                    }
                    unmount_pfsc(pfsc_mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("PFSC root mounting failed");
                nullfs_src = cwd;
            }
        } else if (is_exfat) {
            notify("ExFat image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (mount_exfat_image(image_file, mount_point)) {
                nullfs_src = mount_point;
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys extracted from ExFat mount");
					nullfs_src = mount_point;
                } else {
                    notify("Warning: sce_sys copy from ExFat failed");
					unmount_exfat(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("ExFat mount failed - falling back to folder mode");
            }
		}
    }

    // Safety check for sce_sys before title ID read
    char sce_sys_path[MAX_PATH];
    snprintf(sce_sys_path, sizeof(sce_sys_path), "%s/sce_sys", nullfs_src);
    if (access(sce_sys_path, F_OK) != 0) {
        notify("CRITICAL: No sce_sys folder at %s - cannot read Title ID", sce_sys_path);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_pfsc) unmount_pfsc(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    if (get_title_id(nullfs_src, title_id, sizeof(title_id))) {
        printf("Error: Could not read Title ID from %s/sce_sys\n", nullfs_src);
        notify("Failed to read Title ID from %s/sce_sys", nullfs_src);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_pfsc) unmount_pfsc(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    mkdir(system_ex_app, 0755);

    if (is_mounted(system_ex_app)) {
        unmount(system_ex_app, 0);
    }

    //notify("Mounting nullfs: %s → %s", nullfs_src, system_ex_app);

    if (mount_nullfs(nullfs_src, system_ex_app)) {
        notify("nullfs mount failed → %s (errno %d)", system_ex_app, errno);
        //printf("Error: Failed to mount application (errno %d)\n", errno);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_pfsc) unmount_pfsc(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    notify("Fixing Config, please wait...");

	sceAppInstUtilInitialize();
	sceAppInstUtilAppUnInstall(title_id);
    update_trophy(title_id, src_sce_sys);
    update_snd0info(title_id);
	
    char test_sfo[MAX_PATH];
    snprintf(test_sfo, sizeof(test_sfo), "%s/sce_sys/param.sfo", system_ex_app);
    /*if (access(test_sfo, F_OK) == 0) {
        notify("param.sfo visible in mounted location: %s", test_sfo);
    } else {
        notify("CRITICAL: param.sfo NOT visible at %s - install may fail", test_sfo);
    }*/

    //notify("nullfs mounted OK → %s overlays %s", system_ex_app, nullfs_src);

    remount_system_ex();

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    mkdir(user_app_dir, 0755);

    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0755);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
    copy_dir(src_sce_sys, user_sce_sys);

    copy_sce_sys_to_appmeta(src_sce_sys, title_id);

    notify("Installing %s, please wait...", title_id);
	
	if (install_app(title_id, "/user/app/")) {
        notify("Application install failed");
        printf("Error: Application install failed\n");
        return -1;
    }
	
    snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);

    FILE* f = fopen(mount_lnk_path, "w");
    if (f) {
        fprintf(f, "%s", cwd);
        fclose(f);
    }

    /*if (strlen(mount_point) > 0) {
        notify("%s kept mounted at %s for launch",
               is_ufs ? "UFS" : (is_exfat ? "exFAT" : "PFS"), mount_point);
    }*/

    if (has_image) {
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
        //notify("Cleaning up: removing %s ...", src_sce_sys);
        if (remove_dir(src_sce_sys) == 0) {
            //notify("sce_sys folder removed successfully");
        } else {
            //notify("Warning: failed to remove sce_sys folder (errno %d)", errno);
        }
    }

    notify("%s installed and ready to use!", title_id);
    //printf("%s installed and ready to use!\n", title_id);
    printf("The icon should now appear on the home screen.\n");

    return 0;
}