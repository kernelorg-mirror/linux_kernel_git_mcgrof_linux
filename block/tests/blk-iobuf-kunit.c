// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for the bounded block iobuf folio pool.
 *
 * Two groups. The object tests prove strict bounded semantics: exactly N
 * folios of the configured order come out, the N+1th checkout fails with
 * -ENOBUFS and no buddy fallback, batches are all-or-nothing, and folios
 * return to the exact pool. The queue tests prove the attachment lifetime
 * using a bare request_queue -- the attach helpers touch only q->iobuf_pool
 * and q->iobuf_pool_lock, so no block-mq machinery is needed -- including the
 * load-bearing case: replace or detach a queue's pool while a holder still
 * owns folios from it, which under KASAN is a use-after-free check.
 */
#include <kunit/test.h>
#include <linux/blk-iobuf.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/slab.h>

static void iobuf_create_destroy(struct kunit *test)
{
	struct blk_iobuf_pool *p0 = blk_iobuf_pool_create(0, 4);
	struct blk_iobuf_pool *p2 = blk_iobuf_pool_create(2, 8);

	KUNIT_ASSERT_FALSE(test, IS_ERR(p0));
	KUNIT_ASSERT_FALSE(test, IS_ERR(p2));
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_order(p0), 0u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_order(p2), 2u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_folio_size(p2),
			(unsigned int)(PAGE_SIZE << 2));
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_capacity(p2), 8u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_available(p2), 8u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(p2), 0u);
	blk_iobuf_pool_put(p0);
	blk_iobuf_pool_put(p2);
}

static void iobuf_create_rejects(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, PTR_ERR(blk_iobuf_pool_create(0, 0)), -EINVAL);
	KUNIT_EXPECT_EQ(test,
			PTR_ERR(blk_iobuf_pool_create(MAX_PAGE_ORDER + 1, 1)),
			-EINVAL);
	/* order 10 (max) x a huge count blows the per-pool byte cap */
	KUNIT_EXPECT_EQ(test,
			PTR_ERR(blk_iobuf_pool_create(MAX_PAGE_ORDER, U32_MAX)),
			-EINVAL);
}

static void iobuf_strict_capacity(struct kunit *test)
{
	struct blk_iobuf_pool *pool = blk_iobuf_pool_create(0, 4);
	struct folio *f[4], *extra;
	int i, ret;

	KUNIT_ASSERT_FALSE(test, IS_ERR(pool));

	for (i = 0; i < 4; i++) {
		ret = blk_iobuf_pool_alloc_batch(pool, &f[i], 1);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(pool), 4u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_available(pool), 0u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_high_water(pool), 4u);

	/* the pool is exhausted; the next checkout must fail, not grow */
	ret = blk_iobuf_pool_alloc_batch(pool, &extra, 1);
	KUNIT_EXPECT_EQ(test, ret, -ENOBUFS);
	KUNIT_EXPECT_GE(test, blk_iobuf_pool_alloc_failures(pool), 1ull);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(pool), 4u);

	/* return one, one slot opens */
	blk_iobuf_pool_free_batch(pool, &f[0], 1);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_available(pool), 1u);
	ret = blk_iobuf_pool_alloc_batch(pool, &f[0], 1);
	KUNIT_EXPECT_EQ(test, ret, 0);

	blk_iobuf_pool_free_batch(pool, f, 4);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(pool), 0u);
	blk_iobuf_pool_put(pool);
}

static void iobuf_batch_all_or_nothing(struct kunit *test)
{
	struct blk_iobuf_pool *pool = blk_iobuf_pool_create(1, 4);
	struct folio *three[3], *two[2];
	int ret;

	KUNIT_ASSERT_FALSE(test, IS_ERR(pool));

	ret = blk_iobuf_pool_alloc_batch(pool, three, 3);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, folio_order(three[0]), 1u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(pool), 3u);

	/* only 1 left; a 2-folio batch is all-or-nothing -> fails, no partial */
	two[0] = NULL;
	two[1] = NULL;
	ret = blk_iobuf_pool_alloc_batch(pool, two, 2);
	KUNIT_EXPECT_EQ(test, ret, -ENOBUFS);
	KUNIT_EXPECT_NULL(test, two[0]);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(pool), 3u);

	blk_iobuf_pool_free_batch(pool, three, 3);
	blk_iobuf_pool_put(pool);
}

