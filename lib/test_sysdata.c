/*
 * System Data test interface
 *
 * Copyright (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 *
 * This module provides an interface to trigger and test system data API
 * through a series of configurations and a few triggers. This driver
 * lacks any extra dependencies, and will not normally be loaded by the
 * system unless explicitly requested by name. You can also build this
 * driver into your kernel.
 *
 * Although all configurations are already written for and will be supported
 * for this test driver, ideally we should strive to see what mechanisms we
 * can put in place to instead automatically generate this sort of test
 * interface, test cases, and infer results. Its a simple enough interface that
 * should hopefully enable more exploring in this area.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/completion.h>
#include <linux/sysdata.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>

/* Used for the fallback default to test against */
#define TEST_SYSDATA "test-sysdata.bin"

/*
 * For device allocation / registration
 */
static DEFINE_MUTEX(reg_dev_mutex);
static LIST_HEAD(reg_test_devs);

/*
 * num_test_devs actually represents the *next* ID of the next
 * device we will allow to create.
 */
int num_test_devs;

/**
 * test_config - represents configuration for the sysdata API
 *
 * @name: the name of the primary sysdata file to look for
 * @default_name: a fallback example, used to test the optional callback
 * 	mechanism.
 * @async: true if you want to trigger an async request. This will use
 * 	sysdata_file_request_async(). If false the synchronous call will
 * 	be used, sysdata_file_request().
 * @optional: whether or not the sysdata is optional refer to the
 *	struct sysdata_file_desc @optional field for more information.
 * @keep: whether or not we wish to free the sysdata on our own, refer to
 *	the struct sysdata_file_desc @keep field for more information.
 * @enable_opt_cb: whether or not the optional callback should be set
 *	on a trigger. There is no equivalent setting on the struct
 *	sysdata_file_desc as this is implementation specific, and in
 *	in sysdata API its explicit if you had defined an optional call
 *	back for your descriptor with either SYSDATA_SYNC_OPT_CB() or
 *	SYSDATA_ASYNC_OPT_CB(). Since the descriptor is a const we have
 *	no option but to use a flag and two const structs to decide which
 *	one we should use.
 * @test_result: a test may use this to collect the result from the call
 *	of the sysdata_file_request_async() or sysdata_file_request() calls
 *	used in their tests. Note that for async calls this typically will
 *	be a successful result (0) unless of course you've have sent in
 *	a bogus descriptor, or the system is out of memory. Tests against
 *	the callbacks can only be implementation specific, so we don't
 *	test for that for now but it may make sense to build tests cases
 *	against a series of semantically similar family of callbacks that
 *	generally represents usage in the kernel. Synchronous calls return
 *	bogus error checks against the descriptor as well, but also return
 *	the result of the work from the callbacks. You can therefore rely
 *	on sync calls if you really want to test for the callback results
 *	as well. Errors you can expect:
 *
 *	API specific:
 *
 *	0:		success for sync, for async it means request was sent
 *	-EINVAL:	invalid descriptor or request
 *	-ENOENT:	files not found
 *
 *	System environment:
 *
 *	-ENOMEM:	memory pressure on system
 *	-ENODEV:	out of number of devices to test
 *
 * The ordering of elements in this struct must match the exact order of the
 * elements in the ATTRIBUTE_GROUPS(test_dev_config), this is done to know
 * what corresponding field each device attribute configuration entry maps
 * to what struct member on test_alloc_dev_attrs().
 */
struct test_config {
	char *name;
	char *default_name;
	bool async;
	bool optional;
	bool keep;
	bool enable_opt_cb;

	int test_result;
};

enum test_config_name {
	CONFIG_NAME = 0,
	CONFIG_DEFAULT_NAME = 1,
	CONFIG_ASYNC = 2,
	CONFIG_OPTIONAL = 3,
	CONFIG_KEEP = 4,
	CONFIG_ENABLE_OPT_CB = 5,
	CONFIG_TEST_RESULT = 6,
};

/**
 * test_sysdata_private - private device driver sysdata representation
 *
 * @size: size of the data copied, in bytes
 * @data: the actual data we copied over from sysdata
 * @written: true if a callback managed to copy data over to the device
 *	successfully. Since different callbacks are used for this purpose
 *	having the data written does not necessarily mean a test case
 *	completed successfully. Each tests case has its own specific
 *	goals.
 *
 * Private representation of buffer where we put the device system data */
struct test_sysdata_private {
	size_t size;
	u8 *data;
	bool written;
};

