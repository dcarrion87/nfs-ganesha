// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (c) 2024
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
 * @file nfs4_xattr_handle_ops.c
 * @brief NFSv4 OPENATTR xattr handle operation dispatch.
 *
 * Implements NFS4 operations (READDIR, LOOKUP, GETATTR, READ, WRITE,
 * OPEN, CLOSE, REMOVE) when the current filehandle has xattr flags set.
 * These bridge NFSv4 named attributes (OPENATTR) to the FSAL xattr
 * interface, allowing macOS clients to use namedattr mounts.
 */

#include "config.h"
#include <string.h>
#include "log.h"
#include "gsh_rpc.h"
#include "fsal.h"
#include "nfs4.h"
#include "nfs_core.h"
#include "nfs_exports.h"
#include "nfs_proto_functions.h"
#include "nfs_proto_tools.h"
#include "nfs_file_handle.h"
#include "nfs_convert.h"
#include "sal_functions.h"
#include "export_mgr.h"
#include "nfs4_xattr_handle_ops.h"

/* Maximum xattr value size for read/write ops */
#define XATTR_MAX_VALUE_SIZE (64 * 1024)

/* Release callback for xattr read data buffers */
static void xattr_read_data_release(void *data)
{
	gsh_free(data);
}

/* FNV-1a hash for fileid generation */
static uint64_t fnv1a_hash(const char *data, size_t len)
{
	uint64_t hash = 14695981039346656037ULL;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= (uint8_t)data[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

/**
 * @brief Build an xattr object file handle from a parent xattr-dir handle.
 *
 * Layout: [parent_fsopaque][xattr_name (name_len bytes)][name_len: 1 byte]
 *
 * The caller passes the current xattr-dir handle (in data->currentFH).
 * We copy it, append the xattr name + length byte, set the xattr obj flag,
 * and write the result into dst_fh.
 */
static nfsstat4 nfs4_xattr_build_entry_fh(compound_data_t *data,
					   const char *name, uint8_t name_len,
					   nfs_fh4 *dst_fh)
{
	struct file_handle_v4 *src_v4 =
		(struct file_handle_v4 *)data->currentFH.nfs_fh4_val;
	struct file_handle_v4 *dst_v4;
	size_t new_fs_len;
	size_t new_handle_size;

	new_fs_len = src_v4->fs_len + name_len + 1;
	new_handle_size =
		offsetof(struct file_handle_v4, fsopaque) + new_fs_len;

	if (new_handle_size > NFS4_FHSIZE) {
		LogInfo(COMPONENT_NFS_V4,
			"xattr obj handle too large: %zu > %d",
			new_handle_size, NFS4_FHSIZE);
		return NFS4ERR_FBIG;
	}

	/* Copy the base handle into dst */
	memcpy(dst_fh->nfs_fh4_val, data->currentFH.nfs_fh4_val,
	       nfs4_sizeof_handle(src_v4));

	dst_v4 = (struct file_handle_v4 *)dst_fh->nfs_fh4_val;

	/* Append xattr name and length byte */
	memcpy(&dst_v4->fsopaque[src_v4->fs_len], name, name_len);
	dst_v4->fsopaque[src_v4->fs_len + name_len] = name_len;
	dst_v4->fs_len = new_fs_len;

	/* Change flag from xattr dir to xattr obj */
	dst_v4->fhflags1 &= ~FILE_HANDLE_V4_FLAG_XATTR_DIR;
	dst_v4->fhflags1 |= FILE_HANDLE_V4_FLAG_XATTR_OBJ;

	dst_fh->nfs_fh4_len = new_handle_size;

	return NFS4_OK;
}

/**
 * @brief LOOKUP in an xattr directory.
 *
 * Verifies the named xattr exists on the parent object, then builds
 * an xattr-obj handle and sets it as the current FH.
 */
enum nfs_req_result nfs4_op_lookup_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp)
{
	LOOKUP4args *const arg_LOOKUP4 = &op->nfs_argop4_u.oplookup;
	LOOKUP4res *const res_LOOKUP4 = &resp->nfs_resop4_u.oplookup;
	struct fsal_obj_handle *obj = data->current_obj;
	xattrkey4 xa_name;
	xattrvalue4 xa_value;
	fsal_status_t fsal_status;
	char val_fh[NFS4_FHSIZE];
	nfs_fh4 entry_fh = { .nfs_fh4_len = NFS4_FHSIZE,
			      .nfs_fh4_val = val_fh };
	nfsstat4 status;

	resp->resop = NFS4_OP_LOOKUP;
	res_LOOKUP4->status = NFS4_OK;

	if (arg_LOOKUP4->objname.utf8string_len == 0 ||
	    arg_LOOKUP4->objname.utf8string_len > NAME_MAX) {
		res_LOOKUP4->status = NFS4ERR_NAMETOOLONG;
		return NFS_REQ_ERROR;
	}

	LogDebug(COMPONENT_NFS_V4, "LOOKUP xattr name=%.*s",
		 arg_LOOKUP4->objname.utf8string_len,
		 arg_LOOKUP4->objname.utf8string_val);

	/* Verify the xattr exists by doing a zero-length get (size probe) */
	xa_name.utf8string_len = arg_LOOKUP4->objname.utf8string_len;
	xa_name.utf8string_val = arg_LOOKUP4->objname.utf8string_val;
	xa_value.utf8string_len = 0;
	xa_value.utf8string_val = NULL;

	fsal_status = obj->obj_ops->getxattrs(obj, &xa_name, &xa_value);
	if (FSAL_IS_ERROR(fsal_status)) {
		if (fsal_status.major == ERR_FSAL_NOXATTR)
			res_LOOKUP4->status = NFS4ERR_NOENT;
		else
			res_LOOKUP4->status = nfs4_Errno_status(fsal_status);
		return NFS_REQ_ERROR;
	}
	/* Free any data returned from size probe */
	gsh_free(xa_value.utf8string_val);

	/* Build the xattr object handle */
	status = nfs4_xattr_build_entry_fh(
		data, arg_LOOKUP4->objname.utf8string_val,
		(uint8_t)arg_LOOKUP4->objname.utf8string_len, &entry_fh);
	if (status != NFS4_OK) {
		res_LOOKUP4->status = status;
		return NFS_REQ_ERROR;
	}

	/* Install the new handle as the current FH */
	memcpy(data->currentFH.nfs_fh4_val, entry_fh.nfs_fh4_val,
	       entry_fh.nfs_fh4_len);
	data->currentFH.nfs_fh4_len = entry_fh.nfs_fh4_len;

	/* xattr entries appear as regular files */
	data->current_filetype = REGULAR_FILE;

	return NFS_REQ_OK;
}

/**
 * @brief READDIR over the xattr directory.
 *
 * Lists xattrs on the parent object and returns them as directory entries
 * using the entry4 linked list approach.
 */
enum nfs_req_result nfs4_op_readdir_xattr(struct nfs_argop4 *op,
					  compound_data_t *data,
					  struct nfs_resop4 *resp)
{
	READDIR4args *const arg_READDIR4 = &op->nfs_argop4_u.opreaddir;
	READDIR4res *res_READDIR4 = &resp->nfs_resop4_u.opreaddir;
	READDIR4resok *resok = &res_READDIR4->READDIR4res_u.resok4;
	struct fsal_obj_handle *obj = data->current_obj;
	fsal_status_t fsal_status;
	xattrlist4 xattr_list = { 0 };
	nfs_cookie4 cookie = arg_READDIR4->cookie;
	bool_t lr_eof = false;
	uint32_t maxcount;
	uint32_t i;
	entry4 *entries = NULL;
	entry4 **tail = &entries;

	resp->resop = NFS4_OP_READDIR;
	res_READDIR4->status = NFS4_OK;

	LogDebug(COMPONENT_NFS_V4,
		 "READDIR xattr cookie=%" PRIu64 " maxcount=%u",
		 cookie, arg_READDIR4->maxcount);

	/* Cap maxcount */
	maxcount = arg_READDIR4->maxcount;
	if (maxcount > 65536)
		maxcount = 65536;

	LogDebug(COMPONENT_NFS_V4,
		 "READDIR xattr: calling listxattrs on obj %p (type=%d)",
		 obj, obj->type);

	/* List xattrs from the parent object */
	fsal_status = obj->obj_ops->listxattrs(obj, maxcount, &cookie,
					       &lr_eof, &xattr_list);
	if (FSAL_IS_ERROR(fsal_status)) {
		LogDebug(COMPONENT_NFS_V4,
			 "READDIR xattr: listxattrs failed %s",
			 msg_fsal_err(fsal_status.major));
		res_READDIR4->status = nfs4_Errno_status(fsal_status);
		return NFS_REQ_ERROR;
	}

	LogDebug(COMPONENT_NFS_V4,
		 "READDIR xattr: listxattrs returned %u entries, eof=%d",
		 xattr_list.xl4_count, lr_eof);

	/* Encode verifier (zeroed for xattr dir -- no real change tracking) */
	memset(resok->cookieverf, 0, sizeof(verifier4));

	/* Build entry4 linked list */
	for (i = 0; i < xattr_list.xl4_count; i++) {
		component4 *src_name = &xattr_list.xl4_entries[i];
		entry4 *ent = gsh_calloc(1, sizeof(entry4));

		ent->cookie = cookie + i + 1;
		ent->name.utf8string_len = src_name->utf8string_len;
		ent->name.utf8string_val =
			gsh_malloc(src_name->utf8string_len);
		memcpy(ent->name.utf8string_val, src_name->utf8string_val,
		       src_name->utf8string_len);

		/* Empty fattr4 -- no attributes requested in xattr dir */
		memset(&ent->attrs, 0, sizeof(fattr4));

		ent->nextentry = NULL;
		*tail = ent;
		tail = &ent->nextentry;
	}

	resok->reply.entries = entries;
	resok->reply.uio = NULL;
	resok->reply.eof = lr_eof || (xattr_list.xl4_count == 0);

	/* Free the xattr list from FSAL (names were copied above) */
	for (i = 0; i < xattr_list.xl4_count; i++)
		gsh_free(xattr_list.xl4_entries[i].utf8string_val);
	gsh_free(xattr_list.xl4_entries);

	return NFS_REQ_OK;
}

/**
 * @brief GETATTR on an xattr handle (dir or obj).
 *
 * Synthesizes minimal attributes. For xattr dir: returns DIRECTORY type.
 * For xattr obj: returns REGULAR_FILE type with size from getxattrs.
 */
enum nfs_req_result nfs4_op_getattr_xattr(struct nfs_argop4 *op,
					  compound_data_t *data,
					  struct nfs_resop4 *resp)
{
	GETATTR4args *const arg_GETATTR4 = &op->nfs_argop4_u.opgetattr;
	GETATTR4res *const res_GETATTR4 = &resp->nfs_resop4_u.opgetattr;
	fattr4 *obj_attributes =
		&res_GETATTR4->GETATTR4res_u.resok4.obj_attributes;
	struct fsal_obj_handle *obj = data->current_obj;
	struct fsal_attrlist attrs;
	struct xdr_attrs_args args;
	fsal_status_t fsal_status;
	attrmask_t mask;
	bool is_xattr_obj = nfs4_Is_Fh_Xattr_Obj(&data->currentFH);

	resp->resop = NFS4_OP_GETATTR;
	res_GETATTR4->status = NFS4_OK;

	if (arg_GETATTR4->attr_request.bitmap4_len == 0)
		goto out;

	/* Get parent's attributes as a base */
	bitmap4_to_attrmask_t(&arg_GETATTR4->attr_request, &mask);
	fsal_prepare_attrs(&attrs, mask | ATTR_MODE | ATTR_SIZE);

	fsal_status = obj->obj_ops->getattrs(obj, &attrs);
	if (FSAL_IS_ERROR(fsal_status)) {
		res_GETATTR4->status = nfs4_Errno_status(fsal_status);
		fsal_release_attrs(&attrs);
		goto out;
	}

	/* Override attributes for xattr context */
	if (is_xattr_obj) {
		const char *xname;
		uint8_t xname_len;

		if (nfs4_xattr_obj_name(&data->currentFH, &xname,
					&xname_len)) {
			xattrkey4 xa_name;
			xattrvalue4 xa_value;

			/* Get actual xattr size */
			xa_name.utf8string_len = xname_len;
			xa_name.utf8string_val = (char *)xname;
			xa_value.utf8string_len = 0;
			xa_value.utf8string_val = NULL;

			fsal_status = obj->obj_ops->getxattrs(
				obj, &xa_name, &xa_value);
			if (!FSAL_IS_ERROR(fsal_status)) {
				attrs.filesize = xa_value.utf8string_len;
				attrs.spaceused = xa_value.utf8string_len;
				FSAL_SET_MASK(attrs.valid_mask, ATTR_SIZE);
				gsh_free(xa_value.utf8string_val);
			}

			/* Synthesize fileid from parent + xattr name */
			attrs.fileid = obj->fileid ^
				       fnv1a_hash(xname, xname_len);
		}

		attrs.type = REGULAR_FILE;
		attrs.numlinks = 1;
		attrs.mode &= 0644;
	} else {
		/* xattr directory */
		attrs.type = DIRECTORY;
		attrs.fileid = obj->fileid ^ 0xFFFFFFFF;
		attrs.filesize = 4096;
		attrs.numlinks = 2;
		attrs.mode = 0755;
	}

	FSAL_SET_MASK(attrs.valid_mask,
		      ATTR_TYPE | ATTR_FILEID | ATTR_NUMLINKS | ATTR_MODE);

	/* Restore the originally requested mask for the encoder */
	attrs.request_mask = mask;

	nfs4_bitmap4_Remove_Unsupported(&arg_GETATTR4->attr_request);

	LogDebug(COMPONENT_NFS_V4,
		 "GETATTR xattr: type=%d fileid=%" PRIu64
		 " mode=%o size=%" PRIu64 " valid_mask=0x%" PRIx64
		 " bitmap_len=%d is_xattr_obj=%d",
		 attrs.type, attrs.fileid, attrs.mode,
		 attrs.filesize, (uint64_t)attrs.valid_mask,
		 arg_GETATTR4->attr_request.bitmap4_len,
		 is_xattr_obj);

	memset(&args, 0, sizeof(args));
	args.attrs = &attrs;
	args.data = data;
	args.hdl4 = &data->currentFH;
	args.fileid = attrs.fileid;
	args.type = attrs.type;
	args.fsid = obj->fsid;

	if (nfs4_FSALattr_To_Fattr(&args, &arg_GETATTR4->attr_request,
				    obj_attributes) != 0) {
		res_GETATTR4->status = NFS4ERR_IO;
	}

	LogDebug(COMPONENT_NFS_V4,
		 "GETATTR xattr result: status=%s bitmap_len=%d attrlist_len=%u",
		 nfsstat4_to_str(res_GETATTR4->status),
		 obj_attributes->attrmask.bitmap4_len,
		 obj_attributes->attr_vals.attrlist4_len);

	fsal_release_attrs(&attrs);

	if (res_GETATTR4->status == NFS4_OK) {
		data->op_resp_size =
			sizeof(nfsstat4) +
			obj_attributes->attr_vals.attrlist4_len;

		res_GETATTR4->status =
			check_resp_room(data, data->op_resp_size);
	}

out:
	if (res_GETATTR4->status != NFS4_OK) {
		nfs4_Fattr_Free(obj_attributes);
		data->op_resp_size = sizeof(nfsstat4);
	}

	return nfsstat4_to_nfs_req_result(res_GETATTR4->status);
}

/**
 * @brief READ an xattr value.
 *
 * Reads the xattr value from the parent object using the xattr name
 * encoded in the current file handle. Returns data via io_data iov.
 */
enum nfs_req_result nfs4_op_read_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp)
{
	READ4args *const arg_READ4 = &op->nfs_argop4_u.opread;
	READ4res *const res_READ4 = &resp->nfs_resop4_u.opread;
	READ4resok *resok = &res_READ4->READ4res_u.resok4;
	struct fsal_obj_handle *obj = data->current_obj;
	fsal_status_t fsal_status;
	const char *xname;
	uint8_t xname_len;
	xattrkey4 xa_name;
	xattrvalue4 xa_value;
	uint64_t offset;
	uint32_t count;
	uint32_t avail;
	uint32_t alloc_size;
	char *buffer;