static void iobuf_two_pools_exact(struct kunit *test)
{
	struct blk_iobuf_pool *a = blk_iobuf_pool_create(1, 2);
	struct blk_iobuf_pool *b = blk_iobuf_pool_create(3, 2);
	struct folio *fa, *fb;

	KUNIT_ASSERT_FALSE(test, IS_ERR(a));
	KUNIT_ASSERT_FALSE(test, IS_ERR(b));

	KUNIT_ASSERT_EQ(test, blk_iobuf_pool_alloc_batch(a, &fa, 1), 0);
	KUNIT_ASSERT_EQ(test, blk_iobuf_pool_alloc_batch(b, &fb, 1), 0);
	KUNIT_EXPECT_EQ(test, folio_order(fa), 1u);
	KUNIT_EXPECT_EQ(test, folio_order(fb), 3u);

	blk_iobuf_pool_free_batch(a, &fa, 1);
	blk_iobuf_pool_free_batch(b, &fb, 1);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(a), 0u);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(b), 0u);
	blk_iobuf_pool_put(a);
	blk_iobuf_pool_put(b);
}

/* A bare queue: the attach helpers touch only these two fields. */
static struct request_queue *iobuf_fake_queue(struct kunit *test)
{
	struct request_queue *q = kunit_kzalloc(test, sizeof(*q), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, q);
	mutex_init(&q->iobuf_pool_lock);
	RCU_INIT_POINTER(q->iobuf_pool, NULL);
	return q;
}

static void iobuf_queue_attach_get_clear(struct kunit *test)
{
	struct request_queue *q = iobuf_fake_queue(test);
	struct blk_iobuf_pool *pool = blk_iobuf_pool_create(2, 4);
	struct blk_iobuf_pool *got;

	KUNIT_ASSERT_FALSE(test, IS_ERR(pool));

	KUNIT_EXPECT_NULL(test, blk_queue_get_iobuf_pool(q));

	/* set takes its own ref; drop ours so only the queue holds one */
	blk_queue_set_iobuf_pool(q, pool);
	blk_iobuf_pool_put(pool);

	got = blk_queue_get_iobuf_pool(q);
	KUNIT_ASSERT_PTR_EQ(test, got, pool);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_order(got), 2u);
	blk_iobuf_pool_put(got);

	blk_queue_clear_iobuf_pool(q);
	KUNIT_EXPECT_NULL(test, blk_queue_get_iobuf_pool(q));
}

/*
 * The lifetime crux. Replace a queue's pool while a holder still owns folios
 * from the old one; the old pool and its folios must stay valid, and only the
 * final reference may destroy it. Under KASAN this is a use-after-free check.
 */
static void iobuf_queue_replace_outstanding(struct kunit *test)
{
	struct request_queue *q = iobuf_fake_queue(test);
	struct blk_iobuf_pool *a = blk_iobuf_pool_create(2, 4);
	struct blk_iobuf_pool *b = blk_iobuf_pool_create(2, 4);
	struct blk_iobuf_pool *holder;
	struct folio *f;

	KUNIT_ASSERT_FALSE(test, IS_ERR(a));
	KUNIT_ASSERT_FALSE(test, IS_ERR(b));

	blk_queue_set_iobuf_pool(q, a);
	blk_iobuf_pool_put(a);		/* queue holds the only ref to a */

	/* a consumer takes a counted ref and checks out a folio */
	holder = blk_queue_get_iobuf_pool(q);
	KUNIT_ASSERT_PTR_EQ(test, holder, a);
	KUNIT_ASSERT_EQ(test, blk_iobuf_pool_alloc_batch(holder, &f, 1), 0);

	/* replace: the queue drops its ref to a while the folio is out */
	blk_queue_set_iobuf_pool(q, b);
	blk_iobuf_pool_put(b);

	/* the queue now serves b, but a is alive via the holder */
	KUNIT_EXPECT_PTR_NE(test, blk_queue_get_iobuf_pool(q), a);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_order(holder), 2u);

	/* return the folio to a and drop the holder ref -> a is destroyed */
	blk_iobuf_pool_free_batch(holder, &f, 1);
	KUNIT_EXPECT_EQ(test, blk_iobuf_pool_in_use(holder), 0u);
	blk_iobuf_pool_put(holder);

	/* b was fetched above; drop that ref, then detach */
	blk_iobuf_pool_put(blk_queue_get_iobuf_pool(q));
	blk_queue_clear_iobuf_pool(q);
}

static struct kunit_case blk_iobuf_test_cases[] = {
	KUNIT_CASE(iobuf_create_destroy),
	KUNIT_CASE(iobuf_create_rejects),
	KUNIT_CASE(iobuf_strict_capacity),
	KUNIT_CASE(iobuf_batch_all_or_nothing),
	KUNIT_CASE(iobuf_two_pools_exact),
	KUNIT_CASE(iobuf_queue_attach_get_clear),
	KUNIT_CASE(iobuf_queue_replace_outstanding),
	{}
};

static struct kunit_suite blk_iobuf_test_suite = {
	.name = "blk-iobuf",
	.test_cases = blk_iobuf_test_cases,
};

kunit_test_suite(blk_iobuf_test_suite);

MODULE_DESCRIPTION("KUnit tests for the bounded block iobuf folio pool");
MODULE_LICENSE("GPL");
