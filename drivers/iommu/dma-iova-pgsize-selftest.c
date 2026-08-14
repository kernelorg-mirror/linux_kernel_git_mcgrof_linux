// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA-IOVA page-size stress test.  The module never submits a command to the
 * target device; it only maps test-owned folios into otherwise unused IOVA.
 */
#include <linux/capability.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/prandom.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/utsname.h>

#define TEST_MAX_BYTES		SZ_1G
#define TEST_MAX_ITERATIONS	100000U
#define TEST_MAX_LIVE		4096U
#define TEST_MAX_FOLIOS		4096U
#define TEST_NAME_LEN		32
#define TEST_DMA_ATTRS		DMA_ATTR_REQUIRE_COHERENT

enum test_api { TEST_API_CURRENT, TEST_API_STRICT };
enum test_mode { TEST_MODE_IMMEDIATE, TEST_MODE_CHURN, TEST_MODE_RAMP };
enum test_negative { TEST_NEG_NONE, TEST_NEG_MISALIGNED, TEST_NEG_UNSUPPORTED };

struct test_config {
	enum test_api api;
	enum test_mode mode;
	enum test_negative negative;
	unsigned int order;
	unsigned int folios_per_mapping;
	unsigned int iterations;
	unsigned int max_live_requested;
	unsigned int max_live_effective;
	unsigned int seed;
	size_t folio_size;
	size_t mapping_size;
	size_t max_bytes;
};

struct latency_summary { u64 p50, p95, p99; };

struct test_metrics {
	u64 folio_alloc_attempts, folio_alloc_successes, folio_alloc_failures;
	u64 folio_frees, physical_alignment_failures;
	u64 physical_contiguity_failures;
	u64 iova_alloc_attempts, iova_alloc_successes, iova_alloc_failures;
	u64 iova_alignment_failures, pgsize_unsupported;
	u64 link_failures, sync_failures, mapping_successes, strict_successes;
	u64 destroy_count, iova_outstanding;
	u64 negative_rejections, negative_unexpected_accepts;
	u64 iterations_completed, peak_live, peak_live_bytes;
	u64 elapsed_ns, iova_min, iova_max;
	u64 *folio_latency, *iova_latency, *map_latency, *destroy_latency;
	unsigned int folio_samples, iova_samples, map_samples, destroy_samples;
	struct latency_summary folio_summary, iova_summary, map_summary;
	struct latency_summary destroy_summary;
	int last_errno;
};

struct test_mapping {
	struct dma_iova_state state;
	struct folio **folios;
	unsigned int nr_folios;
	size_t mapped_len;
	bool iova_allocated;
};

static char target_bdf[16];
static char api_name[TEST_NAME_LEN] = "current";
static char mode_name[TEST_NAME_LEN] = "immediate";
static char negative_name[TEST_NAME_LEN] = "none";
static char kernel_commit[41] = "unknown";
static unsigned int order = 9;
static unsigned int folios_per_mapping = 1;
static unsigned int iterations = 1;
static unsigned int max_live = 1;
static unsigned long max_bytes = SZ_64M;
static unsigned int seed = 1;
static char result_json[PAGE_SIZE] =
	"{\"schema_version\":1,\"status\":-61,\"status_text\":\"not_run\","
	"\"domain_token\":null}\n";

module_param_string(bdf, target_bdf, sizeof(target_bdf), 0600);
MODULE_PARM_DESC(bdf, "Target PCI BDF in dddd:bb:ss.f form");
module_param_string(api, api_name, sizeof(api_name), 0600);
MODULE_PARM_DESC(api, "DMA-IOVA API: current or strict");
module_param_string(mode, mode_name, sizeof(mode_name), 0600);
MODULE_PARM_DESC(mode, "Workload: immediate, churn, or ramp");
module_param_string(negative, negative_name, sizeof(negative_name), 0600);
MODULE_PARM_DESC(negative, "Strict negative: none, misaligned, or unsupported");
module_param_string(kernel_commit, kernel_commit, sizeof(kernel_commit), 0600);
MODULE_PARM_DESC(kernel_commit, "Source commit recorded in result metadata");
module_param(order, uint, 0600);
module_param(folios_per_mapping, uint, 0600);
module_param(iterations, uint, 0600);
module_param(max_live, uint, 0600);
module_param(max_bytes, ulong, 0600);
module_param(seed, uint, 0600);