	resp->resop = NFS4_OP_READ;
	res_READ4->status = NFS4_OK;

	if (!nfs4_xattr_obj_name(&data->currentFH, &xname, &xname_len)) {
		res_READ4->status = NFS4ERR_BADHANDLE;
		return NFS_REQ_ERROR;
	}

	offset = arg_READ4->offset;
	count = arg_READ4->count;

	LogDebug(COMPONENT_NFS_V4,
		 "READ xattr name=%.*s offset=%" PRIu64 " count=%u",
		 xname_len, xname, offset, count);

	/* Read the full xattr value */
	xa_name.utf8string_len = xname_len;
	xa_name.utf8string_val = (char *)xname;
	xa_value.utf8string_len = XATTR_MAX_VALUE_SIZE;
	xa_value.utf8string_val = gsh_malloc(XATTR_MAX_VALUE_SIZE);

	fsal_status = obj->obj_ops->getxattrs(obj, &xa_name, &xa_value);
	if (FSAL_IS_ERROR(fsal_status)) {
		gsh_free(xa_value.utf8string_val);
		if (fsal_status.major == ERR_FSAL_NOXATTR) {
			/*
			 * The xattr does not exist (or has been removed since
			 * OPEN).  In the OPENATTR named-attribute paradigm the
			 * client treats xattrs as regular files, so READ must
			 * return standard READ results, never NFS4ERR_NOXATTR
			 * (which is only valid for GETXATTR/SETXATTR/
			 * REMOVEXATTR per RFC 8276).  Returning NFS4ERR_NOXATTR
			 * from READ causes the macOS NFS kernel module to panic
			 * (NULL deref at offset 0x90) because it does not
			 * expect this error from a READ operation.
			 *
			 * Return an empty read with EOF -- this is consistent
			 * with reading a file that has been truncated to zero.
			 */
			LogDebug(COMPONENT_NFS_V4,
				 "READ xattr name=%.*s: xattr not found, "
				 "returning empty read with EOF",
				 xname_len, xname);
			resok->eof = true;
			resok->data.data_len = 0;
			resok->data.iovcnt = 1;
			resok->data.iov = &resok->iov0;
			resok->data.last_iov_buf_size = 0;
			resok->iov0.iov_len = 0;
			resok->iov0.iov_base = NULL;
			resok->data.release = NULL;
			return NFS_REQ_OK;
		} else if (fsal_status.major == ERR_FSAL_XATTR2BIG) {
			/* Retry with size discovery */
			xa_value.utf8string_len = 0;
			xa_value.utf8string_val = NULL;
			fsal_status = obj->obj_ops->getxattrs(obj, &xa_name,
							      &xa_value);
			if (FSAL_IS_ERROR(fsal_status)) {
				res_READ4->status =
					nfs4_Errno_status(fsal_status);
				return NFS_REQ_ERROR;
			}
			xa_value.utf8string_val =
				gsh_malloc(xa_value.utf8string_len);
			fsal_status = obj->obj_ops->getxattrs(obj, &xa_name,
							      &xa_value);
			if (FSAL_IS_ERROR(fsal_status)) {
				gsh_free(xa_value.utf8string_val);
				res_READ4->status =
					nfs4_Errno_status(fsal_status);
				return NFS_REQ_ERROR;
			}
		} else {
			res_READ4->status = nfs4_Errno_status(fsal_status);
			return NFS_REQ_ERROR;
		}
	}

