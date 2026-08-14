// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/iommu.h>
#include <linux/sizes.h>

#include "dma-iommu.h"

static void dma_iova_pgsize_invalid_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M, 0, SZ_2M, 0),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M, 0, SZ_2M,
						SZ_2M + SZ_4K),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M, 0, 0, SZ_2M),
		-EINVAL);
}

static void dma_iova_pgsize_unsupported_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K, 0, SZ_2M, SZ_2M),
		-EOPNOTSUPP);
}

static void dma_iova_pgsize_alignment_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M, SZ_4K,
						SZ_2M, SZ_2M),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M, 0,
						SZ_2M + SZ_4K, SZ_2M),
		-EINVAL);
}

static void dma_iova_link_alignment_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_link(SZ_4K, 0, SZ_2M, SZ_2M),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_link(0, SZ_4K, SZ_2M, SZ_2M),
		-EINVAL);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_link(0, 0, SZ_2M + SZ_4K, SZ_2M),
		-EINVAL);
}

static void dma_iova_pgsize_valid_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_pgsize(SZ_4K | SZ_2M | SZ_1G,
						SZ_2M, 4 * SZ_2M, SZ_2M),
		0);
	KUNIT_EXPECT_EQ(test,
		iommu_dma_iova_validate_link(SZ_2M, 2 * SZ_2M,
					     4 * SZ_2M, SZ_2M),
		0);
}

struct dma_iova_test_domain {
	struct iommu_domain domain;
	size_t map_calls;
	size_t mapped_bytes;
	size_t unmapped_bytes;
	size_t min_pgsize;
	size_t max_pgsize;
	bool fail_partial;
};

static struct dma_iova_test_domain *
to_dma_iova_test_domain(struct iommu_domain *domain)
{
	return container_of(domain, struct dma_iova_test_domain, domain);
}

static int dma_iova_test_map_pages(struct iommu_domain *domain,
		unsigned long iova, phys_addr_t paddr, size_t pgsize,
		size_t pgcount, int prot, gfp_t gfp, size_t *mapped)
{
	struct dma_iova_test_domain *test_domain =
		to_dma_iova_test_domain(domain);

	test_domain->map_calls++;
	test_domain->min_pgsize = min(test_domain->min_pgsize, pgsize);
	test_domain->max_pgsize = max(test_domain->max_pgsize, pgsize);
	if (test_domain->fail_partial) {
		*mapped = pgsize;
		test_domain->mapped_bytes += *mapped;
		return -ENOMEM;
	}

	*mapped = pgsize * pgcount;
	test_domain->mapped_bytes += *mapped;
	return 0;
}

static size_t dma_iova_test_unmap_pages(struct iommu_domain *domain,
		unsigned long iova, size_t pgsize, size_t pgcount,
		struct iommu_iotlb_gather *iotlb_gather)
{
	struct dma_iova_test_domain *test_domain =
		to_dma_iova_test_domain(domain);
	size_t unmapped = pgsize * pgcount;

	test_domain->unmapped_bytes += unmapped;
	return unmapped;
}

static const struct iommu_domain_ops dma_iova_test_domain_ops = {
	.map_pages = dma_iova_test_map_pages,
	.unmap_pages = dma_iova_test_unmap_pages,
};

static void dma_iova_test_domain_init(struct dma_iova_test_domain *test_domain)
{
	memset(test_domain, 0, sizeof(*test_domain));
	test_domain->domain.type = IOMMU_DOMAIN_UNMANAGED;
	test_domain->domain.pgsize_bitmap = SZ_4K | SZ_2M;
	test_domain->domain.ops = &dma_iova_test_domain_ops;
	test_domain->min_pgsize = SIZE_MAX;
}

static void dma_iova_pgsize_core_map_test(struct kunit *test)
{
	struct dma_iova_test_domain test_domain;
	int ret;

	dma_iova_test_domain_init(&test_domain);
	ret = iommu_map_nosync_pgsized(&test_domain.domain, SZ_2M, SZ_2M,
			SZ_2M, SZ_2M, IOMMU_READ, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, test_domain.map_calls, (size_t)1);
	KUNIT_EXPECT_EQ(test, test_domain.min_pgsize, (size_t)SZ_2M);
	KUNIT_EXPECT_EQ(test, test_domain.max_pgsize, (size_t)SZ_2M);
	KUNIT_EXPECT_EQ(test, test_domain.mapped_bytes, (size_t)SZ_2M);
	KUNIT_EXPECT_EQ(test,
		iommu_unmap(&test_domain.domain, SZ_2M, SZ_2M),
		(size_t)SZ_2M);
}

static void dma_iova_legacy_small_leaf_test(struct kunit *test)
{
	struct dma_iova_test_domain test_domain;
	int ret;

	dma_iova_test_domain_init(&test_domain);
	ret = iommu_map_nosync(&test_domain.domain, SZ_4K, SZ_4K, SZ_64K,
			IOMMU_READ, GFP_KERNEL);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, test_domain.map_calls, (size_t)1);
	KUNIT_EXPECT_EQ(test, test_domain.min_pgsize, (size_t)SZ_4K);
	KUNIT_EXPECT_EQ(test, test_domain.mapped_bytes, (size_t)SZ_64K);
	KUNIT_EXPECT_EQ(test,
		iommu_unmap(&test_domain.domain, SZ_4K, SZ_64K),
		(size_t)SZ_64K);

	dma_iova_test_domain_init(&test_domain);
	ret = iommu_map_nosync_pgsized(&test_domain.domain, SZ_4K, SZ_4K,
			SZ_64K, SZ_2M, IOMMU_READ, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, test_domain.map_calls, (size_t)0);
}

static void dma_iova_pgsize_partial_unwind_test(struct kunit *test)
{
	struct dma_iova_test_domain test_domain;
	int ret;

	dma_iova_test_domain_init(&test_domain);
	test_domain.fail_partial = true;
	ret = iommu_map_nosync_pgsized(&test_domain.domain, SZ_2M, SZ_2M,
			2 * SZ_2M, SZ_2M, IOMMU_READ, GFP_KERNEL);
	KUNIT_EXPECT_EQ(test, ret, -ENOMEM);
	KUNIT_EXPECT_EQ(test, test_domain.map_calls, (size_t)1);
	KUNIT_EXPECT_EQ(test, test_domain.mapped_bytes, (size_t)SZ_2M);
	KUNIT_EXPECT_EQ(test, test_domain.unmapped_bytes, (size_t)SZ_2M);
}

static struct kunit_case dma_iova_pgsize_cases[] = {
	KUNIT_CASE(dma_iova_pgsize_invalid_test),
	KUNIT_CASE(dma_iova_pgsize_unsupported_test),
	KUNIT_CASE(dma_iova_pgsize_alignment_test),
	KUNIT_CASE(dma_iova_link_alignment_test),
	KUNIT_CASE(dma_iova_pgsize_valid_test),
	KUNIT_CASE(dma_iova_pgsize_core_map_test),
	KUNIT_CASE(dma_iova_legacy_small_leaf_test),
	KUNIT_CASE(dma_iova_pgsize_partial_unwind_test),
	{}
};

static struct kunit_suite dma_iova_pgsize_suite = {
	.name = "dma-iova-pgsize",
	.test_cases = dma_iova_pgsize_cases,
};

kunit_test_suite(dma_iova_pgsize_suite);

MODULE_DESCRIPTION("KUnit tests for minimum-page-size DMA-IOVA mappings");
MODULE_LICENSE("GPL");
