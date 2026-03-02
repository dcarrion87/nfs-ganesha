#!/bin/bash
# Start NFS-Ganesha server with MEM FSAL for macOS OPENATTR testing
set -e

# Start dbus
if [ ! -f /run/dbus/pid ]; then
    dbus-daemon --system 2>/dev/null || true
fi

# Start rpcbind
rpcbind 2>/dev/null || true

echo "Starting NFS-Ganesha (MEM FSAL)..."
echo "Mount from macOS with:"
echo "  sudo mkdir -p /tmp/nfs_test"
echo "  sudo mount -t nfs -o vers=4,resvport,namedattr,port=12049 localhost:/export /tmp/nfs_test"

# Run ganesha in foreground
exec ganesha.nfsd -F -L /var/log/ganesha/ganesha.log -f /etc/ganesha/ganesha.conf -N NIV_EVENT