static int cmp_u64(const void *left, const void *right)
{
	const u64 *a = left, *b = right;

	return (*a > *b) - (*a < *b);
}

static struct latency_summary summarize(u64 *samples, unsigned int nr)
{
	struct latency_summary result = {};

	if (!nr)
		return result;
	sort_nonatomic(samples, nr, sizeof(*samples), cmp_u64, NULL);
	result.p50 = samples[div_u64((u64)(nr - 1) * 50, 100)];
	result.p95 = samples[div_u64((u64)(nr - 1) * 95, 100)];
	result.p99 = samples[div_u64((u64)(nr - 1) * 99, 100)];
	return result;
}

static const char *api_string(enum test_api api)
{
	return api == TEST_API_STRICT ? "strict" : "current";
}

static const char *mode_string(enum test_mode mode)
{
	if (mode == TEST_MODE_CHURN)
		return "churn";
	if (mode == TEST_MODE_RAMP)
		return "ramp";
	return "immediate";
}

static const char *negative_string(enum test_negative negative)
{
	if (negative == TEST_NEG_MISALIGNED)
		return "misaligned";
	if (negative == TEST_NEG_UNSUPPORTED)
		return "unsupported";
	return "none";
}

static const char *domain_type_string(unsigned int type)
{
	if (type == IOMMU_DOMAIN_DMA_FQ)
		return "dma-fq";
	if (type == IOMMU_DOMAIN_DMA)
		return "dma";
	if (type == IOMMU_DOMAIN_IDENTITY)
		return "identity";
	if (type == IOMMU_DOMAIN_UNMANAGED)
		return "unmanaged";
	if (type == IOMMU_DOMAIN_BLOCKED)
		return "blocked";
	return "other";
}

static int parse_bdf(const char *name, int *domain, unsigned int *bus,
		     unsigned int *devfn)
{
	unsigned int segment, busnr, slot, function;
	char trailing;

	if (sscanf(name, "%x:%x:%x.%x%c", &segment, &busnr, &slot,
		   &function, &trailing) != 4)
		return -EINVAL;
	if (segment > 0xffff || busnr > 0xff || slot > 0x1f || function > 7)
		return -EINVAL;
	*domain = segment;
	*bus = busnr;
	*devfn = PCI_DEVFN(slot, function);
	return 0;
}

static bool valid_kernel_commit(void)
{
	unsigned int index;

	if (!strcmp(kernel_commit, "unknown"))
		return true;
	if (strlen(kernel_commit) != 40)
		return false;
	for (index = 0; index < 40; index++)
		if (!isxdigit(kernel_commit[index]))
			return false;
	return true;
}

static int parse_config(struct test_config *config)
{
	u64 capacity;

	memset(config, 0, sizeof(*config));
	if (!strcmp(api_name, "current"))
		config->api = TEST_API_CURRENT;
	else if (!strcmp(api_name, "strict"))
		config->api = TEST_API_STRICT;
	else
		return -EINVAL;
	if (!strcmp(mode_name, "immediate"))
		config->mode = TEST_MODE_IMMEDIATE;
	else if (!strcmp(mode_name, "churn"))
		config->mode = TEST_MODE_CHURN;
	else if (!strcmp(mode_name, "ramp"))
		config->mode = TEST_MODE_RAMP;
	else
		return -EINVAL;
	if (!strcmp(negative_name, "none"))
		config->negative = TEST_NEG_NONE;
	else if (!strcmp(negative_name, "misaligned"))
		config->negative = TEST_NEG_MISALIGNED;
	else if (!strcmp(negative_name, "unsupported"))
		config->negative = TEST_NEG_UNSUPPORTED;
	else
		return -EINVAL;
	if (config->negative != TEST_NEG_NONE && config->api != TEST_API_STRICT)
		return -EINVAL;
	if (!target_bdf[0] || !iterations || iterations > TEST_MAX_ITERATIONS ||
	    !max_live || max_live > TEST_MAX_LIVE || !folios_per_mapping ||
	    folios_per_mapping > TEST_MAX_FOLIOS || order > MAX_PAGE_ORDER ||
	    !max_bytes || max_bytes > TEST_MAX_BYTES)
		return -EINVAL;
	if (!valid_kernel_commit())
		return -EINVAL;
	if (config->negative == TEST_NEG_MISALIGNED &&
	    (!order || order == MAX_PAGE_ORDER))
		return -EINVAL;

	config->order = order;
	config->folios_per_mapping = folios_per_mapping;
	config->iterations = iterations;
	config->max_live_requested = max_live;
	config->max_bytes = max_bytes;
	config->seed = seed;
	config->folio_size = (size_t)PAGE_SIZE << order;
	if (check_mul_overflow(config->folio_size,
			       (size_t)folios_per_mapping,
			       &config->mapping_size))
		return -EOVERFLOW;
	if (config->mapping_size > config->max_bytes)
		return -E2BIG;
	capacity = div_u64(config->max_bytes, config->mapping_size);
	config->max_live_effective = config->mode == TEST_MODE_IMMEDIATE ?
		1 : min_t(u64, config->max_live_requested, capacity);
	return config->max_live_effective ? 0 : -E2BIG;
}

