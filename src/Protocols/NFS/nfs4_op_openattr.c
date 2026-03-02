// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file    nfs4_op_openattr.c
 * @brief   Implementation of NFS4_OP_OPENATTR.
 *
 * Opens the named attribute directory for the current filehandle.
 * The resulting current FH is a synthetic xattr directory handle
 * (same fsopaque as parent, with FILE_HANDLE_V4_FLAG_XATTR_DIR set).
 */

#include "config.h"
#include "hashtable.h"
#include "log.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "nfs_proto_functions.h"
#include "nfs_file_handle.h"
#include "fsal_convert.h"

/**
 * @brief NFS4_OP_OPENATTR
 *
 * Opens the named attribute directory associated with the current
 * filehandle object. On success, the current FH is replaced with
 * a synthetic xattr directory handle.
 *
 * @param[in]     op   Arguments for nfs4_op
 * @param[in,out] data Compound request's data
 * @param[out]    resp Results for nfs4_op
 *
 * @return per RFC5661, pp. 370-1
 */
enum nfs_req_result nfs4_op_openattr(struct nfs_argop4 *op,
				     compound_data_t *data,
				     struct nfs_resop4 *resp)
{
	OPENATTR4args *const arg_OPENATTR4 = &op->nfs_argop4_u.opopenattr;
	OPENATTR4res *const res_OPENATTR4 = &resp->nfs_resop4_u.opopenattr;
	struct file_handle_v4 *v4_handle;

	resp->resop = NFS4_OP_OPENATTR;
	res_OPENATTR4->status = NFS4_OK;

	/* Sanity check: we need a valid current FH that is not a DS handle */
	res_OPENATTR4->status =
		nfs4_sanity_check_FH(data, NO_FILE_TYPE, false);
	if (res_OPENATTR4->status != NFS4_OK)
		return NFS_REQ_ERROR;

	/* Reject if current FH is already an xattr handle */
	if (nfs4_Is_Fh_Xattr(&data->currentFH)) {
		LogDebug(COMPONENT_NFS_V4,
			 "OPENATTR on xattr handle not allowed");
		res_OPENATTR4->status = NFS4ERR_WRONG_TYPE;
		return NFS_REQ_ERROR;
	}

	/* Only regular files and directories may have named attributes */
	if (data->current_filetype != REGULAR_FILE &&
	    data->current_filetype != DIRECTORY) {
		LogDebug(COMPONENT_NFS_V4,
			 "OPENATTR on type %s not allowed",
			 object_file_type_to_str(data->current_filetype));
		res_OPENATTR4->status = NFS4ERR_WRONG_TYPE;
		return NFS_REQ_ERROR;
	}

	/*
	 * createdir (arg_OPENATTR4->createdir) is acknowledged but we
	 * don't need to do anything special -- xattrs are always available
	 * on VFS if the filesystem supports them. If the client asks us
	 * not to create and there happen to be no xattrs, that's fine;
	 * READDIR on the xattr dir will simply return empty.
	 */
	(void)arg_OPENATTR4;

	/* Set the xattr directory flag on the current file handle */
	v4_handle = (struct file_handle_v4 *)data->currentFH.nfs_fh4_val;
	v4_handle->fhflags1 |= FILE_HANDLE_V4_FLAG_XATTR_DIR;

	/* Override filetype to DIRECTORY for the xattr directory */
	data->current_filetype = DIRECTORY;

	LogDebug(COMPONENT_NFS_V4,
		 "OPENATTR success, fhflags1=0x%02X",
		 v4_handle->fhflags1);

	return NFS_REQ_OK;
} /* nfs4_op_openattr */

/**
 * @brief Free memory allocated for OPENATTR result
 *
 * This function frees any memory allocated for the result of the
 * NFS4_OP_OPENATTR operation.
 *
 * @param[in,out] resp nfs4_op results
 */
void nfs4_op_openattr_Free(nfs_resop4 *resp)
{
	/* Nothing to be done */
}