	/* Apply offset and count, set up io_data response.
	 *
	 * The io_data struct is consumed by xdr_io_data_encode() which builds
	 * an xdr_uio for zero-copy transmission.  When the data length is not
	 * a multiple of BYTES_PER_XDR_UNIT (4), the encoder needs padding
	 * bytes.  It first checks last_iov_buf_size to see if the last iov
	 * buffer has room for the padding; if so it extends the buffer
	 * in-place.  We therefore allocate with RNDUP(count) and set
	 * last_iov_buf_size accordingly so the encoder can pad safely.
	 */
	if (offset >= xa_value.utf8string_len) {
		/* Reading past end */
		resok->eof = true;
		resok->data.data_len = 0;
		resok->data.iovcnt = 1;
		resok->data.iov = &resok->iov0;
		resok->data.last_iov_buf_size = 0;
		resok->iov0.iov_len = 0;
		resok->iov0.iov_base = NULL;
		resok->data.release = NULL;
		gsh_free(xa_value.utf8string_val);
	} else {
		avail = xa_value.utf8string_len - offset;
		if (count > avail)
			count = avail;

		/* Allocate with XDR alignment so the encoder can pad
		 * in-place without overflowing the buffer.
		 */
		alloc_size = RNDUP(count);
		buffer = gsh_malloc(alloc_size);
		memcpy(buffer, xa_value.utf8string_val + offset, count);
		gsh_free(xa_value.utf8string_val);

		resok->eof = (offset + count >= xa_value.utf8string_len);
		resok->iov0.iov_base = buffer;
		resok->iov0.iov_len = count;
		resok->data.data_len = count;
		resok->data.iovcnt = 1;
		resok->data.iov = &resok->iov0;
		resok->data.last_iov_buf_size = alloc_size;
		resok->data.release = xattr_read_data_release;
		resok->data.release_data = buffer;
	}

