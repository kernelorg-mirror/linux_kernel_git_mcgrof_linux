// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test the function and performance of kallsyms
 *
 * Copyright (C) Huawei Technologies Co., Ltd., 2022
 *
 * Authors: Zhen Lei <thunder.leizhen@huawei.com> Huawei
 */

#define pr_fmt(fmt) "kallsyms_selftest: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/random.h>
#include <linux/sched/clock.h>
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <linux/prandom.h>
#include <linux/sort.h>
#include <linux/perf_event.h>
#include <linux/preempt.h>

#include "kallsyms_internal.h"
#include "kallsyms_selftest.h"


#define MAX_NUM_OF_RECORDS		64

struct test_stat {
	int min;
	int max;
	int save_cnt;
	int real_cnt;
	int perf;
	u64 sum;
	char *name;
	unsigned long addr;
	unsigned long addrs[MAX_NUM_OF_RECORDS];
};

struct test_item {
	char *name;
	unsigned long addr;
};

#define ITEM_FUNC(s)				\
	{					\
		.name = #s,			\
		.addr = (unsigned long)s,	\
	}

#define ITEM_DATA(s)				\
	{					\
		.name = #s,			\
		.addr = (unsigned long)&s,	\
	}


static int kallsyms_test_var_bss_static;
static int kallsyms_test_var_data_static = 1;
int kallsyms_test_var_bss;
int kallsyms_test_var_data = 1;

static int kallsyms_test_func_static(void)
{
	kallsyms_test_var_bss_static++;
	kallsyms_test_var_data_static++;

	return 0;
}

int kallsyms_test_func(void)
{
	return kallsyms_test_func_static();
}

__weak int kallsyms_test_func_weak(void)
{
	kallsyms_test_var_bss++;
	kallsyms_test_var_data++;
	return 0;
}

static struct test_item test_items[] = {
	ITEM_FUNC(kallsyms_test_func_static),
	ITEM_FUNC(kallsyms_test_func),
	ITEM_FUNC(kallsyms_test_func_weak),
	ITEM_FUNC(vmalloc_noprof),
	ITEM_FUNC(vfree),
#ifdef CONFIG_KALLSYMS_ALL
	ITEM_DATA(kallsyms_test_var_bss_static),
	ITEM_DATA(kallsyms_test_var_data_static),
	ITEM_DATA(kallsyms_test_var_bss),
	ITEM_DATA(kallsyms_test_var_data),
#endif
};

static char stub_name[KSYM_NAME_LEN];

static int stat_symbol_len(void *data, const char *name, unsigned long addr)
{
	*(u32 *)data += strlen(name);

	return 0;
}

static void test_kallsyms_compression_ratio(void)
{
	u32 pos, off, len, num;
	u32 ratio, total_size, total_len = 0;

	kallsyms_on_each_symbol(stat_symbol_len, &total_len);

	/*
	 * A symbol name cannot start with a number. This stub name helps us
	 * traverse the entire symbol table without finding a match. It's used
	 * for subsequent performance tests, and its length is the average
	 * length of all symbol names.
	 */
	memset(stub_name, '4', sizeof(stub_name));
	pos = total_len / kallsyms_num_syms;
	stub_name[pos] = 0;

	pos = 0;
	num = 0;
	off = 0;
	while (pos < kallsyms_num_syms) {
		len = kallsyms_names[off];
		num++;
		off++;
		pos++;
		if ((len & 0x80) != 0) {
			len = (len & 0x7f) | (kallsyms_names[off] << 7);
			num++;
			off++;
		}
		off += len;
	}

	/*
	 * 1. The length fields is not counted
	 * 2. The memory occupied by array kallsyms_token_table[] and
	 *    kallsyms_token_index[] needs to be counted.
	 */
	total_size = off - num;
	pos = kallsyms_token_index[0xff];
	total_size += pos + strlen(&kallsyms_token_table[pos]) + 1;
	total_size += 0x100 * sizeof(u16);

	pr_info(" ---------------------------------------------------------\n");
	pr_info("| nr_symbols | compressed size | original size | ratio(%%) |\n");
	pr_info("|---------------------------------------------------------|\n");
	ratio = (u32)div_u64(10000ULL * total_size, total_len);
	pr_info("| %10d |    %10d   |   %10d  |  %2d.%-2d   |\n",
		kallsyms_num_syms, total_size, total_len, ratio / 100, ratio % 100);
	pr_info(" ---------------------------------------------------------\n");
}