static bool verify_folio(struct folio *folio, const struct test_config *config,
			 struct test_metrics *metrics)
{
	unsigned long base = folio_pfn(folio), index;
	unsigned long nr_pages = folio_nr_pages(folio);
	phys_addr_t phys = (phys_addr_t)base << PAGE_SHIFT;
	bool valid = true;

	if (folio_order(folio) != config->order ||
	    !IS_ALIGNED(phys, config->folio_size)) {
		metrics->physical_alignment_failures++;
		valid = false;
	}
	if (nr_pages != (1UL << config->order)) {
		metrics->physical_contiguity_failures++;
		return false;
	}
	for (index = 0; index < nr_pages; index++) {
		if (page_to_pfn(folio_page(folio, index)) != base + index) {
			metrics->physical_contiguity_failures++;
			valid = false;
			break;
		}
	}
	return valid;
}

static void record_sample(u64 *samples, unsigned int *nr, unsigned int capacity,
			  u64 value)
{
	if (*nr < capacity)
		samples[(*nr)++] = value;
}

static void record_iova_range(struct test_metrics *metrics, dma_addr_t addr,
			      size_t size)
{
	u64 end;

	if (check_add_overflow((u64)addr, (u64)size, &end))
		end = U64_MAX;
	if (!metrics->iova_min || addr < metrics->iova_min)
		metrics->iova_min = addr;
	if (end > metrics->iova_max)
		metrics->iova_max = end;
}

static void release_folios(struct test_mapping *mapping,
			   struct test_metrics *metrics)
{
	unsigned int index;

	for (index = 0; index < mapping->nr_folios; index++) {
		if (!mapping->folios[index])
			continue;
		folio_put(mapping->folios[index]);
		metrics->folio_frees++;
	}
	kfree(mapping->folios);
	mapping->folios = NULL;
	mapping->nr_folios = 0;
}

static void destroy_mapping(struct device *dev, struct test_mapping *mapping,
			    struct test_metrics *metrics,
			    const struct test_config *config)
{
	u64 before;

	if (!mapping)
		return;
	if (mapping->iova_allocated) {
		before = ktime_get_ns();
		dma_iova_destroy(dev, &mapping->state, mapping->mapped_len,
				 DMA_BIDIRECTIONAL, TEST_DMA_ATTRS);
		record_sample(metrics->destroy_latency,
			      &metrics->destroy_samples, config->iterations,
			      ktime_get_ns() - before);
		metrics->destroy_count++;
		if (WARN_ON_ONCE(!metrics->iova_outstanding))
			metrics->last_errno = -EUCLEAN;
		else
			metrics->iova_outstanding--;
		mapping->iova_allocated = false;
	}
	release_folios(mapping, metrics);
	kfree(mapping);
}

static struct test_mapping *create_mapping(struct device *dev,
					   const struct test_config *config,
					   struct test_metrics *metrics)
{
	struct test_mapping *mapping;
	phys_addr_t phys;
	u64 folio_before, iova_before, map_before;
	unsigned int index;
	int ret = 0;