	return NFS_REQ_OK;
}

/**
 * @brief WRITE an xattr value.
 *
 * Writes the xattr value on the parent object using the xattr name
 * encoded in the current file handle.
 */
enum nfs_req_result nfs4_op_write_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp)
{
	WRITE4args *const arg_WRITE4 = &op->nfs_argop4_u.opwrite;
	WRITE4res *const res_WRITE4 = &resp->nfs_resop4_u.opwrite;
	struct fsal_obj_handle *obj = data->current_obj;
	fsal_status_t fsal_status;
	const char *xname;
	uint8_t xname_len;
	xattrkey4 xa_name;
	xattrvalue4 xa_value;
	char *write_buf = NULL;

	resp->resop = NFS4_OP_WRITE;
	res_WRITE4->status = NFS4_OK;

	if (!nfs4_xattr_obj_name(&data->currentFH, &xname, &xname_len)) {
		res_WRITE4->status = NFS4ERR_BADHANDLE;
		return NFS_REQ_ERROR;
	}

	LogDebug(COMPONENT_NFS_V4,
		 "WRITE xattr name=%.*s offset=%" PRIu64 " len=%u",
		 xname_len, xname, arg_WRITE4->offset,
		 arg_WRITE4->data.data_len);

	xa_name.utf8string_len = xname_len;
	xa_name.utf8string_val = (char *)xname;

