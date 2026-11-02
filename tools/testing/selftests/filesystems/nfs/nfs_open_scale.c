// SPDX-License-Identifier: GPL-2.0
/*
 * Measure how NFS open(2)/close(2) scales with threads.
 *
 * Threads repeatedly open and close a set of small files under a
 * directory on an NFS mount and the test reports opens per second at
 * each thread count.  With NFSv4 every open of a regular file looks up
 * the caller's state owner; with a delegation in hand the open completes
 * without an RPC, so the client's own locking sets the rate.  By default
 * all threads share one credential, and so one state owner; -u gives each
 * thread its own uid, and so its own state owner, which isolates the
 * client-wide locks from the per-owner ones.  -r N goes further: each
 * thread cycles through N uids of its own, so state owners keep going
 * idle and being reaped while other threads look theirs up, which
 * stresses owner lifetime rather than measuring speed.
 *
 * The directory comes from -d or NFS_OPEN_SCALE_DIR; without one the test
 * is skipped.  It refuses a non-NFS directory unless -F is given, so the
 * same run can be repeated on a local filesystem for comparison.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "../../kselftest.h"

#ifndef NFS_SUPER_MAGIC
#define NFS_SUPER_MAGIC 0x6969
#endif

#define MAX_THREADS 1024

static char workdir[PATH_MAX - 16];
static int nfiles = 64;
static double seconds = 3.0;
static int per_thread_uid;
static int rotate_uids;
static atomic_int stop;
static atomic_int failed;

struct worker {
	pthread_t thread;
	int index;
	unsigned long opens;
	int error;
};

static void file_path(char *buf, size_t len, int i)
{
	snprintf(buf, len, "%s/f%04d", workdir, i);
}

/*
 * Switch the calling thread, and only it (hence the raw syscalls), to
 * @uid and the matching gid.  The saved uid stays 0, so the thread can
 * regain root and switch again.
 */
static int set_thread_ids(uid_t uid)
{
	if (syscall(SYS_setresuid, -1, 0, -1) ||
	    syscall(SYS_setresgid, uid, uid, uid) ||
	    syscall(SYS_setresuid, uid, uid, 0))
		return errno;
	return 0;
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	char path[PATH_MAX];
	int i = w->index % nfiles;
	int k = 0;

	if (per_thread_uid && !rotate_uids) {
		w->error = set_thread_ids(20000 + w->index);
		if (w->error) {
			atomic_store(&failed, 1);
			return NULL;
		}
	}
	while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
		int fd;

		if (rotate_uids) {
			w->error = set_thread_ids(20000 + w->index * rotate_uids + k);
			if (w->error) {
				atomic_store(&failed, 1);
				return NULL;
			}
			if (++k == rotate_uids)
				k = 0;
		}
		file_path(path, sizeof(path), i);
		fd = open(path, O_RDONLY);
		if (fd < 0) {
			w->error = errno;
			atomic_store(&failed, 1);
			return NULL;
		}
		close(fd);
		w->opens++;
		if (++i == nfiles)
			i = 0;
	}
	return NULL;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Returns opens per second at @nthreads, or a negative errno. */
static double run(int nthreads, struct worker *workers)
{
	unsigned long total = 0;
	double start, elapsed;
	int t;

	atomic_store(&stop, 0);
	start = now();
	for (t = 0; t < nthreads; t++) {
		workers[t] = (struct worker){ .index = t };
		if (pthread_create(&workers[t].thread, NULL, worker_main,
				   &workers[t]))
			ksft_exit_fail_msg("pthread_create: %s\n",
					   strerror(errno));
	}
	usleep((useconds_t)(seconds * 1e6));
	atomic_store(&stop, 1);
	for (t = 0; t < nthreads; t++) {
		pthread_join(workers[t].thread, NULL);
		total += workers[t].opens;
		if (workers[t].error)
			return -workers[t].error;
	}
	elapsed = now() - start;
	return total / elapsed;
}

static int make_files(void)
{
	char path[PATH_MAX], block[4096];
	int i, fd;

	memset(block, 0x5a, sizeof(block));
	if (mkdir(workdir, 0755) && errno != EEXIST)
		return -errno;
	chmod(workdir, 0755);
	for (i = 0; i < nfiles; i++) {
		file_path(path, sizeof(path), i);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return -errno;
		if (write(fd, block, sizeof(block)) != sizeof(block)) {
			close(fd);
			return -EIO;
		}
		fchmod(fd, 0644);
		close(fd);
	}
	return 0;
}

