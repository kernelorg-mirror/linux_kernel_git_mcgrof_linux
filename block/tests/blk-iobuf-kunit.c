// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for blk_iobuf_pool order-selection and allocation logic.
 */
#include <kunit/test.h>
#include <linux/blk-iobuf.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/module.h>

/* Helper: build queue_limits with given params */
static void make_lim(struct queue_limits *lim,
		     unsigned int io_min,
		     unsigned int io_opt,
		     unsigned long max_hw_bytes,
		     unsigned int max_segments)
{
	memset(lim, 0, sizeof(*lim));
	lim->io_min         = io_min;
	lim->io_opt         = io_opt;
	lim->max_hw_sectors = max_hw_bytes >> SECTOR_SHIFT;
	lim->max_segments   = max_segments;
}

/* case 1a: io_min=32KiB, expect IO_MIN reason, order 3 */
static void test_order_io_min_32k(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, SZ_32K, 0, SZ_1M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_IO_MIN));
	KUNIT_EXPECT_FALSE(test, !!(reasons & BLK_IOBUF_REASON_SEG_GEOM));
	KUNIT_EXPECT_EQ(test, order, 3u);
}

/* case 1b: io_min=4KiB, max_hw=8MiB, segs=256, expect SEG_GEOM, order 3 */
static void test_order_seg_geom_8m_256(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, PAGE_SIZE, 0, SZ_8M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_SEG_GEOM));
	/* 8 MiB / 256 = 32 KiB -> order 3 */
	KUNIT_EXPECT_EQ(test, order, 3u);
}

/* case 1b larger: max_hw=16MiB, segs=256, expect order 4 */
static void test_order_seg_geom_16m_256(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, PAGE_SIZE, 0, SZ_16M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_SEG_GEOM));
	/* 16 MiB / 256 = 64 KiB -> order 4 */
	KUNIT_EXPECT_EQ(test, order, 4u);
}

/* case 1c: io_opt=128KiB, prefer_io_opt=true, expect IO_OPT, order >=3 */
static void test_order_io_opt_128k(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;
	bool saved = blk_iobuf_pool_prefer_io_opt;

	blk_iobuf_pool_prefer_io_opt = true;
	make_lim(&lim, PAGE_SIZE, SZ_128K, SZ_1M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);
	blk_iobuf_pool_prefer_io_opt = saved;

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_IO_OPT));
	/* 128 KiB = order 5 */
	KUNIT_EXPECT_EQ(test, order, 5u);
}

/* case capped: io_opt=1MiB but max_order=5, expect order 5 not 8 */
static void test_order_capped_at_max(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;
	bool saved = blk_iobuf_pool_prefer_io_opt;

	blk_iobuf_pool_prefer_io_opt = true;
	make_lim(&lim, PAGE_SIZE, SZ_1M, SZ_8M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);
	blk_iobuf_pool_prefer_io_opt = saved;

	KUNIT_EXPECT_LE(test, order, 5u);
}

/* case no pool: io_min=4KiB, io_opt=0, small max_hw/segs -> order 0 */
static void test_order_no_pool(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, PAGE_SIZE, 0, SZ_1M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);

	/* 1 MiB / 256 = 4 KiB = PAGE_SIZE: no higher-order pool needed */
	KUNIT_EXPECT_EQ(test, order, 0u);
	KUNIT_EXPECT_EQ(test, reasons, 0u);
}

/* prefer_io_opt=false should suppress IO_OPT reason */
static void test_order_io_opt_disabled(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;
	bool saved = blk_iobuf_pool_prefer_io_opt;

	blk_iobuf_pool_prefer_io_opt = false;
	make_lim(&lim, PAGE_SIZE, SZ_128K, SZ_1M, 256);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);
	blk_iobuf_pool_prefer_io_opt = saved;

	KUNIT_EXPECT_EQ(test, order, 0u);
	KUNIT_EXPECT_FALSE(test, !!(reasons & BLK_IOBUF_REASON_IO_OPT));
}

/* validation matrix: 32MiB/256 -> 128KiB target, order 5 */
static void test_order_32m_256segs(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, PAGE_SIZE, 0, SZ_32M, 256);
	order = blk_iobuf_choose_order(&lim, 6, &reasons);

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_SEG_GEOM));
	/* 32 MiB / 256 = 128 KiB -> order 5 */
	KUNIT_EXPECT_EQ(test, order, 5u);
}

/* validation: 8MiB/1024 -> 8KiB target (order 1) */
static void test_order_8m_1024segs(struct kunit *test)
{
	struct queue_limits lim;
	unsigned int reasons = 0, order;

	make_lim(&lim, PAGE_SIZE, 0, SZ_8M, 1024);
	order = blk_iobuf_choose_order(&lim, 5, &reasons);

	KUNIT_EXPECT_NE(test, order, 0u);
	KUNIT_EXPECT_TRUE(test, !!(reasons & BLK_IOBUF_REASON_SEG_GEOM));
	/* 8 MiB / 1024 = 8 KiB -> order 1 */
	KUNIT_EXPECT_EQ(test, order, 1u);
}

static struct kunit_case blk_iobuf_test_cases[] = {
	KUNIT_CASE(test_order_io_min_32k),
	KUNIT_CASE(test_order_seg_geom_8m_256),
	KUNIT_CASE(test_order_seg_geom_16m_256),
	KUNIT_CASE(test_order_io_opt_128k),
	KUNIT_CASE(test_order_capped_at_max),
	KUNIT_CASE(test_order_no_pool),
	KUNIT_CASE(test_order_io_opt_disabled),
	KUNIT_CASE(test_order_32m_256segs),
	KUNIT_CASE(test_order_8m_1024segs),
	{}
};

static struct kunit_suite blk_iobuf_test_suite = {
	.name = "blk_iobuf_pool",
	.test_cases = blk_iobuf_test_cases,
};

kunit_test_suite(blk_iobuf_test_suite);

MODULE_DESCRIPTION("KUnit tests for blk_iobuf_pool order selection");
MODULE_LICENSE("GPL");