	mapping = kzalloc_obj(*mapping, GFP_KERNEL);
	if (!mapping) {
		ret = -ENOMEM;
		goto out_error;
	}
	mapping->folios = kcalloc(config->folios_per_mapping,
				   sizeof(*mapping->folios), GFP_KERNEL);
	if (!mapping->folios) {
		ret = -ENOMEM;
		goto out_free_mapping;
	}

	folio_before = ktime_get_ns();
	for (index = 0; index < config->folios_per_mapping; index++) {
		struct folio *folio;

		metrics->folio_alloc_attempts++;
		folio = folio_alloc(GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN |
				    __GFP_ZERO, config->order);
		if (!folio) {
			metrics->folio_alloc_failures++;
			ret = -EAGAIN;
			record_sample(metrics->folio_latency,
				      &metrics->folio_samples,
				      config->iterations,
				      ktime_get_ns() - folio_before);
			goto out_release;
		}
		mapping->folios[index] = folio;
		mapping->nr_folios++;
		metrics->folio_alloc_successes++;
		if (!verify_folio(folio, config, metrics)) {
			ret = -EUCLEAN;
			record_sample(metrics->folio_latency,
				      &metrics->folio_samples,
				      config->iterations,
				      ktime_get_ns() - folio_before);
			goto out_release;
		}
	}
	record_sample(metrics->folio_latency, &metrics->folio_samples,
		      config->iterations, ktime_get_ns() - folio_before);

	phys = page_to_phys(&mapping->folios[0]->page);
	metrics->iova_alloc_attempts++;
	iova_before = ktime_get_ns();
	if (config->api == TEST_API_STRICT) {
		ret = dma_iova_alloc_pgsized(dev, &mapping->state, phys,
					     config->mapping_size,
					     config->folio_size);
	} else if (!dma_iova_try_alloc(dev, &mapping->state, phys,
					config->mapping_size)) {
		ret = -ENOSPC;
	}
	record_sample(metrics->iova_latency, &metrics->iova_samples,
		      config->iterations, ktime_get_ns() - iova_before);
	if (ret) {
		metrics->iova_alloc_failures++;
		if (ret == -EOPNOTSUPP)
			metrics->pgsize_unsupported++;
		goto out_release;
	}
	mapping->iova_allocated = true;
	metrics->iova_alloc_successes++;
	metrics->iova_outstanding++;
	record_iova_range(metrics, mapping->state.addr, config->mapping_size);
	if (!IS_ALIGNED(mapping->state.addr, config->folio_size)) {
		metrics->iova_alignment_failures++;
		if (config->api == TEST_API_STRICT) {
			ret = -EUCLEAN;
			goto out_destroy;
		}
	}

	map_before = ktime_get_ns();
	for (index = 0; index < mapping->nr_folios; index++) {
		phys = page_to_phys(&mapping->folios[index]->page);
		if (config->api == TEST_API_STRICT)
			ret = dma_iova_link_pgsized(dev, &mapping->state, phys,
						    mapping->mapped_len,
						    config->folio_size,
						    DMA_BIDIRECTIONAL,
						    TEST_DMA_ATTRS);
		else
			ret = dma_iova_link(dev, &mapping->state, phys,
					    mapping->mapped_len,
					    config->folio_size,
					    DMA_BIDIRECTIONAL,
					    TEST_DMA_ATTRS);
		if (ret) {
			metrics->link_failures++;
			goto out_record_map_destroy;
		}
		mapping->mapped_len += config->folio_size;
	}

	ret = dma_iova_sync(dev, &mapping->state, 0, mapping->mapped_len);
	if (ret) {
		metrics->sync_failures++;
		goto out_record_map_destroy;
	}
	record_sample(metrics->map_latency, &metrics->map_samples,
		      config->iterations, ktime_get_ns() - map_before);
	metrics->mapping_successes++;
	if (config->api == TEST_API_STRICT)
		metrics->strict_successes++;
	return mapping;

out_record_map_destroy:
	record_sample(metrics->map_latency, &metrics->map_samples,
		      config->iterations, ktime_get_ns() - map_before);
out_destroy:
	destroy_mapping(dev, mapping, metrics, config);
	mapping = NULL;
	goto out_error;
out_release:
	release_folios(mapping, metrics);
out_free_mapping:
	kfree(mapping);
out_error:
	metrics->last_errno = ret;
	return ERR_PTR(ret);
}