/**
 * sysdata_test_device - test device to help test sysdata
 *
 * @dev_idx: unique ID for test device
 * @config: this keeps the device's own configuration. Instead of creating
 *	different triggers for all possible test cases we can think of in
 *	kernel, we expose a set possible device attributes for tuning the
 *	sysdata API and we to let you tune them in userspace. We then just
 *	provide one trigger.
 * @test_sysdata: internal private representation of a storage area
 *	a driver might typically use to stuff firmware / sysdata.
 * @misc_dev: we use a misc device under the hood
 * @dev: pointer to misc_dev's own struct device
 * @sysdata_mutex: for access into the @sysdata, the fake storage location for
 * 	the system data we copy.
 * @config_mutex:
 * @trigger_mutex: all triggers are mutually exclusive when testing. To help
 *	enable testing you can create a different device, each device has its
 *	own set of protections, mimicking real devices.
 * list: needed to be part of the reg_test_devs
 */
struct sysdata_test_device {
	int dev_idx;
	struct test_config config;
	struct test_sysdata_private test_sysdata;
	struct miscdevice misc_dev;
	struct device *dev;

	struct mutex sysdata_mutex;
	struct mutex config_mutex;
	struct mutex trigger_mutex;
	struct list_head list;
};

static struct miscdevice *dev_to_misc_dev(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static
struct sysdata_test_device *misc_dev_to_test_dev(struct miscdevice *misc_dev)
{
	return container_of(misc_dev, struct sysdata_test_device, misc_dev);
}

static struct sysdata_test_device *dev_to_test_dev(struct device *dev)
{
	struct miscdevice *misc_dev;

	misc_dev = dev_to_misc_dev(dev);

	return misc_dev_to_test_dev(misc_dev);
}

static ssize_t test_fw_misc_read(struct file *f, char __user *buf,
				 size_t size, loff_t *offset)
{
	struct miscdevice *misc_dev = f->private_data;
	struct sysdata_test_device *test_dev = misc_dev_to_test_dev(misc_dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	ssize_t rc = 0;

	mutex_lock(&test_dev->sysdata_mutex);
	if (test_sysdata->written)
		rc = simple_read_from_buffer(buf, size, offset,
					     test_sysdata->data,
					     test_sysdata->size);
	mutex_unlock(&test_dev->sysdata_mutex);

	return rc;
}

static const struct file_operations test_fw_fops = {
	.owner          = THIS_MODULE,
	.read           = test_fw_misc_read,
};

static void free_test_sysdata(struct test_sysdata_private *test_sysdata)
{
	kfree(test_sysdata->data);
	test_sysdata->data = NULL;
	test_sysdata->size = 0;
	test_sysdata->written = false;
}

static int test_load_sysdata(struct sysdata_test_device *test_dev,
			     const struct sysdata_file *sysdata)
{
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	int ret = 0;

	if (!sysdata)
		return -ENOENT;

	mutex_lock(&test_dev->sysdata_mutex);

	free_test_sysdata(test_sysdata);

	test_sysdata->data = kzalloc(sysdata->size, GFP_KERNEL);
	if (!test_sysdata->data) {
		ret = -ENOMEM;
		goto out;
	}

	memcpy(test_sysdata->data, sysdata->data, sysdata->size);
	test_sysdata->size = sysdata->size;
	test_sysdata->written = true;

	dev_info(test_dev->dev, "loaded: %zu\n", test_sysdata->size);

out:
	mutex_unlock(&test_dev->sysdata_mutex);

	return ret;
}

static int sync_found_cb(void *context, const struct sysdata_file *sysdata)
{
	struct sysdata_test_device *test_dev = context;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	if (ret)
		dev_info(test_dev->dev, "unable to write sysdata: %d\n", ret);
	return ret;
}

static ssize_t trigger_request_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_SYNC_FOUND(sync_found_cb, test_dev),
	};

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	pr_info("loading '%s'\n", name);

	rc = sysdata_file_request(name, &sysdata_desc, dev);
	if (rc) {
		dev_err(dev, "load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	rc = count;

out:
	mutex_unlock(&test_dev->trigger_mutex);
	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request);

static int sync_found_keep_cb(void *context,
			      const struct sysdata_file *sysdata)
{
	struct sysdata_test_device *test_dev = context;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	if (ret)
		dev_info(test_dev->dev, "unable to write sysdata: %d\n", ret);
	release_sysdata_file(sysdata);

	return ret;
}

static ssize_t trigger_request_keep_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc_keep = {
		SYSDATA_SYNC_FOUND(sync_found_keep_cb, test_dev),
		.keep = true, /* we will free sysdata */
	};

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request(name, &sysdata_desc_keep, dev);
	if (rc) {
		dev_err(dev, "load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	rc = count;

out:
	mutex_unlock(&test_dev->trigger_mutex);
	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request_keep);

static ssize_t trigger_request_opt_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc_opt = {
		.optional = true, /* this should not be chatty on error */
		SYSDATA_SYNC_FOUND(sync_found_cb, test_dev),
	};

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);

	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request(name, &sysdata_desc_opt, dev);
	if (rc) {
		dev_info(dev, "load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	rc = count;

out:
	mutex_unlock(&test_dev->trigger_mutex);
	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request_opt);

static int sync_found_opt_cb(void *context)
{
	int rc;
	struct sysdata_test_device *test_dev = context;
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_SYNC_FOUND(sync_found_cb, test_dev),
	};

	dev_info(test_dev->dev, "loading default fallback '%s'\n",
		 TEST_SYSDATA);

	rc = sysdata_file_request(TEST_SYSDATA, &sysdata_desc, test_dev->dev);
	if (rc)
		dev_err(test_dev->dev, "load of default '%s' failed: %d\n",
			TEST_SYSDATA, rc);

	return rc;
}

static ssize_t trigger_request_opt_default_store(struct device *dev,
						 struct device_attribute *attr,
						 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc_opt = {
		.optional = true, /* this should not be chatty on error */
		SYSDATA_SYNC_FOUND(sync_found_cb, test_dev),
		SYSDATA_SYNC_OPT_CB(sync_found_opt_cb, test_dev),
	};

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request(name, &sysdata_desc_opt, dev);
	if (rc) {
		dev_err(dev, "load of '%s' failed: %d\n", name, rc);
		goto out;
	}
	rc = count;

out:
	mutex_unlock(&test_dev->trigger_mutex);
	kfree(name);

	return rc;
}
static DEVICE_ATTR_WO(trigger_request_opt_default);

static void async_request_cb(const struct sysdata_file *sysdata, void *context)
{
	struct sysdata_test_device *test_dev = context;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	if (ret)
		dev_err(test_dev->dev, "unable to write sysdata\n");
}

static ssize_t trigger_async_request_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_DEFAULT_ASYNC(async_request_cb, test_dev),
	};
	async_cookie_t async_cookie;


	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request_async(name, &sysdata_desc, dev,
					&async_cookie);
	if (rc) {
		dev_err(dev, "async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	sysdata_synchronize_request(async_cookie);

	if (test_sysdata->written) {
		dev_info(dev, "loaded: %zu\n", test_sysdata->size);
		rc = count;
	} else {
		dev_err(dev, "failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_dev->trigger_mutex);
	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request);

static void async_request_keep_cb(const struct sysdata_file *sysdata,
				  void *context)
{
	struct sysdata_test_device *test_dev = context;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	if (ret)
		dev_info(test_dev->dev, "unable to write sysdata\n");
	release_sysdata_file(sysdata);
}

static ssize_t trigger_async_request_keep_store(struct device *dev,
						struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_DEFAULT_ASYNC(async_request_keep_cb, test_dev),
		.keep = true, /* we will free sysdata */
	};
	async_cookie_t async_cookie;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request_async(name, &sysdata_desc, dev,
					&async_cookie);
	if (rc) {
		dev_err(dev, "async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	sysdata_synchronize_request(async_cookie);

	if (test_sysdata->written) {
		dev_info(dev, "loaded: %zu\n", test_sysdata->size);
		rc = count;
	} else {
		dev_err(dev, "failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_dev->trigger_mutex);
	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request_keep);

static ssize_t trigger_async_request_opt_store(struct device *dev,
					       struct device_attribute *attr,
					       const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc = {
		.optional = true, /* this should not be chatty on error */
		SYSDATA_DEFAULT_ASYNC(async_request_cb, test_dev),
	};
	async_cookie_t async_cookie;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request_async(name, &sysdata_desc, dev,
					&async_cookie);
	if (rc) {
		dev_info(dev, "async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	sysdata_synchronize_request(async_cookie);

	if (test_sysdata->written) {
		dev_info(dev, "loaded: %zu\n", test_sysdata->size);
		rc = count;
	} else {
		pr_err("failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_dev->trigger_mutex);
	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request_opt);

static void async_request_opt_default_cb(void *context)
{
	struct sysdata_test_device *test_dev = context;
	int rc;
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_DEFAULT_ASYNC(async_request_cb, test_dev),
	};
	async_cookie_t async_cookie;

	/* Our parent already locked the test_dev->trigger_mutex */
	dev_info(test_dev->dev, "loading '%s'\n", TEST_SYSDATA);

	/* We don't have to really use async but let's keep at it */
	rc = sysdata_file_request_async(TEST_SYSDATA, &sysdata_desc,
					test_dev->dev,
					&async_cookie);
	if (rc) {
		dev_err(test_dev->dev,
			"async load of default '%s' failed: %d\n",
			TEST_SYSDATA, rc);
		return;
	}

	sysdata_synchronize_request(async_cookie);
	/* Leave all the error checking for the main caller */
}

static void async_request_cb_top(const struct sysdata_file *sysdata,
				 void *context)
{
	struct sysdata_test_device *test_dev = context;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	/*
	 * If we failed, we need the caller to wait a bit more,
	 * our other secondary callback will be called, we can
	 * complete there.
	 */
	if (ret)
		dev_err(test_dev->dev, "unable to write sysdata\n");
}

static ssize_t
trigger_async_request_opt_default_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	int rc;
	char *name;
	const struct sysdata_file_desc sysdata_desc = {
		.optional = true, /* this should not be chatty on error */
		SYSDATA_DEFAULT_ASYNC(async_request_cb_top, test_dev),
		SYSDATA_ASYNC_OPT_CB(async_request_opt_default_cb, test_dev),
	};
	async_cookie_t async_cookie;

	name = kstrndup(buf, count, GFP_KERNEL);
	if (!name)
		return -ENOSPC;

	mutex_lock(&test_dev->trigger_mutex);
	dev_info(dev, "loading '%s'\n", name);

	rc = sysdata_file_request_async(name, &sysdata_desc, dev,
					&async_cookie);
	if (rc) {
		dev_err(dev, "async load of '%s' failed: %d\n", name, rc);
		kfree(name);
		goto out;
	}
	/* Free 'name' ASAP, to test for race conditions */
	kfree(name);

	sysdata_synchronize_request(async_cookie);

	if (test_sysdata->written) {
		dev_info(dev, "loaded: %zu\n", test_sysdata->size);
		rc = count;
	} else {
		dev_err(dev, "failed to async load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_dev->trigger_mutex);
	return rc;
}
static DEVICE_ATTR_WO(trigger_async_request_opt_default);

static ssize_t config_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;
	int len = 0;

	mutex_lock(&test_dev->config_mutex);

	len += sprintf(buf, "Custom trigger configuration for: %s\n",
		       dev_name(dev));

	if (config->default_name)
		len += sprintf(buf+len, "default name:\t%s\n",
			       config->default_name);
	else
		len += sprintf(buf+len, "default name:\tEMTPY\n");

	if (config->name)
		len += sprintf(buf+len, "name:\t\t%s\n", config->name);
	else
		len += sprintf(buf+len, "name:\t\tEMPTY\n");

	len += sprintf(buf+len, "type:\t\t%s\n",
		       config->async ? "async" : "sync");
	len += sprintf(buf+len, "optional:\t%s\n",
		       config->optional ? "true" : "false");
	len += sprintf(buf+len, "enable_opt_cb:\t%s\n",
		       config->enable_opt_cb ? "true" : "false");
	len += sprintf(buf+len, "keep:\t\t%s\n",
		       config->keep ? "true" : "false");

	mutex_unlock(&test_dev->config_mutex);

	return len;
}
static DEVICE_ATTR_RO(config);

static int config_load_data(struct sysdata_test_device *test_dev,
			    const struct sysdata_file *sysdata)
{
	struct test_config *config = &test_dev->config;
	int ret;

	ret = test_load_sysdata(test_dev, sysdata);
	if (ret) {
		if (!config->optional)
			dev_info(test_dev->dev, "unable to write sysdata\n");
	}
	if (config->keep) {
		release_sysdata_file(sysdata);
		sysdata = NULL;
	}
	return ret;
}

static int config_req_default(struct sysdata_test_device *test_dev)
{
	struct test_config *config = &test_dev->config;
	int rc;
	/*
	 * Note: we don't chain config->optional here, we make this
	 * fallback file a requirement. It doesn't make much sense to test
	 * chaining further as the optional callback is implementation
	 * specific, by testing it once we test it for any possible
	 * chains. We provide this as an example of what people can do
	 * and use a default non-optional fallback.
	 */
	const struct sysdata_file_desc sysdata_desc = {
		SYSDATA_DEFAULT_SYNC(sync_found_cb, test_dev),
	};

	if (config->async)
		dev_info(test_dev->dev,
			 "loading default fallback '%s' using sync request now\n",
			 config->default_name);
	else
		dev_info(test_dev->dev,
			 "loading default fallback '%s'\n",
			config->default_name);

	rc = sysdata_file_request(config->default_name,
				  &sysdata_desc, test_dev->dev);
	if (rc)
		dev_info(test_dev->dev,
			 "load of default '%s' failed: %d\n",
			 config->default_name, rc);

	return rc;
}

/*
 * This is the default sync fallback callback, as a fallback this
 * then uses a sync request.
 */
static int config_sync_req_default_cb(void *context)
{
	struct sysdata_test_device *test_dev = context;
	int rc;

	rc = config_req_default(test_dev);

	return rc;

	/* Leave all the error checking for the main caller */
}

/*
 * This is the default config->async fallback callback, as a fallback this
 * then uses a sync request.
 */
static void config_async_req_default_cb(void *context)
{
	struct sysdata_test_device *test_dev = context;

	config_req_default(test_dev);

	/* Leave all the error checking for the main caller */
}

static int config_sync_req_cb(void *context,
			      const struct sysdata_file *sysdata)
{
	struct sysdata_test_device *test_dev = context;

	return config_load_data(test_dev, sysdata);
}

static int trigger_config_sync(struct sysdata_test_device *test_dev)
{
	struct test_config *config = &test_dev->config;
	int rc;
	const struct sysdata_file_desc sysdata_desc_default = {
		SYSDATA_DEFAULT_SYNC(config_sync_req_cb, test_dev),
		.optional = config->optional,
		.keep = config->keep,
	};
	const struct sysdata_file_desc sysdata_desc_opt_cb = {
		SYSDATA_DEFAULT_SYNC(config_sync_req_cb, test_dev),
		SYSDATA_SYNC_OPT_CB(config_sync_req_default_cb, test_dev),
		.optional = config->optional,
		.keep = config->keep,
	};
	const struct sysdata_file_desc *sysdata_desc;

	if (config->enable_opt_cb)
		sysdata_desc = &sysdata_desc_opt_cb;
	else
		sysdata_desc = &sysdata_desc_default;

	rc = sysdata_file_request(config->name, sysdata_desc, test_dev->dev);
	if (rc)
		dev_err(test_dev->dev, "sync load of '%s' failed: %d\n",
			config->name, rc);

	return rc;
}

static void config_async_req_cb(const struct sysdata_file *sysdata,
				void *context)
{
	struct sysdata_test_device *test_dev = context;
	config_load_data(test_dev, sysdata);
}

static int trigger_config_async(struct sysdata_test_device *test_dev)
{
	struct test_config *config = &test_dev->config;
	int rc;
	const struct sysdata_file_desc sysdata_desc_default = {
		SYSDATA_DEFAULT_ASYNC(config_async_req_cb, test_dev),
		.sync_reqs.mode = config->async ?
			SYNCDATA_ASYNC : SYNCDATA_SYNC,
		.optional = config->optional,
		.keep = config->keep,
	};
	const struct sysdata_file_desc sysdata_desc_opt_cb = {
		SYSDATA_DEFAULT_ASYNC(config_async_req_cb, test_dev),
		SYSDATA_ASYNC_OPT_CB(config_async_req_default_cb, test_dev),
		.sync_reqs.mode = config->async ?
			SYNCDATA_ASYNC : SYNCDATA_SYNC,
		.optional = config->optional,
		.keep = config->keep,
	};
	const struct sysdata_file_desc *sysdata_desc;
	async_cookie_t async_cookie;

	if (config->enable_opt_cb)
		sysdata_desc = &sysdata_desc_opt_cb;
	else
		sysdata_desc = &sysdata_desc_default;

	rc = sysdata_file_request_async(config->name, sysdata_desc,
					test_dev->dev,
					&async_cookie);
	if (rc) {
		dev_err(test_dev->dev, "async load of '%s' failed: %d\n",
			config->name, rc);
		goto out;
	}

	sysdata_synchronize_request(async_cookie);
out:
	return rc;
}

static ssize_t
trigger_config_store(struct device *dev,
		     struct device_attribute *attr,
		     const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_sysdata_private *test_sysdata = &test_dev->test_sysdata;
	struct test_config *config = &test_dev->config;
	int rc;

	mutex_lock(&test_dev->trigger_mutex);
	mutex_lock(&test_dev->config_mutex);

	dev_info(dev, "loading '%s'\n", config->name);

	if (config->async)
		rc = trigger_config_async(test_dev);
	else
		rc = trigger_config_sync(test_dev);

	config->test_result = rc;

	if (rc)
		goto out;

	if (test_sysdata->written) {
		dev_info(dev, "loaded: %zu\n", test_sysdata->size);
		rc = count;
	} else {
		dev_err(dev, "failed to load firmware\n");
		rc = -ENODEV;
	}

out:
	mutex_unlock(&test_dev->config_mutex);
	mutex_unlock(&test_dev->trigger_mutex);

	return rc;
}
static DEVICE_ATTR_WO(trigger_config);

/*
 * XXX: move to kstrncpy() once merged.
 *
 * Users should use kfree_const() when freeing these.
 */
static int __kstrncpy(char **dst, const char *name, size_t count, gfp_t gfp)
{
	*dst = kstrndup(name, count, gfp);
	if (!*dst)
		return -ENOSPC;
	return count;
}

static int config_copy_name(struct test_config *config,
			    const char *name,
			    size_t count)
{
	return __kstrncpy(&config->name, name, count, GFP_KERNEL);
}

static int config_copy_default_name(struct test_config *config,
				    const char *name,
				    size_t count)
{
	return __kstrncpy(&config->default_name, name, count, GFP_KERNEL);
}

static void __sysdata_config_free(struct test_config *config)
{
	kfree_const(config->name);
	config->name = NULL;
	kfree_const(config->default_name);
	config->default_name = NULL;
}

static void sysdata_config_free(struct sysdata_test_device *test_dev)
{
	struct test_config *config = &test_dev->config;

	mutex_lock(&test_dev->config_mutex);
	__sysdata_config_free(config);
	mutex_unlock(&test_dev->config_mutex);
}

static int __sysdata_config_init(struct test_config *config)
{
	int ret;

	ret = config_copy_name(config, TEST_SYSDATA, strlen(TEST_SYSDATA));
	if (ret < 0)
		goto out;

	ret = config_copy_default_name(config, TEST_SYSDATA,
				       strlen(TEST_SYSDATA));
	if (ret < 0)
		goto out;

	config->async = false;
	config->optional = false;
	config->keep = false;
	config->enable_opt_cb = false;
	config->test_result = 0;

out:
	return ret;
}

int sysdata_config_init(struct sysdata_test_device *test_dev)
{
	struct test_config *config = &test_dev->config;
	int ret;

	mutex_lock(&test_dev->config_mutex);
	ret = __sysdata_config_init(config);
	mutex_unlock(&test_dev->config_mutex);

	return ret;
}

static ssize_t config_name_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;
	int rc;

	mutex_lock(&test_dev->config_mutex);
	rc = config_copy_name(config, buf, count);
	mutex_unlock(&test_dev->config_mutex);

	return rc;
}

static ssize_t config_name_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	mutex_lock(&test_dev->config_mutex);
	strcpy(buf, config->name);
	strcat(buf, "\n");
	mutex_unlock(&test_dev->config_mutex);

	return strlen(buf) + 1;
}
static DEVICE_ATTR(config_name, 0644, config_name_show, config_name_store);

static ssize_t config_default_name_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;
	int rc;

	mutex_lock(&test_dev->config_mutex);
	rc = config_copy_default_name(config, buf, count);
	mutex_unlock(&test_dev->config_mutex);

	return rc;
}

static ssize_t config_default_name_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	mutex_lock(&test_dev->config_mutex);
	strcpy(buf, config->default_name);
	strcat(buf, "\n");
	mutex_unlock(&test_dev->config_mutex);

	return strlen(buf) + 1;
}
static DEVICE_ATTR(config_default_name, 0644, config_default_name_show,
		   config_default_name_store);

static ssize_t reset_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;
	int ret;

	mutex_lock(&test_dev->trigger_mutex);

	mutex_lock(&test_dev->sysdata_mutex);
	free_test_sysdata(&test_dev->test_sysdata);
	mutex_unlock(&test_dev->sysdata_mutex);

	mutex_lock(&test_dev->config_mutex);

	__sysdata_config_free(config);

	ret = __sysdata_config_init(config);
	if (ret < 0) {
		ret = -ENOMEM;
		dev_err(dev, "could not alloc settings for config trigger: %d\n",
		       ret);
		goto out;
	}

	dev_info(dev, "reset\n");
	ret = count;

out:
	mutex_unlock(&test_dev->config_mutex);
	mutex_unlock(&test_dev->trigger_mutex);

	return ret;
}
static DEVICE_ATTR_WO(reset);

/*
 * XXX: consider a soluton to generalize drivers to specify their own
 * mutex, adding it to dev core after this gets merged. This may not
 * be important for once-in-a-while system tuning parameters, but if
 * we want to enable fuzz testing, this is really important.
 *
 * It may make sense to just have a "struct device configuration mutex"
 * for these sorts of things, although there is difficulty in that we'd
 * need dynamically allocated attributes for that. Its the same reasn
 * why we ended up not using the provided standard device attribute
 * bool, int interfaces.
 */

static int test_dev_config_update_bool(struct sysdata_test_device *test_dev,
				       const char *buf, size_t size,
				       bool *config)
{
	int ret;

	mutex_lock(&test_dev->config_mutex);
	if (strtobool(buf, config) < 0)
		ret = -EINVAL;
	else
		ret = size;
	mutex_unlock(&test_dev->config_mutex);

	return ret;
}

static ssize_t test_dev_config_show_bool(struct sysdata_test_device *test_dev,
					 char *buf,
					 bool config)
{
	bool val;

	//mutex_lock(&test_dev->config_mutex);
	val = config;
	//mutex_unlock(&test_dev->config_mutex);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static int test_dev_config_update_int(struct sysdata_test_device *test_dev,
				      const char *buf, size_t size,
				      int *config)
{
	char *end;
	long new = simple_strtol(buf, &end, 0);
	if (end == buf || new > INT_MAX || new < INT_MIN)
		return -EINVAL;
	mutex_lock(&test_dev->config_mutex);
	*(int *)config = new;
	mutex_unlock(&test_dev->config_mutex);
	/* Always return full write size even if we didn't consume all */
	return size;
}

static ssize_t test_dev_config_show_int(struct sysdata_test_device *test_dev,
					char *buf,
					int config)
{
	int val;

	mutex_lock(&test_dev->config_mutex);
	val = config;
	mutex_unlock(&test_dev->config_mutex);

	return snprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t config_async_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_update_bool(test_dev, buf, count,
					   &config->async);
}

static ssize_t config_async_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_show_bool(test_dev, buf, config->async);
}
static DEVICE_ATTR(config_async, 0644, config_async_show, config_async_store);

static ssize_t config_optional_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_update_bool(test_dev, buf, count,
					   &config->optional);
}

static ssize_t config_optional_show(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_show_bool(test_dev, buf, config->optional);
}
static DEVICE_ATTR(config_optional, 0644, config_optional_show,
		   config_optional_store);

static ssize_t config_keep_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_update_bool(test_dev, buf, count,
					   &config->keep);
}

static ssize_t config_keep_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_show_bool(test_dev, buf, config->keep);
}
static DEVICE_ATTR(config_keep, 0644, config_keep_show, config_keep_store);

static ssize_t config_enable_opt_cb_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_update_bool(test_dev, buf, count,
					   &config->enable_opt_cb);
}

