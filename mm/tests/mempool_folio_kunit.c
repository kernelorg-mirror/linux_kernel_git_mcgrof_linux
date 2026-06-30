// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit tests for folio-backed mempools.
 */
#include <kunit/test.h>
#include <linux/gfp.h>
#include <linux/mempool.h>
#include <linux/mm.h>
#include <linux/module.h>

#define TEST_MIN_NR	8

static void mempool_folio_order0_alloc_free(struct kunit *test)
{
	mempool_t *pool;
	struct folio *folio;

	pool = mempool_create_folio_pool(TEST_MIN_NR, 0);
	KUNIT_ASSERT_NOT_NULL(test, pool);

	folio = mempool_alloc(pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, folio_order(folio), 0u);

	mempool_free(folio, pool);
	mempool_destroy(pool);
}

static void mempool_folio_order3_alloc_free(struct kunit *test)
{
	mempool_t *pool;
	struct folio *folio;

	pool = mempool_create_folio_pool(TEST_MIN_NR, 3);
	KUNIT_ASSERT_NOT_NULL(test, pool);

	folio = mempool_alloc(pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, folio_order(folio), 3u);
	KUNIT_EXPECT_EQ(test, folio_size(folio), (size_t)PAGE_SIZE << 3);

	mempool_free(folio, pool);
	mempool_destroy(pool);
}

static void mempool_folio_alloc_all_free_all(struct kunit *test)
{
	mempool_t *pool;
	struct folio *folios[TEST_MIN_NR];
	int i;

	pool = mempool_create_folio_pool(TEST_MIN_NR, 2);
	KUNIT_ASSERT_NOT_NULL(test, pool);

	for (i = 0; i < TEST_MIN_NR; i++) {
		folios[i] = mempool_alloc(pool, GFP_KERNEL | __GFP_NOWARN);
		KUNIT_ASSERT_NOT_NULL(test, folios[i]);
		KUNIT_EXPECT_EQ(test, folio_order(folios[i]), 2u);
	}

	for (i = 0; i < TEST_MIN_NR; i++)
		mempool_free(folios[i], pool);

	mempool_destroy(pool);
}

static void mempool_folio_resize(struct kunit *test)
{
	mempool_t *pool;
	struct folio *folio;
	int ret;

	pool = mempool_create_folio_pool(4, 1);
	KUNIT_ASSERT_NOT_NULL(test, pool);

	ret = mempool_resize(pool, 8);
	KUNIT_EXPECT_EQ(test, ret, 0);

	folio = mempool_alloc(pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, folio_order(folio), 1u);

	mempool_free(folio, pool);
	mempool_destroy(pool);
}

static void mempool_folio_init_pool(struct kunit *test)
{
	mempool_t pool;
	struct folio *folio;
	int ret;

	ret = mempool_init_folio_pool(&pool, TEST_MIN_NR, 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, mempool_initialized(&pool));

	folio = mempool_alloc(&pool, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, folio);
	KUNIT_EXPECT_EQ(test, folio_order(folio), 0u);

	mempool_free(folio, &pool);
	mempool_exit(&pool);
}

static struct kunit_case mempool_folio_test_cases[] = {
	KUNIT_CASE(mempool_folio_order0_alloc_free),
	KUNIT_CASE(mempool_folio_order3_alloc_free),
	KUNIT_CASE(mempool_folio_alloc_all_free_all),
	KUNIT_CASE(mempool_folio_resize),
	KUNIT_CASE(mempool_folio_init_pool),
	{}
};

static struct kunit_suite mempool_folio_test_suite = {
	.name = "mempool_folio",
	.test_cases = mempool_folio_test_cases,
};

kunit_test_suite(mempool_folio_test_suite);

MODULE_DESCRIPTION("KUnit tests for folio mempool");
MODULE_LICENSE("GPL");