static bool resource_error(int ret)
{
	return ret == -EAGAIN || ret == -ENOMEM || ret == -ENOSPC;
}

static int run_workload(struct device *dev, const struct test_config *config,
			struct test_metrics *metrics)
{
	struct rnd_state random;
	struct test_mapping **live;
	unsigned int live_count = 0, iteration, victim;
	int ret = 0;

	live = kcalloc(config->max_live_effective, sizeof(*live), GFP_KERNEL);
	if (!live)
		return -ENOMEM;
	prandom_seed_state(&random, config->seed);

	for (iteration = 0; iteration < config->iterations; iteration++) {
		struct test_mapping *mapping;

		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			break;
		}
		if (config->mode == TEST_MODE_CHURN &&
		    live_count == config->max_live_effective) {
			victim = prandom_u32_state(&random) % live_count;
			destroy_mapping(dev, live[victim], metrics, config);
			live[victim] = live[--live_count];
		}

		mapping = create_mapping(dev, config, metrics);
		metrics->iterations_completed++;
		if (IS_ERR(mapping)) {
			ret = PTR_ERR(mapping);
			if (!resource_error(ret))
				break;
			ret = 0;
			cond_resched();
			continue;
		}

		if (config->mode == TEST_MODE_IMMEDIATE) {
			metrics->peak_live = max_t(u64, metrics->peak_live, 1);
			metrics->peak_live_bytes = max_t(u64,
					metrics->peak_live_bytes,
					config->mapping_size);
			destroy_mapping(dev, mapping, metrics, config);
		} else {
			live[live_count++] = mapping;
			metrics->peak_live = max_t(u64, metrics->peak_live,
						   live_count);
			metrics->peak_live_bytes = max_t(u64,
					metrics->peak_live_bytes,
					(u64)live_count * config->mapping_size);
			if (config->mode == TEST_MODE_RAMP &&
			    live_count == config->max_live_effective)
				break;
		}
		cond_resched();
	}

	while (live_count)
		destroy_mapping(dev, live[--live_count], metrics, config);
	kfree(live);
	if (!ret && !metrics->mapping_successes)
		ret = -ENODATA;
	return ret;
}

static size_t find_unsupported_pgsize(unsigned long bitmap)
{
	size_t size;

	/*
	 * Some domains support every page-size power within TEST_MAX_BYTES.
	 * Include bounded sub-page powers so this negative remains useful
	 * without asking the IOVA allocator to reserve an enormous test range.
	 */
	for (size = 1; size <= TEST_MAX_BYTES; size <<= 1) {
		if (!(bitmap & size))
			return size;
		if (size == TEST_MAX_BYTES)
			break;
	}
	return 0;
}

static int run_unsupported_negative(struct device *dev,
				    struct iommu_domain *domain,
				    struct test_metrics *metrics)
{
	struct dma_iova_state state;
	size_t unsupported = find_unsupported_pgsize(domain->pgsize_bitmap);
	int ret;

	if (!unsupported)
		return -EOPNOTSUPP;
	metrics->iova_alloc_attempts++;
	ret = dma_iova_alloc_pgsized(dev, &state, 0, unsupported, unsupported);
	if (ret == -EOPNOTSUPP) {
		metrics->iova_alloc_failures++;
		metrics->pgsize_unsupported++;
		metrics->negative_rejections++;
		metrics->iterations_completed = 1;
		metrics->last_errno = ret;
		return 0;
	}
	if (ret)
		return ret;

	metrics->iova_alloc_successes++;
	metrics->negative_unexpected_accepts++;
	dma_iova_destroy(dev, &state, 0, DMA_BIDIRECTIONAL, TEST_DMA_ATTRS);
	metrics->destroy_count++;
	metrics->iterations_completed = 1;
	return -EUCLEAN;
}

static int run_misaligned_negative(struct device *dev,
				   const struct test_config *config,
				   struct test_metrics *metrics)
{
	struct dma_iova_state state;
	struct folio *guard;
	phys_addr_t base;
	size_t mapped_len = 0;
	int ret, sync_ret;

