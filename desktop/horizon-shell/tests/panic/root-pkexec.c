// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for pkexec AFTER the administrator has authenticated, for the
// panic test (run.sh). Installed setuid root by the test, in a private
// directory, and deleted afterwards.
//
// Like pkexec, it runs the program with real, effective and saved user id 0,
// so the user who started it can no longer signal it (kill() fails with
// EPERM). It then plays "shadow-panic release": it waits RELEASE_MS and
// publishes what the release publishes, run/panic.json next to this program
// (released, not engaged, not loaded), and exits 0.
//
// It reads no environment and no file; its only input is argv, which it
// logs to pkexec-root.log next to this program.
#define _GNU_SOURCE
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RELEASE_MS 1500

static char dir[PATH_MAX];

static void path_in(char *out, const char *name)
{
    snprintf(out, PATH_MAX, "%s/%s", dir, name);
}

int main(int argc, char **argv)
{
    if (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0) {
        perror("root-pkexec: not installed setuid root");
        return 99;
    }
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n <= 0)
        return 98;
    dir[n] = '\0';
    char exe[PATH_MAX];
    memcpy(exe, dir, sizeof exe);
    snprintf(dir, sizeof dir, "%s", dirname(exe));

    char path[PATH_MAX];
    path_in(path, "pkexec-root.log");
    FILE *log = fopen(path, "a");
    if (log) {
        fprintf(log, "pkexec");
        for (int i = 1; i < argc; ++i)
            fprintf(log, " %s", argv[i]);
        fprintf(log, " (uid %d, euid %d)\n", (int)getuid(), (int)geteuid());
        fclose(log);
    }
    if (argc == 2 && strcmp(argv[1], "--probe") == 0)
        return 0;

    const struct timespec wait = {RELEASE_MS / 1000, (RELEASE_MS % 1000) * 1000000L};
    nanosleep(&wait, NULL);

    char tmp[PATH_MAX], target[PATH_MAX];
    path_in(tmp, "run/.panic.root.tmp");
    path_in(target, "run/panic.json");
    FILE *out = fopen(tmp, "w");
    if (!out)
        return 3;
    fprintf(out, "{\"version\": 1, \"engaged\": false, \"loaded\": false, \"error\": null, \"since\": null, "
                 "\"released\": \"2026-11-24T00:00:00Z\", \"warnings\": []}\n");
    fclose(out);
    if (rename(tmp, target) != 0)
        return 3;
    path_in(path, "pkexec-root.log");
    log = fopen(path, "a");
    if (log) {
        fprintf(log, "released\n");
        fclose(log);
    }
    return 0;
}