static ssize_t config_enable_opt_cb_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_show_bool(test_dev, buf,
					 config->enable_opt_cb);
}
static DEVICE_ATTR(config_enable_opt_cb, 0644,
		   config_enable_opt_cb_show,
		   config_enable_opt_cb_store);

static ssize_t test_result_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_update_int(test_dev, buf, count,
					  &config->test_result);
}

static ssize_t test_result_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sysdata_test_device *test_dev = dev_to_test_dev(dev);
	struct test_config *config = &test_dev->config;

	return test_dev_config_show_int(test_dev, buf, config->test_result);
}
static DEVICE_ATTR(test_result, 0644, test_result_show, test_result_store);

#define SYSDATA_DEV_ATTR(name)		&dev_attr_##name.attr

static struct attribute *test_dev_attrs[] = {
	SYSDATA_DEV_ATTR(trigger_request),
	SYSDATA_DEV_ATTR(trigger_request_keep),
	SYSDATA_DEV_ATTR(trigger_request_opt),
	SYSDATA_DEV_ATTR(trigger_request_opt_default),

	SYSDATA_DEV_ATTR(trigger_async_request),
	SYSDATA_DEV_ATTR(trigger_async_request_keep),
	SYSDATA_DEV_ATTR(trigger_async_request_opt),
	SYSDATA_DEV_ATTR(trigger_async_request_opt_default),