static int lookup_name(void *data, const char *name, unsigned long addr)
{
	u64 t0, t1, t;
	struct test_stat *stat = (struct test_stat *)data;

	t0 = ktime_get_ns();
	(void)kallsyms_lookup_name(name);
	t1 = ktime_get_ns();

	t = t1 - t0;
	if (t < stat->min)
		stat->min = t;

	if (t > stat->max)
		stat->max = t;

	stat->real_cnt++;
	stat->sum += t;

	return 0;
}

static void test_perf_kallsyms_lookup_name(void)
{
	struct test_stat stat;

	memset(&stat, 0, sizeof(stat));
	stat.min = INT_MAX;
	kallsyms_on_each_symbol(lookup_name, &stat);
	pr_info("kallsyms_lookup_name() looked up %d symbols\n", stat.real_cnt);
	pr_info("The time spent on each symbol is (ns): min=%d, max=%d, avg=%lld\n",
		stat.min, stat.max, div_u64(stat.sum, stat.real_cnt));
}

static int find_symbol(void *data, const char *name, unsigned long addr)
{
	struct test_stat *stat = (struct test_stat *)data;

	if (!strcmp(name, stat->name)) {
		stat->real_cnt++;
		stat->addr = addr;

		if (stat->save_cnt < MAX_NUM_OF_RECORDS) {
			stat->addrs[stat->save_cnt] = addr;
			stat->save_cnt++;
		}

		if (stat->real_cnt == stat->max)
			return 1;
	}

	return 0;
}

static void test_perf_kallsyms_on_each_symbol(void)
{
	u64 t0, t1;
	struct test_stat stat;

	memset(&stat, 0, sizeof(stat));
	stat.max = INT_MAX;
	stat.name = stub_name;
	stat.perf = 1;
	t0 = ktime_get_ns();
	kallsyms_on_each_symbol(find_symbol, &stat);
	t1 = ktime_get_ns();
	pr_info("kallsyms_on_each_symbol() traverse all: %lld ns\n", t1 - t0);
}

static int match_symbol(void *data, unsigned long addr)
{
	struct test_stat *stat = (struct test_stat *)data;

	stat->real_cnt++;
	stat->addr = addr;

	if (stat->save_cnt < MAX_NUM_OF_RECORDS) {
		stat->addrs[stat->save_cnt] = addr;
		stat->save_cnt++;
	}

	if (stat->real_cnt == stat->max)
		return 1;

	return 0;
}

static void test_perf_kallsyms_on_each_match_symbol(void)
{
	u64 t0, t1;
	struct test_stat stat;

	memset(&stat, 0, sizeof(stat));
	stat.max = INT_MAX;
	stat.name = stub_name;
	t0 = ktime_get_ns();
	kallsyms_on_each_match_symbol(match_symbol, stat.name, &stat);
	t1 = ktime_get_ns();
	pr_info("kallsyms_on_each_match_symbol() traverse all: %lld ns\n", t1 - t0);
}

