#include <sys/mount.h>

#include "logging.h"
#include "misc.hpp"
#include "zygisk.hpp"

using namespace std::string_view_literals;

namespace {
    constexpr auto KSU_MODULE_DIR = "/data/adb/ksu/modules"sv;

    void lazy_unmount(const char* mountpoint) {
        if (umount2(mountpoint, MNT_DETACH) != -1) {
            LOGD("Unmounted (%s)", mountpoint);
        } else {
            PLOGE("Unmount (%s)", mountpoint);
        }
    }
}

#define PARSE_OPT(name, flag)   \
    if (opt == (name)) {        \
        flags |= (flag);        \
        return true;            \
    }

void revert_unmount() {
    std::string ksu_loop;
    std::vector<std::string> targets;
    std::list<std::pair<std::string, std::string>> backups;

    // Unmount ksu module dir last
    targets.emplace_back(KSU_MODULE_DIR);
    parse_mnt("/proc/self/mounts", [&](mntent* mentry) {
        if (mentry->mnt_dir == KSU_MODULE_DIR) {
            ksu_loop = mentry->mnt_fsname;
            return;
        }
        // Unmount everything on /data/adb except ksu module dir
        if (str_starts(mentry->mnt_dir, "/data/adb")) {
            targets.emplace_back(mentry->mnt_dir);
        }
        // Unmount ksu overlays
        if (mentry->mnt_type == "overlay"sv) {
            if (str_contains(mentry->mnt_opts, KSU_MODULE_DIR)) {
                targets.emplace_back(mentry->mnt_dir);
            } else {
                backups.emplace_back(mentry->mnt_dir, mentry->mnt_opts);
            }
        }
    });
    // Unmount everything from ksu loop except ksu module dir
    parse_mnt("/proc/self/mounts", [&](mntent* mentry) {
        if (mentry->mnt_fsname == ksu_loop && mentry->mnt_dir != KSU_MODULE_DIR) {
            targets.emplace_back(mentry->mnt_dir);
        }
    });

    // Do unmount
    for (auto& s: reversed(targets)) {
        lazy_unmount(s.data());
    }

    // Affirm unmounted system overlays
    parse_mnt("/proc/self/mounts", [&](mntent* mentry) {
        if (mentry->mnt_type == "overlay"sv) {
            backups.remove_if([&](auto& mnt) {
                return mnt.first == mentry->mnt_dir && mnt.second == mentry->mnt_opts;
            });
        }
        return true;
    });

    // Restore system overlays
    for (auto& mnt: backups) {
        auto opts = split_str(mnt.second, ",");
        unsigned long flags = 0;
        opts.remove_if([&](auto& opt) {
            PARSE_OPT(MNTOPT_RO, MS_RDONLY)
            PARSE_OPT(MNTOPT_NOSUID, MS_NOSUID)
            PARSE_OPT("relatime", MS_RELATIME)
            return false;
        });
        auto mnt_data = join_str(opts, ",");
        if (mount("overlay", mnt.first.data(), "overlay", flags, mnt_data.data()) != -1) {
            LOGD("Remounted (%s)", mnt.first.data());
        } else {
            PLOGE("Remount (%s, %s)", mnt.first.data(), mnt_data.data());
        }
    }
}