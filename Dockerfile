FROM ubuntu:20.04

# get dependencies
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && \
    apt install -y wget unzip build-essential git m4 scons zlib1g zlib1g-dev libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev python3-dev python3-six python3-pydot python-is-python3 libboost-all-dev pkg-config && \
    apt-get clean

# checkout gem5 repo
WORKDIR /usr/local/src
RUN git clone -b v20.1.0.5 https://gem5.googlesource.com/public/gem5
# build it
WORKDIR /usr/local/src/gem5

# Build gem5
RUN scons -j$(nproc) --ignore-style build/X86/gem5.opt && \
    rm -f /usr/local/bin/gem5.opt

# WORKDIR /usr/local/src/gem5/util/m5
# RUN scons build/x86/out/m5

# WORKDIR /usr/local/src
# RUN git clone -b v20.1.0.5 https://gem5.googlesource.com/public/gem5-resources && \
#     cp -r /usr/local/src/gem5-resources/src/parsec/disk-image ./parsec-bench
# WORKDIR /usr/local/src/parsec-bench
# RUN wget -q https://releases.hashicorp.com/packer/1.6.0/packer_1.6.0_linux_amd64.zip && \
#     unzip packer_1.6.0_linux_amd64.zip && \
#     ./packer validate parsec/parsec.json && \
#     ./packer build parsec/parsec.json
# RUN wget http://dist.gem5.org/dist/v20-1/kernels/x86/static/vmlinux-4.19.83

WORKDIR /root/workspace
RUN chmod 777 /root/workspace
ADD dbrc_cache.hh dbrc_cache.cc SConscript DbrcCache.py /usr/local/src/gem5/src/learning_gem5/mine/
WORKDIR /usr/local/src/gem5
RUN rm -f /usr/local/bin/gem5.opt && \
    scons -j$(nproc) --ignore-style build/X86/gem5.opt && \
    mv build/X86/gem5.opt /usr/local/bin && \
    ln -s /usr/local/bin/gem5.opt build/X86/gem5.opt
WORKDIR /root/workspace
CMD bash