static int test_kallsyms_basic_function(void)
{
	int i, j, ret;
	int next = 0, nr_failed = 0;
	char *prefix;
	unsigned short rand;
	unsigned long addr, lookup_addr;
	char namebuf[KSYM_NAME_LEN];
	struct test_stat *stat, *stat2;

	stat = kmalloc_objs(*stat, 2);
	if (!stat)
		return -ENOMEM;
	stat2 = stat + 1;

	prefix = "kallsyms_lookup_name() for";
	for (i = 0; i < ARRAY_SIZE(test_items); i++) {
		addr = kallsyms_lookup_name(test_items[i].name);
		if (addr != test_items[i].addr) {
			nr_failed++;
			pr_info("%s %s failed: addr=%lx, expect %lx\n",
				prefix, test_items[i].name, addr, test_items[i].addr);
		}
	}

	prefix = "kallsyms_on_each_symbol() for";
	for (i = 0; i < ARRAY_SIZE(test_items); i++) {
		memset(stat, 0, sizeof(*stat));
		stat->max = INT_MAX;
		stat->name = test_items[i].name;
		kallsyms_on_each_symbol(find_symbol, stat);
		if (stat->addr != test_items[i].addr || stat->real_cnt != 1) {
			nr_failed++;
			pr_info("%s %s failed: count=%d, addr=%lx, expect %lx\n",
				prefix, test_items[i].name,
				stat->real_cnt, stat->addr, test_items[i].addr);
		}
	}

	prefix = "kallsyms_on_each_match_symbol() for";
	for (i = 0; i < ARRAY_SIZE(test_items); i++) {
		memset(stat, 0, sizeof(*stat));
		stat->max = INT_MAX;
		stat->name = test_items[i].name;
		kallsyms_on_each_match_symbol(match_symbol, test_items[i].name, stat);
		if (stat->addr != test_items[i].addr || stat->real_cnt != 1) {
			nr_failed++;
			pr_info("%s %s failed: count=%d, addr=%lx, expect %lx\n",
				prefix, test_items[i].name,
				stat->real_cnt, stat->addr, test_items[i].addr);
		}
	}

	if (nr_failed) {
		kfree(stat);
		return -ESRCH;
	}

	for (i = 0; i < kallsyms_num_syms; i++) {
		addr = kallsyms_sym_address(i);
		if (!is_ksym_addr(addr))
			continue;

		ret = lookup_symbol_name(addr, namebuf);
		if (unlikely(ret)) {
			namebuf[0] = 0;
			pr_info("%d: lookup_symbol_name(%lx) failed\n", i, addr);
			goto failed;
		}

		lookup_addr = kallsyms_lookup_name(namebuf);

		memset(stat, 0, sizeof(*stat));
		stat->max = INT_MAX;
		kallsyms_on_each_match_symbol(match_symbol, namebuf, stat);

		/*
		 * kallsyms_on_each_symbol() is too slow, randomly select some
		 * symbols for test.
		 */
		if (i >= next) {
			memset(stat2, 0, sizeof(*stat2));
			stat2->max = INT_MAX;
			stat2->name = namebuf;
			kallsyms_on_each_symbol(find_symbol, stat2);

			/*
			 * kallsyms_on_each_symbol() and kallsyms_on_each_match_symbol()
			 * need to get the same traversal result.
			 */
			if (stat->addr != stat2->addr ||
			    stat->real_cnt != stat2->real_cnt ||
			    memcmp(stat->addrs, stat2->addrs,
				   stat->save_cnt * sizeof(stat->addrs[0]))) {
				pr_info("%s: mismatch between kallsyms_on_each_symbol() and kallsyms_on_each_match_symbol()\n",
					namebuf);
				goto failed;
			}

			/*
			 * The average of random increments is 128, that is, one of
			 * them is tested every 128 symbols.
			 */
			get_random_bytes(&rand, sizeof(rand));
			next = i + (rand & 0xff) + 1;
		}

		/* Need to be found at least once */
		if (!stat->real_cnt) {
			pr_info("%s: Never found\n", namebuf);
			goto failed;
		}

		/*
		 * kallsyms_lookup_name() returns the address of the first
		 * symbol found and cannot be NULL.
		 */
		if (!lookup_addr) {
			pr_info("%s: NULL lookup_addr?!\n", namebuf);
			goto failed;
		}
		if (lookup_addr != stat->addrs[0]) {
			pr_info("%s: lookup_addr != stat->addrs[0]\n", namebuf);
			goto failed;
		}

		/*
		 * If the addresses of all matching symbols are recorded, the
		 * target address needs to be exist.
		 */
		if (stat->real_cnt <= MAX_NUM_OF_RECORDS) {
			for (j = 0; j < stat->save_cnt; j++) {
				if (stat->addrs[j] == addr)
					break;
			}

			if (j == stat->save_cnt) {
				pr_info("%s: j == save_cnt?!\n", namebuf);
				goto failed;
			}
		}
	}

	kfree(stat);

	return 0;

failed:
	pr_info("Test for %dth symbol failed: (%s) addr=%lx", i, namebuf, addr);
	kfree(stat);
	return -ESRCH;
}

#ifdef CONFIG_KALLSYMS_LEARNED_INDEX
/*
 * The predicted-position search must return the position the binary
 * search returns, for every symbol address, the addresses around it,
 * addresses outside the table and with corrupted models. Then time both
 * over the same query streams.
 */
#define LIDX_NQ		(1U << 17)
#define LIDX_REPS	5