	/*
	 * xattrs are atomic set operations. If offset > 0, we need to
	 * read-modify-write. For macOS named attribute usage the offset
	 * is typically 0.
	 */
	if (arg_WRITE4->offset != 0) {
		/* Read current value, splice in new data */
		xattrvalue4 cur_value;
		uint64_t new_len;

		cur_value.utf8string_len = XATTR_MAX_VALUE_SIZE;
		cur_value.utf8string_val = gsh_malloc(XATTR_MAX_VALUE_SIZE);

		fsal_status = obj->obj_ops->getxattrs(obj, &xa_name,
						      &cur_value);
		if (FSAL_IS_ERROR(fsal_status)) {
			gsh_free(cur_value.utf8string_val);
			res_WRITE4->status = nfs4_Errno_status(fsal_status);
			return NFS_REQ_ERROR;
		}

		/* Compute new total size */
		new_len = arg_WRITE4->offset + arg_WRITE4->data.data_len;
		if (new_len < cur_value.utf8string_len)
			new_len = cur_value.utf8string_len;

		write_buf = gsh_calloc(1, new_len);
		memcpy(write_buf, cur_value.utf8string_val,
		       cur_value.utf8string_len);
		memcpy(write_buf + arg_WRITE4->offset,
		       arg_WRITE4->data.iov[0].iov_base,
		       arg_WRITE4->data.data_len);
		gsh_free(cur_value.utf8string_val);

		xa_value.utf8string_len = new_len;
		xa_value.utf8string_val = write_buf;
	} else {
		xa_value.utf8string_len = arg_WRITE4->data.data_len;
		xa_value.utf8string_val = arg_WRITE4->data.iov[0].iov_base;
	}

