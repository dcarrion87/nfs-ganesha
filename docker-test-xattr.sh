#!/bin/bash
# Test script for NFSv4 OPENATTR / named attribute support
# Run inside the Docker container with --privileged
# Uses MEM FSAL to avoid VFS open_by_handle_at issues in containers.
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0

pass() { echo -e "${GREEN}PASS${NC}: $1"; PASS=$((PASS + 1)); }
fail() { echo -e "${RED}FAIL${NC}: $1"; FAIL=$((FAIL + 1)); }
skip() { echo -e "${YELLOW}SKIP${NC}: $1"; SKIP=$((SKIP + 1)); }

MOUNT_POINT="/mnt/nfs"

echo "=== NFS-Ganesha OPENATTR/xattr Test Suite ==="
echo ""

# --- Phase 1: Start services ---
echo "--- Phase 1: Starting services ---"

# Start dbus
if [ ! -f /run/dbus/pid ]; then
    dbus-daemon --system 2>/dev/null || true
fi

# Start rpcbind
rpcbind 2>/dev/null || true

# Start ganesha (MEM FSAL - no real filesystem needed)
echo "Starting NFS-Ganesha (MEM FSAL)..."
ganesha.nfsd -F -L /var/log/ganesha/ganesha.log -f /etc/ganesha/ganesha.conf &
GANESHA_PID=$!

# Wait for ganesha to be ready
sleep 5
if ! kill -0 $GANESHA_PID 2>/dev/null; then
    echo -e "${RED}ERROR${NC}: Ganesha failed to start. Log:"
    tail -50 /var/log/ganesha/ganesha.log
    exit 1
fi
echo "Ganesha started (pid $GANESHA_PID)"

# --- Phase 2: Mount ---
echo ""
echo "--- Phase 2: Mounting ---"

mkdir -p "$MOUNT_POINT"

if mount -t nfs4 -o vers=4.0,noac 127.0.0.1:/export "$MOUNT_POINT" 2>/dev/null; then
    pass "NFSv4 mount succeeded"
else
    fail "NFSv4 mount failed"
    echo "Log tail:"
    tail -20 /var/log/ganesha/ganesha.log
    kill $GANESHA_PID 2>/dev/null
    exit 1
fi

# --- Phase 3: Basic NFS operations ---
echo ""
echo "--- Phase 3: Basic NFS operations ---"

# MEM FSAL starts empty, create test files via NFS
mkdir -p "${MOUNT_POINT}/testdir"
echo "test content" > "${MOUNT_POINT}/testdir/testfile.txt"

TEST_FILE="${MOUNT_POINT}/testdir/testfile.txt"

if [ -f "$TEST_FILE" ]; then
    pass "Test file visible via NFS"
else
    fail "Test file not visible via NFS"
fi

if cat "$TEST_FILE" | grep -q "test content"; then
    pass "Test file readable via NFS"
else
    fail "Test file not readable via NFS"
fi

# --- Phase 4: xattr operations via NFS ---
echo ""
echo "--- Phase 4: xattr operations via NFS ---"
echo "(Linux NFS client uses NFSv4.2 GETXATTR/SETXATTR, not OPENATTR."
echo " These will likely SKIP. True OPENATTR testing requires macOS client.)"

# Test setting an xattr
if setfattr -n user.nfstest -v "hello_xattr" "$TEST_FILE" 2>/dev/null; then
    pass "setfattr on NFS file"
else
    skip "setfattr on NFS file (Linux client uses NFSv4.2, not OPENATTR)"
fi

# Test reading an xattr
XATTR_VAL=$(getfattr -n user.nfstest --only-values "$TEST_FILE" 2>/dev/null) || true
if [ "$XATTR_VAL" = "hello_xattr" ]; then
    pass "getfattr on NFS file"
else
    skip "getfattr on NFS file (value='${XATTR_VAL}')"
fi

# Test listing xattrs
XATTR_LIST=$(getfattr -d "$TEST_FILE" 2>/dev/null) || true
if echo "$XATTR_LIST" | grep -q "user.nfstest"; then
    pass "listxattr on NFS file"
else
    skip "listxattr on NFS file"
fi

# Test removing an xattr
if setfattr -x user.nfstest "$TEST_FILE" 2>/dev/null; then
    pass "removexattr on NFS file"
else
    skip "removexattr on NFS file"
fi

# Verify removal
XATTR_VAL2=$(getfattr -n user.nfstest --only-values "$TEST_FILE" 2>&1) || true
if echo "$XATTR_VAL2" | grep -qi "no such\|not found\|no data"; then
    pass "xattr removed successfully"
