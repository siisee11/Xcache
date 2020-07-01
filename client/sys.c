// SPDX-License-Identifier: GPL-2.0-only
/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * sys.c
 *
 * OCFS2 cluster sysfs interface
 *
 * Copyright (C) 2005 Oracle.  All rights reserved.
 * Copyright (C) 2020 JY N.  All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>

#include "pmdfc_nodemanager.h"
#include "masklog.h"
#include "sys.h"


static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", PMNM_API_VERSION);
}
static struct kobj_attribute attr_version =
	__ATTR(interface_revision, S_IRUGO, version_show, NULL);

static struct attribute *pmcb_attrs[] = {
	&attr_version.attr,
	NULL,
};

static struct attribute_group pmcb_attr_group = {
	.attrs = pmcb_attrs,
};

static struct kset *pmcb_kset;

void pmcb_sys_shutdown(void)
{
	mlog_sys_shutdown();
	kset_unregister(pmcb_kset);
}

int pmcb_sys_init(void)
{
	int ret;

	pmcb_kset = kset_create_and_add("pmcb", NULL, fs_kobj);
	if (!pmcb_kset)
		return -ENOMEM;

	ret = sysfs_create_group(&pmcb_kset->kobj, &pmcb_attr_group);
	if (ret)
		goto error;

	ret = mlog_sys_init(pmcb_kset);
	if (ret)
		goto error;
	return 0;
error:
	kset_unregister(pmcb_kset);
	return ret;
}