	fsal_status = obj->obj_ops->setxattrs(obj, SETXATTR4_EITHER,
					      &xa_name, &xa_value);

	gsh_free(write_buf);

	if (FSAL_IS_ERROR(fsal_status)) {
		res_WRITE4->status = nfs4_Errno_status(fsal_status);
		return NFS_REQ_ERROR;
	}

	res_WRITE4->WRITE4res_u.resok4.count = arg_WRITE4->data.data_len;
	res_WRITE4->WRITE4res_u.resok4.committed = FILE_SYNC4;

	/* Write verifier */
	{
		struct gsh_buffdesc verf_desc;

		verf_desc.addr = res_WRITE4->WRITE4res_u.resok4.writeverf;
		verf_desc.len = sizeof(verifier4);
		op_ctx->fsal_export->exp_ops.get_write_verifier(
			op_ctx->fsal_export, &verf_desc);
	}

	return NFS_REQ_OK;
}

/**
 * @brief OPEN on an xattr handle.
 *
 * Returns success with an anonymous stateid. No real state is created
 * for xattr opens -- xattr operations are stateless.
 *
 * Crucially, this must also build an xattr-object file handle and
 * install it as the current FH, so that subsequent operations in the
 * same compound (GETATTR, WRITE) operate on the correct handle.
 */
