/*
 * You can use this to help convert a device driver from the old firmware API
 * to the new flexible sysdata API for sync and sync mechanisms. The
 * sync conversion requires more work given tha the prior API did not
 * have a callback, we add it.
 *
 * The confidence of this SmPL patch is low given the amount of work
 * required for the full conversion on the sync mechanism. This SmPL
 * patch however should do most of the required work for the conversion.
 *
 * Confidence: Low
 *
 * Copyright: (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org> GPLv2.
 */

@ sync_get_t @
type T_priv;
T_priv *drv_priv;
expression name, dev;
identifier ret, fw_entry;
@@

(
request_firmware(&drv_priv->fw_entry, name, dev);
|
ret = request_firmware(&drv_priv->fw_entry, name, dev);
|
return request_firmware(&drv_priv->fw_entry, name, dev);
)

@ find_request_sync @
expression name, dev;
identifier ret, fw_entry, f, drv;
@@

f (...)
{
...
(
-request_firmware(&drv->fw_entry, name, dev);
+sysdata_file_request(name, &sysdata_desc, dev);
|
-ret = request_firmware(&drv->fw_entry, name, dev);
+ret = sysdata_file_request(name, &sysdata_desc, dev);
|
-return request_firmware(&drv->fw_entry, name, dev);
+return sysdata_file_request(name, &sysdata_desc, dev);
)
...
}

@ add_desc_sync extends find_request_sync @
type T;
fresh identifier sync_found_cb = f ## "_found_cb";
@@

f (...)
{
...
T *drv;
+const struct sysdata_file_desc sysdata_desc = {
+	SYSDATA_DEFAULT_SYNC(sync_found_cb, drv),
+};
...
}

@ add_desc_direct_sync extends find_request_sync @
type T;
fresh identifier sync_found_cb = f ## "_found_cb";
@@

f (...,
   T *drv
   ,...)
{
+const struct sysdata_file_desc sysdata_desc = {
+	SYSDATA_DEFAULT_SYNC(sync_found_cb, drv),
+};
...
}

@ peg_section depends on find_request_sync @
identifier find_request_sync.ret;
@@

{
...
ret = sysdata_file_request(...);
+if (true) {
...
+}
+return ret;
}

@ add_cb_sync depends on find_request_sync @
identifier find_request_sync.f;
fresh identifier sync_found_cb = f ## "_found_cb";
statement S1;
symbol context, sysdata, true;
identifier find_request_sync.ret;
@@

+static int sync_found_cb(void *context, const struct sysdata_file *sysdata)
+{
+if (true) { S1 }
+}

f (...) {
...
-if (true ) { S1 }
return ret;
}

@ sanitize_braces depends on add_cb_sync @
type sync_get_t.T_priv;
identifier find_request_sync.ret;
identifier add_cb_sync.sync_found_cb;
identifier find_request_sync.drv;
@@

sync_found_cb(void *context, const struct sysdata_file *sysdata)
{
+T_priv *drv = context;
+int ret;
+
-if (true) {{
...
-}}
}

/*
 * This deals with the async mechansims
 */

@ async_get_t @
expression name, dev, uevent;
identifier drv_callback, ret;
type T;
T *drv;
@@

(
request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
|
ret = request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
|
return request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
)

@ find_request_async @
expression name, dev, uevent;
identifier drv_callback, ret, drv, f;
@@

f (...)
{
...
(
-request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
+sysdata_file_request_async(name, &sysdata_desc, dev, &drv->fw_async_cookie);
|
ret = 
-request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
+sysdata_file_request_async(name, &sysdata_desc, dev, &drv->fw_async_cookie);
|
return
-request_firmware_nowait(THIS_MODULE, uevent, name, dev, GFP_KERNEL, drv, drv_callback);
+sysdata_file_request_async(name, &sysdata_desc, dev, &drv->fw_async_cookie);
)
...
}

@ add_desc_async extends find_request_async @
type T;
@@

f (...)
{
...
T *drv;
+const struct sysdata_file_desc sysdata_desc = {
+	SYSDATA_DEFAULT_ASYNC(drv_callback, drv),
+};
...
}

@ add_desc_direct_async extends find_request_async @
type T;
@@

f (...,
   T *drv
   ,...)
{
+const struct sysdata_file_desc sysdata_desc = {
+	SYSDATA_DEFAULT_ASYNC(drv_callback, drv),
+};
...
}

@ found_callback extends find_request_async @
identifier data, cmpl;
type T1;
@@

 drv_callback(
-const struct firmware *data,
+const struct sysdata_file *data,
 void *context)
 {
	...
	T1 *drv = context;
	...
(
-	complete(&drv->cmpl);
|
-	complete_all(&drv->cmpl);
)
	...
 }

@ drop_init_completion extends find_request_async @
identifier found_callback.cmpl;
@@

-init_completion(&drv->cmpl);

@ replace_completion_wait extends found_callback @
@@

-wait_for_completion(&drv->cmpl);
+sysdata_synchronize_request(drv->fw_async_cookie);

@ modify_drv depends on async_get_t @
type async_get_t.T;
typedef async_cookie_t;
identifier found_callback.cmpl;
@@

T {
	...
-	struct completion cmpl;
+	async_cookie_t fw_async_cookie;
	...
};

@ modify_drv2 depends on found_callback @
type found_callback.T1;
identifier found_callback.cmpl;
@@

T1 {
	...
-	struct completion cmpl;
+	async_cookie_t fw_async_cookie;
	...
};

@ modify_drv3 depends on add_desc_async && !(modify_drv || modify_drv2 )@
type add_desc_async.T;
@@

T {
	...
+	async_cookie_t fw_async_cookie;
};

@ modify_drv_direct extends add_desc_direct_async depends on !found_callback @
@@

T {
	...
+	async_cookie_t fw_async_cookie;
};

/* 
 * These are shared rules
 */

@ use_new_struct @
identifier consumer, data;
@@

consumer(...,
-	const struct firmware *data
+	const struct sysdata_file *data
	,...)
{
...
}

@ modify_decl @
type T2;
identifier consumer, data;
@@

T2 consumer(...,
-	const struct firmware *data
+	const struct sysdata_file *data
	,...);


@ drop_fw_release_goto depends on find_request_sync || find_request_async @
identifier out, some_fn;
@@

void some_fn (...) {
<+...
- goto out;
+ return;
...+>
-out:
-release_firmware(...);
}

@ drop_fw_release depends on find_request_sync || find_request_async @
@@

-release_firmware(...);

@ change_headers @
@@

-#include <linux/firmware.h>
+#include <linux/sysdata.h>

@ replace_struct_last_resort @
type T;
identifier data;
@@

T {
	...
-	const struct firmware *data;
+	const struct sysdata_file *data;
	...
};