	metrics->folio_alloc_attempts++;
	guard = folio_alloc(GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN |
			    __GFP_ZERO, config->order + 1);
	if (!guard) {
		metrics->folio_alloc_failures++;
		return -EAGAIN;
	}
	metrics->folio_alloc_successes++;
	base = page_to_phys(&guard->page);
	if (!IS_ALIGNED(base, config->folio_size << 1)) {
		metrics->physical_alignment_failures++;
		ret = -EUCLEAN;
		goto out_folio;
	}

	metrics->iova_alloc_attempts++;
	ret = dma_iova_alloc_pgsized(dev, &state, base, config->folio_size,
				     config->folio_size);
	if (ret) {
		metrics->iova_alloc_failures++;
		if (ret == -EOPNOTSUPP)
			metrics->pgsize_unsupported++;
		goto out_folio;
	}
	metrics->iova_alloc_successes++;
	metrics->iova_outstanding++;
	record_iova_range(metrics, state.addr, config->folio_size);

	ret = dma_iova_link_pgsized(dev, &state, base + PAGE_SIZE, 0,
				    config->folio_size, DMA_BIDIRECTIONAL,
				    TEST_DMA_ATTRS);
	if (ret == -EINVAL) {
		metrics->negative_rejections++;
		metrics->link_failures++;
		metrics->last_errno = ret;
		ret = 0;
	} else if (!ret) {
		metrics->negative_unexpected_accepts++;
		mapped_len = config->folio_size;
		sync_ret = dma_iova_sync(dev, &state, 0, mapped_len);
		if (sync_ret)
			metrics->sync_failures++;
		ret = -EUCLEAN;
	}
	dma_iova_destroy(dev, &state, mapped_len, DMA_BIDIRECTIONAL,
			 TEST_DMA_ATTRS);
	metrics->destroy_count++;
	metrics->iova_outstanding--;
	metrics->iterations_completed = 1;
out_folio:
	folio_put(guard);
	metrics->folio_frees++;
	return ret;
}

static const char *iommu_driver_name(struct device *dev)
{
	if (dev->iommu && dev->iommu->iommu_dev &&
	    dev->iommu->iommu_dev->dev)
		return dev_driver_string(dev->iommu->iommu_dev->dev);
	return "unknown";
}

static void format_result(int status, const struct test_config *config,
			  const struct test_metrics *metrics,
			  struct pci_dev *pdev, struct iommu_domain *domain)
{
	u64 iova_size = metrics->iova_max > metrics->iova_min ?
		metrics->iova_max - metrics->iova_min : 0;
	u64 expected_leaf_bytes = metrics->mapping_successes * config->mapping_size;
	u64 folio_leaks = metrics->folio_alloc_successes - metrics->folio_frees;
	const char *driver = pdev && pdev->driver ? pdev->driver->name : "none";
	const char *iommu_driver = pdev ? iommu_driver_name(&pdev->dev) : "unknown";
	const char *commit = valid_kernel_commit() ? kernel_commit : "unknown";
	unsigned int domain_type = domain ? domain->type : 0;
	unsigned long pgsize_bitmap = domain ? domain->pgsize_bitmap : 0;

