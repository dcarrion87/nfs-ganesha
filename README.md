nfs-ganesha (OPENATTR fork)
===========================

This fork adds **NFSv4 OPENATTR support** to NFS-Ganesha, enabling macOS
clients to read, write, list, and delete extended attributes over NFS using
the named attribute protocol (RFC 5661 Section 10.1).

Upstream NFS-Ganesha stubs OPENATTR as `NFS4ERR_NOTSUPP`. This fork
implements the full protocol flow required by macOS's `namedattr` mount option.

## What's New

### OPENATTR Protocol Support

When a client sends OPENATTR on a file handle, the server now returns a
synthetic xattr directory handle. Standard NFS operations within that
directory map to FSAL xattr calls:

| NFS Operation | xattr Action |
|---------------|-------------|
| READDIR | List xattrs |
| LOOKUP | Resolve xattr name |
| GETATTR | Get xattr size/metadata |
| READ | Read xattr value |
| WRITE | Write xattr value |
| OPEN | Open xattr for I/O |
| CLOSE | Close xattr handle |
| REMOVE | Delete xattr |
| ACCESS | Check xattr permissions |

### File Handle Encoding

Xattr handles extend the parent's fsopaque with flags in `fhflags1`:

    xattr-dir:  [parent_fsopaque] + XATTR_DIR flag
    xattr-obj:  [parent_fsopaque][xattr_name (N bytes)][name_len (1 byte)] + XATTR_OBJ flag

PUTFH strips xattr flags before FSAL lookup — the FSAL only sees the parent
object. All xattr operations work against the parent's FSAL handle.

### MEM FSAL xattr Support

The MEM (in-memory) FSAL now has full xattr support for development and
testing, with linked-list xattr storage per object.

### VFS FSAL Compatibility

The VFS FSAL already had `getxattrs`/`setxattrs`/`listxattrs`/`removexattrs`
methods. This fork also fixes an XDR padding bug in `xdr_io_data_encode()`
that caused `EBADRPC` errors when reading files with sizes not aligned to
4 bytes.

## Usage

### macOS Client

    # Mount with namedattr support
    sudo mount_nfs -o "vers=4,resvport,namedattr" server:/export /mnt

    # xattr operations
    xattr -w com.example.key "value" /mnt/myfile
    xattr -p com.example.key /mnt/myfile    # → value
    xattr /mnt/myfile                        # → com.example.key
    xattr -d com.example.key /mnt/myfile

### Docker (for testing)

    docker compose build
    docker compose up -d    # NFS server on port 12049
    sudo mount -t nfs -o vers=4,resvport,namedattr,port=12049 localhost:/export /tmp/nfs_test

## Files Changed

    New:
      src/Protocols/NFS/nfs4_xattr_handle_ops.c   — xattr operation handlers
      src/Protocols/NFS/nfs4_xattr_handle_ops.h   — declarations

    Modified (dispatch hooks + xattr handle support):
      src/Protocols/NFS/nfs4_op_access.c
      src/Protocols/NFS/nfs4_op_close.c
      src/Protocols/NFS/nfs4_op_getattr.c
      src/Protocols/NFS/nfs4_op_lookup.c
      src/Protocols/NFS/nfs4_op_open.c
      src/Protocols/NFS/nfs4_op_openattr.c
      src/Protocols/NFS/nfs4_op_putfh.c
      src/Protocols/NFS/nfs4_op_read.c
      src/Protocols/NFS/nfs4_op_readdir.c
      src/Protocols/NFS/nfs4_op_remove.c
      src/Protocols/NFS/nfs4_op_write.c
      src/Protocols/NFS/nfs_proto_tools.c
      src/include/nfs_fh.h
      src/include/nfs_file_handle.h
      src/RPCAL/rpc_tools.c

    MEM FSAL:
      src/FSAL/FSAL_MEM/mem_handle.c
      src/FSAL/FSAL_MEM/mem_int.h
      src/FSAL/FSAL_MEM/mem_main.c

---

*Based on [nfs-ganesha](https://github.com/nfs-ganesha/nfs-ganesha) V9.6.*
*For upstream info see [the project wiki](https://github.com/nfs-ganesha/nfs-ganesha/wiki).*