	SYSDATA_DEV_ATTR(trigger_config),
	SYSDATA_DEV_ATTR(config),
	SYSDATA_DEV_ATTR(reset),

	SYSDATA_DEV_ATTR(config_name),
	SYSDATA_DEV_ATTR(config_default_name),
	SYSDATA_DEV_ATTR(config_async),
	SYSDATA_DEV_ATTR(config_optional),
	SYSDATA_DEV_ATTR(config_keep),
	SYSDATA_DEV_ATTR(config_enable_opt_cb),
	SYSDATA_DEV_ATTR(test_result),

	NULL,
};

ATTRIBUTE_GROUPS(test_dev);


/*
 * XXX: this could perhaps be made generic already too, but a hunt
 * for actual users would be needed first. It could be generic
 * if other test drivers end up using a similar mechanism.
 */
const char *test_dev_get_name(const char *base, int idx, gfp_t gfp)
{
	const char *name_const;
	char *name;

	if (!base)
		return NULL;
	if (strlen(base) > 30)
		return NULL;
	name = kzalloc(1024, gfp);
	if (!name)
		return NULL;

	strncat(name, base, strlen(base));
	sprintf(name+(strlen(base)), "%d", idx);
	name_const = kstrdup_const(name, gfp);

	kfree(name);

	return name_const;
}