	scnprintf(result_json, sizeof(result_json),
		"{\"schema_version\":1,\"status\":%d,\"status_text\":\"%s\","
		"\"kernel_release\":\"%s\",\"kernel_commit\":\"%s\","
		"\"device_bdf\":\"%s\","
		"\"device_driver\":\"%s\",\"iommu_driver\":\"%s\","
		"\"domain_token\":\"%p\",\"domain_type\":\"%s\","
		"\"domain_type_raw\":%u,"
		"\"pgsize_bitmap\":\"0x%lx\",\"api\":\"%s\","
		"\"mode\":\"%s\",\"negative\":\"%s\",\"order\":%u,"
		"\"folio_size\":%zu,\"folios_per_mapping\":%u,"
		"\"mapping_size\":%zu,\"iterations\":%u,"
		"\"iterations_completed\":%llu,\"max_live_requested\":%u,"
		"\"max_live_effective\":%u,\"max_bytes\":%zu,\"seed\":%u,"
		"\"folio_alloc_attempts\":%llu,\"folio_alloc_successes\":%llu,"
		"\"folio_alloc_failures\":%llu,\"folio_frees\":%llu,"
		"\"folio_leaks\":%llu,\"physical_alignment_failures\":%llu,"
		"\"physical_contiguity_failures\":%llu,"
		"\"iova_alloc_attempts\":%llu,\"iova_alloc_successes\":%llu,"
		"\"iova_alloc_failures\":%llu,\"iova_alignment_failures\":%llu,"
		"\"pgsize_unsupported\":%llu,\"link_failures\":%llu,"
		"\"sync_failures\":%llu,\"mapping_successes\":%llu,"
		"\"strict_successes\":%llu,\"destroy_count\":%llu,"
		"\"iova_leaks\":%llu,\"negative_rejections\":%llu,"
		"\"negative_unexpected_accepts\":%llu,\"peak_live\":%llu,"
		"\"peak_live_bytes\":%llu,\"iova_start\":%llu,"
		"\"iova_size\":%llu,\"expected_leaf_mapped_bytes\":%llu,"
		"\"elapsed_ns\":%llu,\"last_errno\":%d,"
		"\"latency_ns\":{\"folio_alloc\":{\"p50\":%llu,\"p95\":%llu,"
		"\"p99\":%llu},\"iova_alloc\":{\"p50\":%llu,\"p95\":%llu,"
		"\"p99\":%llu},\"map\":{\"p50\":%llu,\"p95\":%llu,"
		"\"p99\":%llu},\"destroy\":{\"p50\":%llu,\"p95\":%llu,"
		"\"p99\":%llu}},\"leaf_size_histogram\":null,"
		"\"leaf_histogram_source\":\"iommu:iommu_map_leaf\"}\n",
		status, status ? "failed" : "passed", init_utsname()->release,
		commit, pdev ? pci_name(pdev) : "none", driver, iommu_driver,
		domain, domain ? domain_type_string(domain_type) : "none",
		domain_type, pgsize_bitmap,
		api_string(config->api), mode_string(config->mode),
		negative_string(config->negative), config->order,
		config->folio_size, config->folios_per_mapping,
		config->mapping_size, config->iterations,
		metrics->iterations_completed, config->max_live_requested,
		config->max_live_effective, config->max_bytes, config->seed,
		metrics->folio_alloc_attempts, metrics->folio_alloc_successes,
		metrics->folio_alloc_failures, metrics->folio_frees, folio_leaks,
		metrics->physical_alignment_failures,
		metrics->physical_contiguity_failures,
		metrics->iova_alloc_attempts, metrics->iova_alloc_successes,
		metrics->iova_alloc_failures, metrics->iova_alignment_failures,
		metrics->pgsize_unsupported, metrics->link_failures,
		metrics->sync_failures, metrics->mapping_successes,
		metrics->strict_successes, metrics->destroy_count,
		metrics->iova_outstanding, metrics->negative_rejections,
		metrics->negative_unexpected_accepts, metrics->peak_live,
		metrics->peak_live_bytes, metrics->iova_min, iova_size,
		expected_leaf_bytes, metrics->elapsed_ns, metrics->last_errno,
		metrics->folio_summary.p50, metrics->folio_summary.p95,
		metrics->folio_summary.p99, metrics->iova_summary.p50,
		metrics->iova_summary.p95, metrics->iova_summary.p99,
		metrics->map_summary.p50,
		metrics->map_summary.p95, metrics->map_summary.p99,
		metrics->destroy_summary.p50, metrics->destroy_summary.p95,
		metrics->destroy_summary.p99);
}