struct lidx_stat {
	u64 queries;
	u64 mismatches;
	u64 hits;
	u64 windows;
	u64 fallbacks;
	u64 steps;
};

static struct lidx_bucket *lidx_test_buckets;
static struct lidx_model lidx_test_model;
static unsigned long *lidx_queries;
static unsigned long lidx_sink;

static void lidx_stat_query(struct lidx_stat *st, unsigned long addr)
{
	struct kallsyms_lidx_result r;
	unsigned long pos, ref;

	pos = kallsyms_lidx_test_search(&lidx_test_model, addr, &r);
	ref = kallsyms_addr_pos_bsearch(addr);
	st->queries++;
	st->steps += r.steps;
	if (pos != ref) {
		if (st->mismatches < 4)
			pr_info("lidx MISMATCH addr=%lx got=%lu want=%lu\n",
				addr, pos, ref);
		st->mismatches++;
	}
	switch (r.outcome) {
	case KALLSYMS_LIDX_HIT:
		st->hits++;
		break;
	case KALLSYMS_LIDX_WINDOW:
		st->windows++;
		break;
	default:
		st->fallbacks++;
		break;
	}
}

/* Every symbol, its neighbours, and addresses beyond both table ends. */
static u64 lidx_check_all(const char *what)
{
	struct lidx_stat st = {};
	unsigned long i, a, next, n = kallsyms_num_syms;
	u64 steps100;

	lidx_stat_query(&st, kallsyms_sym_address(0) - 1);
	lidx_stat_query(&st, 0);
	lidx_stat_query(&st, ULONG_MAX);
	for (i = 0; i < n; i++) {
		a = kallsyms_sym_address(i);
		lidx_stat_query(&st, a);
		if (i + 1 < n) {
			next = kallsyms_sym_address(i + 1);
			if (next > a + 1) {
				lidx_stat_query(&st, a + 1);
				lidx_stat_query(&st, next - 1);
				lidx_stat_query(&st, a + (next - a) / 2);
			}
		} else {
			lidx_stat_query(&st, a + 1);
			lidx_stat_query(&st, a + 4096);
		}
	}
	steps100 = div64_u64(st.steps * 100, st.queries);
	pr_info("lidx %s max_err=%u queries=%llu mismatches=%llu hits=%llu windows=%llu fallbacks=%llu reads_per_query=%llu.%02llu\n",
		what, lidx_test_model.max_err, st.queries, st.mismatches,
		st.hits, st.windows, st.fallbacks, steps100 / 100,
		steps100 % 100);
	return st.mismatches;
}

enum lidx_stream { LIDX_KEYS, LIDX_RANDOM, LIDX_RANDKEY, LIDX_HOT, LIDX_NR_STREAMS };
static const char * const lidx_stream_names[LIDX_NR_STREAMS] = {
	"keys", "random", "randkey", "hot",
};

static void lidx_fill_queries(enum lidx_stream stream)
{
	struct rnd_state rnd;
	unsigned long n = kallsyms_num_syms;
	unsigned long base = kallsyms_sym_address(0);
	unsigned long span = kallsyms_sym_address(n - 1) - base;
	unsigned long i;
	u64 r;

	prandom_seed_state(&rnd, 0x6c696478);
	for (i = 0; i < LIDX_NQ; i++) {
		switch (stream) {
		case LIDX_KEYS:
			lidx_queries[i] = kallsyms_sym_address(i % n);
			break;
		case LIDX_RANDOM:
			r = ((u64)prandom_u32_state(&rnd) << 32) | prandom_u32_state(&rnd);
			lidx_queries[i] = base + (span ? r % (span + 1) : 0);
			break;
		case LIDX_RANDKEY:
			lidx_queries[i] = kallsyms_sym_address(prandom_u32_state(&rnd) % n);
			break;
		default:
			lidx_queries[i] = kallsyms_sym_address(n / 2) + 3;
			break;
		}
	}
}

static int lidx_cmp_u64(const void *a, const void *b)
{
	const u64 *x = a, *y = b;

	return *x < *y ? -1 : *x > *y;
}