void free_test_dev_sysdata(struct sysdata_test_device *test_dev)
{
	kfree_const(test_dev->misc_dev.name);
	test_dev->misc_dev.name = NULL;
	//kfree(test_dev);
	vfree(test_dev);
	test_dev = NULL;
	sysdata_config_free(test_dev);
}

void unregister_test_dev_sysdata(struct sysdata_test_device *test_dev)
{
	dev_info(test_dev->dev, "removing interface\n");
	misc_deregister(&test_dev->misc_dev);
	free_test_dev_sysdata(test_dev);
}

struct sysdata_test_device *alloc_test_dev_sysdata(int idx)
{
	int rc;
	struct sysdata_test_device *test_dev;
	struct miscdevice *misc_dev;

	//test_dev = kzalloc(GFP_KERNEL, sizeof(struct sysdata_test_device));
	test_dev = vmalloc(sizeof(struct sysdata_test_device));
	if (!test_dev) {
		pr_err("Cannot alloc test_dev\n");
		goto err_out;
	}

	mutex_init(&test_dev->sysdata_mutex);
	mutex_init(&test_dev->config_mutex);
	mutex_init(&test_dev->trigger_mutex);

	rc = sysdata_config_init(test_dev);
	if (rc < 0) {
		pr_err("Cannot alloc sysdata_config_init()\n");
		goto err_out_free;
	}