enum nfs_req_result nfs4_op_open_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp)
{
	OPEN4args *const arg_OPEN4 = &op->nfs_argop4_u.opopen;
	OPEN4res *const res_OPEN4 = &resp->nfs_resop4_u.opopen;
	OPEN4resok *resok = &res_OPEN4->OPEN4res_u.resok4;
	char val_fh[NFS4_FHSIZE];
	nfs_fh4 entry_fh = { .nfs_fh4_len = NFS4_FHSIZE,
			      .nfs_fh4_val = val_fh };
	nfsstat4 status;
	const char *xname;
	uint8_t xname_len;

	resp->resop = NFS4_OP_OPEN;
	res_OPEN4->status = NFS4_OK;

	/*
	 * Get the xattr name from the OPEN claim. For CLAIM_NULL,
	 * it's the filename component.
	 */
	if (arg_OPEN4->claim.claim == CLAIM_NULL) {
		xname = arg_OPEN4->claim.open_claim4_u.file.utf8string_val;
		xname_len = arg_OPEN4->claim.open_claim4_u.file.utf8string_len;
	} else {
		/* Other claim types not expected for xattr opens */
		xname = NULL;
		xname_len = 0;
	}

	LogDebug(COMPONENT_NFS_V4,
		 "OPEN xattr name=%.*s (returning anonymous stateid)",
		 xname_len, xname ? xname : "");

	/*
	 * Build an xattr object handle and install it as current FH.
	 * This is critical: after OPEN, the current FH must point to
	 * the opened object, not the parent directory.
	 */
	if (xname != NULL && xname_len > 0) {
		status = nfs4_xattr_build_entry_fh(data, xname, xname_len,
						   &entry_fh);
		if (status != NFS4_OK) {
			res_OPEN4->status = status;
			return NFS_REQ_ERROR;
		}

		memcpy(data->currentFH.nfs_fh4_val, entry_fh.nfs_fh4_val,
		       entry_fh.nfs_fh4_len);
		data->currentFH.nfs_fh4_len = entry_fh.nfs_fh4_len;
		data->current_filetype = REGULAR_FILE;
	}

	/* Return the anonymous all-zeros stateid */
	memset(&resok->stateid, 0, sizeof(stateid4));

	/* No delegation */
	resok->delegation.delegation_type = OPEN_DELEGATE_NONE;

	/* Dummy change info */
	resok->cinfo.atomic = false;
	resok->cinfo.before = 0;
	resok->cinfo.after = 0;

	/* No special flags */
	resok->rflags = 0;

	/* Empty attrset bitmap */
	memset(&resok->attrset, 0, sizeof(struct bitmap4));

	return NFS_REQ_OK;
}

/**
 * @brief CLOSE on an xattr object handle.
 *
 * No-op success -- xattr operations are stateless.
 */
enum nfs_req_result nfs4_op_close_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp)
{
	CLOSE4res *const res_CLOSE4 = &resp->nfs_resop4_u.opclose;

	resp->resop = NFS4_OP_CLOSE;
	res_CLOSE4->status = NFS4_OK;

	LogDebug(COMPONENT_NFS_V4, "CLOSE xattr (no-op)");

	/* Return all-ones stateid to indicate closed */
	memset(&res_CLOSE4->CLOSE4res_u.open_stateid, 0xFF, sizeof(stateid4));

	return NFS_REQ_OK;
}

/**
 * @brief REMOVE an xattr from the parent object.
 *
 * Removes the named xattr using the FSAL removexattrs interface.
 */
enum nfs_req_result nfs4_op_remove_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp)
{
	REMOVE4args *const arg_REMOVE4 = &op->nfs_argop4_u.opremove;
	REMOVE4res *const res_REMOVE4 = &resp->nfs_resop4_u.opremove;
	struct fsal_obj_handle *obj = data->current_obj;
	fsal_status_t fsal_status;
	xattrkey4 xa_name;

	resp->resop = NFS4_OP_REMOVE;
	res_REMOVE4->status = NFS4_OK;

	if (arg_REMOVE4->target.utf8string_len == 0 ||
	    arg_REMOVE4->target.utf8string_len > NAME_MAX) {
		res_REMOVE4->status = NFS4ERR_NAMETOOLONG;
		return NFS_REQ_ERROR;
	}