/* Time the stock binary search and the production entry point. */
static void lidx_time(enum lidx_stream stream, bool learned)
{
	u64 t[LIDX_REPS], t0, ns100;
	unsigned long i, sink = 0;
	int rep;

	for (rep = 0; rep < LIDX_REPS; rep++) {
		t0 = sched_clock();
		if (learned) {
			for (i = 0; i < LIDX_NQ; i++)
				sink += kallsyms_addr_pos(lidx_queries[i]);
		} else {
			for (i = 0; i < LIDX_NQ; i++)
				sink += kallsyms_addr_pos_bsearch(lidx_queries[i]);
		}
		t[rep] = sched_clock() - t0;
		cond_resched();
	}
	lidx_sink += sink;
	sort(t, LIDX_REPS, sizeof(t[0]), lidx_cmp_u64, NULL);
	ns100 = div64_u64(t[LIDX_REPS / 2] * 100, LIDX_NQ);
	pr_info("lidx time search=%s stream=%s queries=%u reps=%d ns_per_query_median=%llu.%02llu ns_per_query_min=%llu.%02llu\n",
		learned ? "kallsyms_addr_pos" : "bsearch",
		lidx_stream_names[stream], LIDX_NQ, LIDX_REPS,
		ns100 / 100, ns100 % 100,
		div64_u64(t[0] * 100, LIDX_NQ) / 100,
		div64_u64(t[0] * 100, LIDX_NQ) % 100);
}

/* Deliberately wrong models must still return the right positions. */
static u64 lidx_check_corrupt(void)
{
	struct lidx_bucket *b = lidx_test_buckets;
	struct rnd_state rnd;
	unsigned int i, mode;
	u64 bad = 0;
	static const char * const modes[] = {
		"slope0", "slopemax", "iceptmax", "iceptmin", "err0", "errmax",
		"garbage",
	};

	prandom_seed_state(&rnd, 0xbadc0de);
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
		if (kallsyms_lidx_build(&lidx_test_model, lidx_test_buckets))
			return 1;
		for (i = 0; i < KALLSYMS_LIDX_BUCKETS; i++) {
			switch (mode) {
			case 0:
				b[i].slope = 0;
				break;
			case 1:
				b[i].slope = U64_MAX;
				break;
			case 2:
				b[i].icept = S32_MAX;
				break;
			case 3:
				b[i].icept = S32_MIN;
				break;
			case 4:
				b[i].err = 0;
				break;
			case 5:
				b[i].err = U32_MAX;
				break;
			default:
				b[i].slope = ((u64)prandom_u32_state(&rnd) << 32) |
					     prandom_u32_state(&rnd);
				b[i].icept = prandom_u32_state(&rnd);
				b[i].err = prandom_u32_state(&rnd) & 0xff;
				break;
			}
		}
		bad += lidx_check_all(modes[mode]);
	}
	return bad;
}

/*
 * NMI context: address lookups run from oops and perf sampling paths, so
 * the search must work there too. Program a cycle counter to overflow on
 * this CPU and look up precomputed addresses from its handler, which runs
 * in NMI context on architectures whose PMU interrupt is an NMI.
 */
#define LIDX_NMI_ADDRS	64

struct lidx_nmi {
	unsigned long addrs[LIDX_NMI_ADDRS];
	unsigned long want[LIDX_NMI_ADDRS];
	atomic_t calls;
	atomic_t nmi;
	atomic_t mismatches;
};

static struct lidx_nmi lidx_nmi;

static void lidx_nmi_handler(struct perf_event *event,
			     struct perf_sample_data *data,
			     struct pt_regs *regs)
{
	unsigned int i = atomic_inc_return(&lidx_nmi.calls) % LIDX_NMI_ADDRS;

	if (in_nmi())
		atomic_inc(&lidx_nmi.nmi);
	if (kallsyms_addr_pos(lidx_nmi.addrs[i]) != lidx_nmi.want[i])
		atomic_inc(&lidx_nmi.mismatches);
}

