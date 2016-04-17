/*
 * You can use this to convert a device driver from the old firmware API
 * to the new flexible sysdata API.
 *
 * For now the confidence is low as this is a new SmPL patch, however with
 * time, as more drivers get converted and this is fine tuned, this
 * confidence in it may grow higher.
 *
 * Confidence: Medium
 *
 * Copyright: (C) 2016 Luis R. Rodriguez <mcgrof@kernel.org> GPLv2.
 */

@ change_headers @
@@

-#include <linux/firmware.h>
+#include <linux/sysdata.h>


@ find_request_fw_async @
expression name, dev;
identifier drv, drv_callback, f;
@@

f (...) {
+const struct sysdata_file_desc sysdata_desc= {
+	SYSDATA_DEFAULT_ASYNC(drv_callback, drv),
+};
...
(
-request_firmware_nowait(THIS_MODULE, 1, name, dev, GFP_KERNEL, drv, drv_callback);
+sysdata_file_request_async(name, &sysdata_desc, dev, &drv->fw_async_cookie);
|
-return request_firmware_nowait(THIS_MODULE, 1, name, dev, GFP_KERNEL, drv, drv_callback);
+return sysdata_file_request_async(name, &sysdata_desc, dev, &drv->fw_async_cookie);
)
...
}

@ found_callback depends on find_request_fw_async @
type T;
T *drv_priv;
identifier data, context, cmpl;
identifier find_request_fw_async.drv_callback;
@@

 drv_callback(
-const struct firmware *data,
+const struct sysdata_file *data,
 void *context)
 {
	<+...
-	complete(&drv_priv->cmpl);
	...+>
 }

@ drop_init_completion depends on found_callback@
type found_callback.T;
T *drv_priv;
identifier found_callback.cmpl;
@@

-init_completion(&drv_priv->cmpl);

@ replace_completion_wait depends on found_callback@
type found_callback.T;
T *drv_priv;
identifier found_callback.cmpl;
@@

-wait_for_completion(&drv_priv->cmpl);
+sysdata_synchronize_request(drv->fw_async_cookie);

@ modify_drv depends on found_callback @
type found_callback.T;
identifier found_callback.cmpl;
@@

T {
	...
-	struct completion cmpl;
+	async_cookie_t fw_async_cookie;
	...
};

@ use_new_struct @
identifier fn, data;
@@

fn (...,
-const struct firmware *data,
+const struct sysdata_file *data,
...) { ... }

@ drop_fw_release @
@@

-release_firmware(...);

@ find_request_fw_sync @
expression name, dev;
identifier ret, fw_entry, drv, f;
statement S;
@@

+static int sync_found_cb(void *context, const struct sysdata_file *sysdata)
+{
+}

f (...) {
+const struct sysdata_file_desc sysdata_desc = {
+	SYSDATA_DEFAULT_SYNC(sync_found_cb, NULL),
+};
...
(
-request_firmware(&fw_entry, name, dev);
+sysdata_file_request(name, &sysdata_desc, dev);
|
ret =
-request_firmware(&fw_entry, name, dev);
+sysdata_file_request(name, &sysdata_desc, dev);
)
...
}