	LogDebug(COMPONENT_NFS_V4, "REMOVE xattr name=%.*s",
		 arg_REMOVE4->target.utf8string_len,
		 arg_REMOVE4->target.utf8string_val);

	xa_name.utf8string_len = arg_REMOVE4->target.utf8string_len;
	xa_name.utf8string_val = arg_REMOVE4->target.utf8string_val;

	/* Get change info before */
	res_REMOVE4->REMOVE4res_u.resok4.cinfo.atomic = false;
	res_REMOVE4->REMOVE4res_u.resok4.cinfo.before =
		fsal_get_changeid4(obj);

	fsal_status = obj->obj_ops->removexattrs(obj, &xa_name);
	if (FSAL_IS_ERROR(fsal_status)) {
		if (fsal_status.major == ERR_FSAL_NOXATTR)
			res_REMOVE4->status = NFS4ERR_NOENT;
		else
			res_REMOVE4->status = nfs4_Errno_status(fsal_status);
		return NFS_REQ_ERROR;
	}

	res_REMOVE4->REMOVE4res_u.resok4.cinfo.after =
		fsal_get_changeid4(obj);

	return NFS_REQ_OK;
}

/**
 * @brief ACCESS check for xattr handles.
 *
 * The standard ACCESS handler checks data->current_obj which for xattr
 * handles is the parent file, not a directory.  This causes macOS to be
 * denied ACCESS4_LOOKUP (which is invalid for regular files) even though
 * the xattr handle is presented as a directory (NF4ATTRDIR).  macOS then
 * refuses to read xattr values, returning EACCES.
 *
 * For xattr dir handles we grant directory-appropriate access bits based
 * on the parent file's actual read/write permissions.  For xattr object
 * handles we grant file-appropriate access bits.
 */
enum nfs_req_result nfs4_op_access_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp)
{
	ACCESS4args *const arg_ACCESS4 = &op->nfs_argop4_u.opaccess;
	ACCESS4res *const res_ACCESS4 = &resp->nfs_resop4_u.opaccess;
	struct fsal_obj_handle *obj = data->current_obj;
	fsal_accessflags_t access_allowed = 0;
	fsal_accessflags_t access_denied = 0;
	fsal_status_t fsal_status;
	uint32_t requested = arg_ACCESS4->access;
	uint32_t granted = 0;
	bool can_read = false;
	bool can_write = false;

	resp->resop = NFS4_OP_ACCESS;
	res_ACCESS4->status = NFS4_OK;

	/* Check parent file's read/write permission */
	fsal_status = obj->obj_ops->test_access(obj,
						FSAL_R_OK,
						&access_allowed,
						&access_denied,
						false);
	if (access_allowed & FSAL_R_OK)
		can_read = true;

	fsal_status = obj->obj_ops->test_access(obj,
						FSAL_W_OK,
						&access_allowed,
						&access_denied,
						false);
	if (access_allowed & FSAL_W_OK)
		can_write = true;

	if (nfs4_Is_Fh_Xattr_Dir(&data->currentFH)) {
		/* xattr directory: grant directory-like access */
		if (can_read) {
			granted |= ACCESS4_READ | ACCESS4_LOOKUP |
				   ACCESS4_EXECUTE;
		}
		if (can_write) {
			granted |= ACCESS4_MODIFY | ACCESS4_EXTEND |
				   ACCESS4_DELETE;
		}
	} else {
		/* xattr object: grant file-like access */
		if (can_read)
			granted |= ACCESS4_READ;
		if (can_write) {
			granted |= ACCESS4_MODIFY | ACCESS4_EXTEND;
		}
	}

	/* Only return bits the client actually asked about */
	res_ACCESS4->ACCESS4res_u.resok4.supported = requested;
	res_ACCESS4->ACCESS4res_u.resok4.access = granted & requested;

	LogDebug(COMPONENT_NFS_V4,
		 "ACCESS xattr: requested=0x%x granted=0x%x "
		 "can_read=%d can_write=%d is_dir=%d",
		 requested, res_ACCESS4->ACCESS4res_u.resok4.access,
		 can_read, can_write,
		 nfs4_Is_Fh_Xattr_Dir(&data->currentFH));

	return NFS_REQ_OK;
}