static int execute_test(void)
{
	struct test_config config = {};
	struct test_metrics metrics = {};
	struct iommu_domain *domain = NULL;
	struct pci_dev *pdev = NULL;
	u64 before = ktime_get_ns();
	unsigned int bus, devfn;
	bool device_locked = false;
	int segment, ret;

	ret = parse_config(&config);
	if (ret)
		goto out_result;
	ret = parse_bdf(target_bdf, &segment, &bus, &devfn);
	if (ret)
		goto out_result;
	pdev = pci_get_domain_bus_and_slot(segment, bus, devfn);
	if (!pdev) {
		ret = -ENODEV;
		goto out_result;
	}
	device_lock(&pdev->dev);
	device_locked = true;

	metrics.folio_latency = kvcalloc(config.iterations,
					 sizeof(*metrics.folio_latency), GFP_KERNEL);
	metrics.iova_latency = kvcalloc(config.iterations,
					sizeof(*metrics.iova_latency), GFP_KERNEL);
	metrics.map_latency = kvcalloc(config.iterations,
				       sizeof(*metrics.map_latency), GFP_KERNEL);
	metrics.destroy_latency = kvcalloc(config.iterations,
					   sizeof(*metrics.destroy_latency), GFP_KERNEL);
	if (!metrics.folio_latency || !metrics.iova_latency ||
	    !metrics.map_latency ||
	    !metrics.destroy_latency) {
		ret = -ENOMEM;
		goto out_result;
	}

	if (!pdev->driver) {
		ret = -ENODEV;
		goto out_result;
	}
	if (pdev->driver->driver_managed_dma) {
		ret = -EPERM;
		goto out_result;
	}
	if (!dev_is_dma_coherent(&pdev->dev)) {
		ret = -EOPNOTSUPP;
		goto out_result;
	}
	domain = iommu_get_domain_for_dev(&pdev->dev);
	if (!domain || !iommu_is_dma_domain(domain)) {
		ret = -EOPNOTSUPP;
		goto out_result;
	}

	if (config.negative == TEST_NEG_UNSUPPORTED)
		ret = run_unsupported_negative(&pdev->dev, domain, &metrics);
	else if (config.negative == TEST_NEG_MISALIGNED)
		ret = run_misaligned_negative(&pdev->dev, &config, &metrics);
	else
		ret = run_workload(&pdev->dev, &config, &metrics);
	if (!ret && (metrics.iova_outstanding ||
		     metrics.folio_alloc_successes != metrics.folio_frees))
		ret = -EUCLEAN;
out_result:
	metrics.elapsed_ns = ktime_get_ns() - before;
	if (ret && !metrics.last_errno)
		metrics.last_errno = ret;
	metrics.folio_summary = summarize(metrics.folio_latency,
					  metrics.folio_samples);
	metrics.iova_summary = summarize(metrics.iova_latency,
					 metrics.iova_samples);
	metrics.map_summary = summarize(metrics.map_latency,
					metrics.map_samples);
	metrics.destroy_summary = summarize(metrics.destroy_latency,
					    metrics.destroy_samples);
	format_result(ret, &config, &metrics, pdev, domain);
	if (device_locked)
		device_unlock(&pdev->dev);
	kvfree(metrics.destroy_latency);
	kvfree(metrics.map_latency);
	kvfree(metrics.iova_latency);
	kvfree(metrics.folio_latency);
	if (pdev)
		pci_dev_put(pdev);
	return ret;
}

static int run_set(const char *value, const struct kernel_param *kp)
{
	bool run;
	int ret;

	(void)kp;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	ret = kstrtobool(value, &run);
	if (ret)
		return ret;
	if (!run)
		return -EINVAL;
	return execute_test();
}

static int result_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buffer, PAGE_SIZE, "%s", result_json);
}

static const struct kernel_param_ops run_ops = {
	.set = run_set,
};

static const struct kernel_param_ops result_ops = {
	.get = result_get,
};

module_param_cb(run, &run_ops, NULL, 0200);
MODULE_PARM_DESC(run, "Write 1 to execute the configured test synchronously");
module_param_cb(result, &result_ops, NULL, 0400);
MODULE_PARM_DESC(result, "Machine-readable result from the last run");

static int __init dma_iova_pgsize_selftest_init(void)
{
	pr_info("loaded; no mappings are created until run=1\n");
	return 0;
}

static void __exit dma_iova_pgsize_selftest_exit(void)
{
}

module_init(dma_iova_pgsize_selftest_init);
module_exit(dma_iova_pgsize_selftest_exit);

MODULE_DESCRIPTION("Bounded DMA-IOVA page-size stress selftest");
MODULE_LICENSE("GPL");
