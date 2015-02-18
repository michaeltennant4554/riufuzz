/*
 *
 * honggfuzz - architecture dependent code (LINUX)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2015 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "common.h"
#include "arch.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/cdefs.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "linux/perf.h"
#include "linux/ptrace.h"
#include "log.h"
#include "util.h"

bool arch_launchChild(honggfuzz_t * hfuzz, char *fileName)
{
    /*
     * Kill a process which corrupts its own heap (with ABRT)
     */
    if (setenv("MALLOC_CHECK_", "3", 1) == -1) {
        LOGMSG_P(l_ERROR, "setenv(MALLOC_CHECK_=3) failed");
        return false;
    }

    /*
     * Tell asan to ignore SEGVs
     */
    if (setenv("ASAN_OPTIONS", "handle_segv=0:abort_on_error=1", 1) == -1) {
        LOGMSG_P(l_ERROR, "setenv(ASAN_OPTIONS) failed");
        return false;
    }

    /*
     * Kill the children when fuzzer dies (e.g. due to Ctrl+C)
     */
    if (prctl(PR_SET_PDEATHSIG, (long)SIGKILL, 0L, 0L, 0L) == -1) {
        LOGMSG_P(l_ERROR, "prctl(PR_SET_PDEATHSIG, SIGKILL) failed");
        return false;
    }

    /*
     * Disable ASLR
     */
    if (personality(ADDR_NO_RANDOMIZE) == -1) {
        LOGMSG_P(l_ERROR, "personality(ADDR_NO_RANDOMIZE) failed");
        return false;
    }
#define ARGS_MAX 512
    char *args[ARGS_MAX + 2];

    int x;

    for (x = 0; x < ARGS_MAX && hfuzz->cmdline[x]; x++) {
        if (!hfuzz->fuzzStdin && strcmp(hfuzz->cmdline[x], _HF_FILE_PLACEHOLDER) == 0) {
            args[x] = fileName;
        } else {
            args[x] = hfuzz->cmdline[x];
        }
    }

    args[x++] = NULL;

    LOGMSG(l_DEBUG, "Launching '%s' on file '%s'", args[0], fileName);

    /*
     * Set timeout (prof), real timeout (2*prof), and rlimit_cpu (2*prof)
     */
    if (hfuzz->tmOut) {
        /*
         * The hfuzz->tmOut is real CPU usage time...
         */
        struct itimerval it_prof = {
            .it_interval = {.tv_sec = hfuzz->tmOut,.tv_usec = 0},
            .it_value = {.tv_sec = 0,.tv_usec = 0}
        };
        if (setitimer(ITIMER_PROF, &it_prof, NULL) == -1) {
            LOGMSG_P(l_ERROR, "Couldn't set the ITIMER_PROF timer");
            return false;
        }

        /*
         * ...so, if a process sleeps, this one should
         * trigger a signal...
         */
        struct itimerval it_real = {
            .it_interval = {.tv_sec = hfuzz->tmOut * 2UL,.tv_usec = 0},
            .it_value = {.tv_sec = 0,.tv_usec = 0}
        };
        if (setitimer(ITIMER_REAL, &it_real, NULL) == -1) {
            LOGMSG_P(l_ERROR, "Couldn't set the ITIMER_REAL timer");
            return false;
        }

        /*
         * ..if a process sleeps and catches SIGPROF/SIGALRM
         * rlimits won't help either
         */
        struct rlimit rl = {
            .rlim_cur = hfuzz->tmOut * 2,
            .rlim_max = hfuzz->tmOut * 2,
        };
        if (setrlimit(RLIMIT_CPU, &rl) == -1) {
            LOGMSG_P(l_ERROR, "Couldn't enforce the RLIMIT_CPU resource limit");
            return false;
        }
    }

    /*
     * The address space limit. If big enough - roughly the size of RAM used
     */
    if (hfuzz->asLimit) {
        struct rlimit rl = {
            .rlim_cur = hfuzz->asLimit * 1024UL * 1024UL,
            .rlim_max = hfuzz->asLimit * 1024UL * 1024UL,
        };
        if (setrlimit(RLIMIT_AS, &rl) == -1) {
            LOGMSG_P(l_DEBUG, "Couldn't encforce the RLIMIT_AS resource limit, ignoring");
        }
    }

    if (hfuzz->nullifyStdio) {
        util_nullifyStdio();
    }

    if (hfuzz->fuzzStdin) {
        /*
         * Uglyyyyyy ;)
         */
        if (!util_redirectStdin(fileName)) {
            return false;
        }
    }

    if (!arch_ptraceEnable(hfuzz)) {
        return false;
    }

    execvp(args[0], args);

    util_recoverStdio();
    LOGMSG(l_FATAL, "Failed to create new '%s' process", args[0]);
    return false;
}

void arch_reapChild(honggfuzz_t * hfuzz, fuzzer_t * fuzzer)
{
    int status;
    bool perfEnabled = false;

    for (;;) {
        pid_t pid;
        while ((pid = wait3(&status, __WNOTHREAD | __WALL | WUNTRACED, NULL)) <= 0) ;

        int perfFd;
        if (perfEnabled == false) {
            if (arch_perfEnable(pid, hfuzz, &perfFd) == false) {
                LOGMSG(l_FATAL, "Couldn't enable perf subsystem for PID: '%d'", pid);
            }
            perfEnabled = true;
        }

        LOGMSG(l_DEBUG, "Process (pid %d) came back with status %d", pid, status);

        if (arch_ptraceAnalyze(hfuzz, status, pid, fuzzer)) {
            arch_perfAnalyze(hfuzz, fuzzer, perfFd);
            return;
        }
    }
}

bool arch_archInit(honggfuzz_t * hfuzz)
{
    return arch_ptracePrepare(hfuzz);
}
