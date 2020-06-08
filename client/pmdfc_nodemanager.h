/* SPDX-License-Identifier: GPL-2.0-or-later */
/* -*- mode: c; c-basic-offset: 8; -*-
 * vim: noexpandtab sw=8 ts=8 sts=0:
 *
 * pmdfc_nodemanager.h
 *
 * Header describing the interface between userspace and the kernel
 * for the pmdfc_nodemanager module.
 *
 * Copyright (C) 2002, 2004 Oracle.  All rights reserved.
 */

#ifndef _PMDFC_NODEMANAGER_H
#define _PMDFC_NODEMANAGER_H

#define PMNM_API_VERSION	5

#define PMNM_MAX_NODES		255
#define PMNM_INVALID_NODE_NUM	255

/* host name, group name, cluster name all 64 bytes */
#define PMNM_MAX_NAME_LEN        64    // __NEW_UTS_LEN

/*
 * Maximum number of global heartbeat regions allowed.
 * **CAUTION**  Changing this number will break dlm compatibility.
 */
#define PMNM_MAX_REGIONS	32

#endif /* _pmdfc_NODEMANAGER_H */