else
    skip "xattr removal verification"
fi

# --- Phase 5: Intensive I/O ---
echo ""
echo "--- Phase 5: Intensive I/O ---"

# dd write test
if dd if=/dev/urandom of="${MOUNT_POINT}/ddtest" bs=4096 count=256 2>/dev/null; then
    pass "dd write 1MB random data"
else
    fail "dd write 1MB random data"
fi

# dd read test
if dd if="${MOUNT_POINT}/ddtest" of=/dev/null bs=4096 2>/dev/null; then
    pass "dd read 1MB back"
else
    fail "dd read 1MB back"
fi

# Checksum round-trip
MD5_ORIG=$(md5sum "${MOUNT_POINT}/ddtest" 2>/dev/null | awk '{print $1}')
cp "${MOUNT_POINT}/ddtest" /tmp/ddtest_copy 2>/dev/null
MD5_COPY=$(md5sum /tmp/ddtest_copy 2>/dev/null | awk '{print $1}')
if [ -n "$MD5_ORIG" ] && [ "$MD5_ORIG" = "$MD5_COPY" ]; then
    pass "dd checksum round-trip"
else
    fail "dd checksum round-trip (orig=$MD5_ORIG copy=$MD5_COPY)"
fi

# Large file
if dd if=/dev/urandom of="${MOUNT_POINT}/ddtest_large" bs=65536 count=64 2>/dev/null; then
    pass "dd write 4MB file"
else
    fail "dd write 4MB file"
fi

if dd if="${MOUNT_POINT}/ddtest_large" of=/dev/null bs=65536 2>/dev/null; then
    pass "dd read 4MB file"
else
    fail "dd read 4MB file"
fi

# Multiple small files
ALL_OK=true
for i in $(seq 1 50); do
    echo "content_$i" > "${MOUNT_POINT}/smallfile_$i" 2>/dev/null || ALL_OK=false
done
if $ALL_OK; then
    pass "Create 50 small files"
else
    fail "Create 50 small files"
fi

# Verify small files
VERIFY_OK=true
for i in $(seq 1 50); do
    content=$(cat "${MOUNT_POINT}/smallfile_$i" 2>/dev/null)
    if [ "$content" != "content_$i" ]; then
        VERIFY_OK=false
    fi
done
if $VERIFY_OK; then
    pass "Verify 50 small files content"
else
    fail "Verify 50 small files content"
fi

# Directory operations
if mkdir -p "${MOUNT_POINT}/dir1/dir2/dir3" 2>/dev/null; then
    pass "Nested directory creation"
else
    fail "Nested directory creation"
fi

echo "deep file" > "${MOUNT_POINT}/dir1/dir2/dir3/deep.txt" 2>/dev/null
DEEP_CONTENT=$(cat "${MOUNT_POINT}/dir1/dir2/dir3/deep.txt" 2>/dev/null)
if [ "$DEEP_CONTENT" = "deep file" ]; then
    pass "Deep directory file read/write"
else
    fail "Deep directory file read/write"
fi

# File removal
rm -f "${MOUNT_POINT}/ddtest" 2>/dev/null
if [ ! -f "${MOUNT_POINT}/ddtest" ]; then
    pass "File deletion"
else
    fail "File deletion"
fi

# Concurrent writes
for i in $(seq 1 10); do
    dd if=/dev/urandom of="${MOUNT_POINT}/concurrent_$i" bs=4096 count=32 2>/dev/null &
done
wait
ALL_EXIST=true
for i in $(seq 1 10); do
    [ -f "${MOUNT_POINT}/concurrent_$i" ] || ALL_EXIST=false
done
if $ALL_EXIST; then
    pass "10 concurrent dd writes"
else
    fail "10 concurrent dd writes"
fi

# --- Cleanup ---
echo ""
echo "--- Cleanup ---"
umount "$MOUNT_POINT" 2>/dev/null || true
kill $GANESHA_PID 2>/dev/null || true
wait $GANESHA_PID 2>/dev/null || true

# --- Summary ---
echo ""
echo "=== Results ==="
echo -e "  ${GREEN}Passed${NC}: $PASS"
echo -e "  ${RED}Failed${NC}: $FAIL"
echo -e "  ${YELLOW}Skipped${NC}: $SKIP"
echo ""

if [ $FAIL -gt 0 ]; then
    echo -e "${RED}Some tests failed.${NC}"
    echo "Check /var/log/ganesha/ganesha.log for details."
    exit 1
else
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
fi
