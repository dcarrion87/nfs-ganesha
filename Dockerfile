# Dockerfile for building and testing NFS-Ganesha with OPENATTR/xattr support
#
# Build:
#   docker build -t nfs-ganesha-dev .
#
# Run (interactive):
#   docker run --rm -it --privileged nfs-ganesha-dev
#
# Run (start ganesha in foreground):
#   docker run --rm -it --privileged nfs-ganesha-dev /usr/bin/ganesha.nfsd -F -L /dev/stderr -f /etc/ganesha/ganesha.conf
#

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    git \
    cmake \
    make \
    gcc \
    g++ \
    bison \
    flex \
    libdbus-1-dev \
    libnfsidmap-dev \
    libkrb5-dev \
    libcap-dev \
    libjemalloc-dev \
    libblkid-dev \
    uuid-dev \
    libattr1-dev \
    libacl1-dev \
    xfslibs-dev \
    liburcu-dev \
    nfs-common \
    rpcbind \
    dbus \
    attr \
    xfsprogs \
    e2fsprogs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the full source tree (including submodules)
COPY . /src/nfs-ganesha/

# Initialize submodule if not already present
RUN cd /src/nfs-ganesha && \
    if [ ! -f src/libntirpc/CMakeLists.txt ]; then \
        git submodule update --init --recursive; \
    fi

# Build and install NFS-Ganesha with VFS FSAL
RUN mkdir -p /src/build && cd /src/build && \
    cmake \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DBUILD_CONFIG=vfs_only \
        -DUSE_FSAL_VFS=ON \
        -DUSE_FSAL_PROXY_V3=OFF \
        -DUSE_DBUS=ON \
        -DUSE_9P=OFF \
        -DUSE_NFS3=ON \
        -DUSE_LTTNG=OFF \
        -DUSE_NFS_RDMA=OFF \
        -DUSE_RQUOTA=OFF \
        -D_MSPAC_SUPPORT=OFF \
        /src/nfs-ganesha/src/ && \
    make -j$(nproc) && \
    make install

# Create required directories
RUN mkdir -p /var/run/ganesha \
    /var/log/ganesha \
    /var/lib/nfs/ganesha \
    /run/dbus \
    /export

# Ganesha config
COPY ganesha.conf /etc/ganesha/ganesha.conf

# Copy scripts
COPY docker-test-xattr.sh /usr/local/bin/test-xattr.sh
COPY start-server.sh /usr/local/bin/start-server.sh
RUN chmod +x /usr/local/bin/test-xattr.sh /usr/local/bin/start-server.sh

EXPOSE 2049

CMD ["/bin/bash"]