static void remove_files(void)
{
	char path[PATH_MAX];
	int i;

	for (i = 0; i < nfiles; i++) {
		file_path(path, sizeof(path), i);
		unlink(path);
	}
	rmdir(workdir);
}

static int parse_threads(const char *list, int *out, int max)
{
	char *copy = strdup(list), *tok, *save;
	int n = 0;

	for (tok = strtok_r(copy, ",", &save); tok && n < max;
	     tok = strtok_r(NULL, ",", &save)) {
		int v = atoi(tok);

		if (v < 1 || v > MAX_THREADS) {
			free(copy);
			return -1;
		}
		out[n++] = v;
	}
	free(copy);
	return n;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d dir] [-t 1,2,4,8] [-f files] [-s seconds] [-u] [-r N] [-F]\n"
		"  -d  directory on an NFS mount (or NFS_OPEN_SCALE_DIR)\n"
		"  -t  comma-separated thread counts (default 1,2,4,8)\n"
		"  -f  files to cycle through (default 64)\n"
		"  -s  seconds per thread count (default 3)\n"
		"  -u  give each thread its own uid (needs CAP_SETUID)\n"
		"  -r  each thread cycles through N uids of its own (stress)\n"
		"  -F  also run on a directory that is not on NFS\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *dir = getenv("NFS_OPEN_SCALE_DIR");
	const char *tlist = "1,2,4,8";
	int threads[64], nthreads, force = 0, opt, i, ret;
	struct worker *workers;
	struct statfs sfs;
	double base = 0;

	while ((opt = getopt(argc, argv, "d:t:f:s:ur:Fh")) != -1) {
		switch (opt) {
		case 'd':
			dir = optarg;
			break;
		case 't':
			tlist = optarg;
			break;
		case 'f':
			nfiles = atoi(optarg);
			break;
		case 's':
			seconds = atof(optarg);
			break;
		case 'u':
			per_thread_uid = 1;
			break;
		case 'r':
			rotate_uids = atoi(optarg);
			per_thread_uid = rotate_uids > 0;
			break;
		case 'F':
			force = 1;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : KSFT_FAIL;
		}
	}
	nthreads = parse_threads(tlist, threads, 64);

	ksft_print_header();
	if (!dir)
		ksft_exit_skip("no directory: set NFS_OPEN_SCALE_DIR or pass -d\n");
	if (nthreads < 1 || nfiles < 1 || seconds <= 0) {
		usage(argv[0]);
		ksft_exit_fail_msg("bad arguments\n");
	}
	if (statfs(dir, &sfs))
		ksft_exit_fail_msg("statfs %s: %s\n", dir, strerror(errno));
	if (sfs.f_type != NFS_SUPER_MAGIC && !force)
		ksft_exit_skip("%s is not on NFS (use -F to run anyway)\n", dir);
	if (per_thread_uid && geteuid() != 0)
		ksft_exit_skip("-u needs root to change thread credentials\n");

	snprintf(workdir, sizeof(workdir), "%s/nfs_open_scale.%d", dir,
		 getpid());
	ret = make_files();
	if (ret) {
		remove_files();
		ksft_exit_fail_msg("creating files in %s: %s\n", workdir,
				   strerror(-ret));
	}
	workers = calloc(MAX_THREADS, sizeof(*workers));
	if (!workers)
		ksft_exit_fail_msg("out of memory\n");

	ksft_print_msg("dir %s fs 0x%lx files %d seconds %.1f %s\n", dir,
		       (unsigned long)sfs.f_type, nfiles, seconds,
		       rotate_uids ? "rotating uids" :
		       per_thread_uid ? "uid per thread" : "one uid");
	ksft_set_plan(nthreads);
	for (i = 0; i < nthreads; i++) {
		double rate = run(threads[i], workers);

		if (rate < 0) {
			ksft_test_result_fail("threads=%d: %s\n",
					      threads[i], strerror((int)-rate));
			continue;
		}
		if (i == 0)
			base = rate / threads[0];
		ksft_print_msg("threads=%d opens_per_s=%.0f per_thread=%.0f scaling=%.2f\n",
			       threads[i], rate, rate / threads[i],
			       base > 0 ? rate / base : 0);
		ksft_test_result_pass("threads=%d\n", threads[i]);
	}
	remove_files();
	free(workers);
	ksft_finished();
}
