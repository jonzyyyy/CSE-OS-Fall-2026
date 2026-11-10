#include "framework.h"

#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Delete a flat temporary directory and the files inside it. */
static void remove_dir(const char *path) {
    DIR *dir = opendir(path);
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char file[1024]; /* dir (<512) + '/' + name (<256) always fits */
            snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
            unlink(file);
        }
        closedir(dir);
    }
    rmdir(path);
}

#define STDERR_LOG ".stderr.log"

/* Print a test's captured stderr (failed CHECKs, sanitizer reports), indented. */
static void print_log(const char *dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, STDERR_LOG);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("            %s", line);
    }
    fclose(f);
}

/*
 * Fork, run one test in a fresh temp directory, and return its wait status.
 * The temp directory is returned in dir so the caller can read the log.
 */
static int run_one(const test_case_t *test, char *dir, size_t dir_size) {
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, dir_size, "%s/worker-test-XXXXXX", tmp ? tmp : "/tmp");
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }

    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        if (chdir(dir) != 0) {
            perror("chdir");
            exit(2);
        }
        if (freopen(STDERR_LOG, "w", stderr) == NULL) {
            exit(2);
        }
        setvbuf(stderr, NULL, _IONBF, 0);
        /*
         * Signal handling is inherited from whatever launched us, and some
         * environments (e.g. GitHub Actions) start processes with SIGPIPE
         * ignored. Reset it so tests see what a server started from a normal
         * terminal would see, wherever they run.
         */
        signal(SIGPIPE, SIG_DFL);
        alarm(TEST_TIMEOUT_SEC);
        test->fn();
        exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

static void describe_failure(int status, char *buf, size_t size) {
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGALRM) {
            snprintf(buf, size, "timed out after %ds (deadlock or missing message?)",
                     TEST_TIMEOUT_SEC);
        } else {
            snprintf(buf, size, "crashed with signal %d (%s)", sig, strsignal(sig));
        }
    } else {
        snprintf(buf, size, "exit status %d", WEXITSTATUS(status));
    }
}

int run_tests(const test_case_t *tests, size_t count, int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    int passed = 0, failed = 0, xfailed = 0, xpassed = 0;

    for (size_t i = 0; i < count; i++) {
        const test_case_t *test = &tests[i];
        if (filter != NULL && strstr(test->name, filter) == NULL) {
            continue;
        }

        char dir[512];
        int status = run_one(test, dir, sizeof(dir));
        int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        char why[256];
        describe_failure(status, why, sizeof(why));

        if (test->xfail_reason == NULL) {
            if (ok) {
                printf("[ PASS  ] %s\n", test->name);
                passed++;
            } else {
                printf("[ FAIL  ] %s: %s\n", test->name, why);
                print_log(dir);
                failed++;
            }
        } else {
            if (ok) {
                printf("[ XPASS ] %s: known bug seems fixed, remove the XFAIL marker\n",
                       test->name);
                xpassed++;
            } else {
                printf("[ XFAIL ] %s: %s\n", test->name, why);
                printf("            known bug: %s\n", test->xfail_reason);
                xfailed++;
            }
        }
        fflush(stdout);
        remove_dir(dir);
    }

    printf("\n%d passed, %d failed, %d known bugs (xfail), %d unexpectedly passed (xpass)\n",
           passed, failed, xfailed, xpassed);
    return (failed > 0 || xpassed > 0) ? 1 : 0;
}