static u64 lidx_check_nmi(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.config = PERF_COUNT_HW_CPU_CYCLES,
		.size = sizeof(attr),
		.sample_period = 200000,
		.exclude_user = 1,
	};
	struct perf_event *event;
	struct rnd_state rnd;
	unsigned long i, n = kallsyms_num_syms, sink = 0;
	u64 deadline;

	prandom_seed_state(&rnd, 0x6e6d69);
	for (i = 0; i < LIDX_NMI_ADDRS; i++) {
		lidx_nmi.addrs[i] = kallsyms_sym_address(prandom_u32_state(&rnd) % n) + (i & 3);
		lidx_nmi.want[i] = kallsyms_addr_pos_bsearch(lidx_nmi.addrs[i]);
	}
	atomic_set(&lidx_nmi.calls, 0);
	atomic_set(&lidx_nmi.nmi, 0);
	atomic_set(&lidx_nmi.mismatches, 0);

	event = perf_event_create_kernel_counter(&attr, raw_smp_processor_id(),
						 NULL, lidx_nmi_handler, NULL);
	if (IS_ERR(event)) {
		pr_info("lidx nmi skipped: no cycle counter (%ld)\n",
			PTR_ERR(event));
		return 0;
	}
	deadline = sched_clock() + 200 * NSEC_PER_MSEC;
	while (sched_clock() < deadline && atomic_read(&lidx_nmi.calls) < 4096) {
		for (i = 0; i < LIDX_NQ; i++)
			sink += kallsyms_addr_pos(lidx_queries[i]);
	}
	perf_event_release_kernel(event);
	lidx_sink += sink;
	pr_info("lidx nmi handler_calls=%d in_nmi=%d mismatches=%d\n",
		atomic_read(&lidx_nmi.calls), atomic_read(&lidx_nmi.nmi),
		atomic_read(&lidx_nmi.mismatches));
	return atomic_read(&lidx_nmi.mismatches);
}

static int test_kallsyms_learned_index(void)
{
	struct lidx_stat st = {};
	unsigned long i, n = kallsyms_num_syms;
	enum lidx_stream stream;
	u64 bad = 0;
	int ret;

	lidx_test_buckets = vmalloc_array(KALLSYMS_LIDX_BUCKETS + 1,
					  sizeof(*lidx_test_buckets));
	lidx_queries = vmalloc_array(LIDX_NQ, sizeof(*lidx_queries));
	if (!lidx_test_buckets || !lidx_queries) {
		ret = -ENOMEM;
		goto out;
	}

	/* The model published at boot, through the production entry point. */
	for (i = 0; i < n; i++) {
		unsigned long a = kallsyms_sym_address(i);

		st.queries++;
		if (kallsyms_addr_pos(a) != kallsyms_addr_pos_bsearch(a))
			st.mismatches++;
	}
	pr_info("lidx production kallsyms_addr_pos() queries=%llu mismatches=%llu\n",
		st.queries, st.mismatches);
	bad += st.mismatches;

	ret = kallsyms_lidx_build(&lidx_test_model, lidx_test_buckets);
	if (ret) {
		pr_info("lidx build failed: %d\n", ret);
		bad++;
		goto done;
	}
	bad += lidx_check_all("check");
	bad += lidx_check_corrupt();

	for (stream = 0; stream < LIDX_NR_STREAMS; stream++) {
		lidx_fill_queries(stream);
		lidx_time(stream, false);
		lidx_time(stream, true);
	}
	lidx_fill_queries(LIDX_RANDKEY);
	bad += lidx_check_nmi();
done:
	pr_info("lidx result: %s (%llu bad), sink=%lx\n",
		bad ? "FAIL" : "PASS", bad, lidx_sink);
	ret = bad ? -EINVAL : 0;
out:
	vfree(lidx_queries);
	vfree(lidx_test_buckets);
	return ret;
}
#else
static int test_kallsyms_learned_index(void)
{
	return 0;
}
#endif

static int test_entry(void *p)
{
	int ret;

	do {
		schedule_timeout(5 * HZ);
	} while (system_state != SYSTEM_RUNNING);

	pr_info("start\n");
	ret = test_kallsyms_basic_function();
	if (ret) {
		pr_info("abort\n");
		return 0;
	}

	test_kallsyms_compression_ratio();
	test_perf_kallsyms_lookup_name();
	test_perf_kallsyms_on_each_symbol();
	test_perf_kallsyms_on_each_match_symbol();
	ret = test_kallsyms_learned_index();
	if (ret) {
		pr_info("learned index abort\n");
		return 0;
	}
	pr_info("finish\n");

	return 0;
}

static int __init kallsyms_test_init(void)
{
	struct task_struct *t;

	t = kthread_run_on_cpu(test_entry, NULL, 0, "kallsyms_test");
	if (IS_ERR(t)) {
		pr_info("Create kallsyms selftest task failed\n");
		return PTR_ERR(t);
	}

	return 0;
}
late_initcall(kallsyms_test_init);
