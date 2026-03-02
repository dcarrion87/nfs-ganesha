/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * @file nfs4_xattr_handle_ops.h
 * @brief Declarations for NFSv4 OPENATTR xattr handle operations.
 *
 * These functions handle NFS4 operations when the current filehandle
 * has xattr flags set (FILE_HANDLE_V4_FLAG_XATTR_DIR or
 * FILE_HANDLE_V4_FLAG_XATTR_OBJ). They bridge NFSv4 named attribute
 * protocol operations to the FSAL xattr interface.
 */

#ifndef NFS4_XATTR_HANDLE_OPS_H
#define NFS4_XATTR_HANDLE_OPS_H

#include "nfs_proto_functions.h"

/* Dispatch functions called from standard NFS4 op handlers */

enum nfs_req_result nfs4_op_readdir_xattr(struct nfs_argop4 *op,
					  compound_data_t *data,
					  struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_lookup_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_getattr_xattr(struct nfs_argop4 *op,
					  compound_data_t *data,
					  struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_read_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_write_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_open_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_close_xattr(struct nfs_argop4 *op,
					compound_data_t *data,
					struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_remove_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp);

enum nfs_req_result nfs4_op_access_xattr(struct nfs_argop4 *op,
					 compound_data_t *data,
					 struct nfs_resop4 *resp);

#endif /* NFS4_XATTR_HANDLE_OPS_H */