	test_dev->dev_idx = idx;
	misc_dev = &test_dev->misc_dev;

	misc_dev->minor = MISC_DYNAMIC_MINOR;
	misc_dev->name = test_dev_get_name("test_sysdata", test_dev->dev_idx,
					   GFP_KERNEL);
	if (!misc_dev->name) {
		pr_err("Cannot alloc misc_dev->name\n");
		goto err_out_free_config;
	}
	misc_dev->fops = &test_fw_fops;
	misc_dev->groups = test_dev_groups;

	return test_dev;

err_out_free_config:
	__sysdata_config_free(&test_dev->config);
err_out_free:
	kfree(test_dev);
err_out:
	return NULL;
}

static int register_test_dev_sysdata(void)
{
	struct sysdata_test_device *test_dev = NULL;
	int rc = -ENODEV;

	mutex_lock(&reg_dev_mutex);

	/* int should suffice for number of devices, test for wrap */
	if (unlikely(num_test_devs + 1) < 0) {
		pr_err("reached limit of number of test devices\n");
		goto out;
	}

	test_dev = alloc_test_dev_sysdata(num_test_devs);
	if (!test_dev) {
		rc = -ENOMEM;
		goto out;
	}

	rc = misc_register(&test_dev->misc_dev);
	if (rc) {
		pr_err("could not register misc device: %d\n", rc);
		free_test_dev_sysdata(test_dev);
		return rc;
	}

	test_dev->dev = test_dev->misc_dev.this_device;
	list_add_tail(&test_dev->list, &reg_test_devs);
	dev_info(test_dev->dev, "interface ready\n");

	num_test_devs++;

	mutex_unlock(&reg_dev_mutex);

out:
	return rc;
}

static void do_init_work(struct work_struct *work);
static DECLARE_WORK(init_work, do_init_work);

static void do_init_work(struct work_struct *work)
{
	int rc;
	ssleep(3);
	rc = register_test_dev_sysdata();
	if (rc)
		pr_err("Cannot add first test sysdata device\n");
}

/* Let's not clobber init large allocations */
static int __init test_sysdata_init(void)
{
	schedule_work(&init_work);
	return 0;
}

module_init(test_sysdata_init);

static void __exit test_sysdata_exit(void)
{
	struct sysdata_test_device *test_dev, *tmp;

	mutex_lock(&reg_dev_mutex);
	list_for_each_entry_safe(test_dev, tmp, &reg_test_devs, list) {
		list_del(&test_dev->list);
		unregister_test_dev_sysdata(test_dev);
	}
	mutex_unlock(&reg_dev_mutex);
}

module_exit(test_sysdata_exit);

MODULE_AUTHOR("Luis R. Rodriguez <mcgrof@kernel.org>");
MODULE_LICENSE("GPL");
