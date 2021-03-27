FROM ubuntu:20.04

# get dependencies
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && \
    apt install -y build-essential git m4 scons zlib1g zlib1g-dev libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev python3-dev python3-six python-is-python3 libboost-all-dev pkg-config && \
    apt-get clean

# checkout repo with mercurial
WORKDIR /usr/local/src
RUN git clone https://github.com/gem5/gem5.git
# build it
WORKDIR /usr/local/src/gem5

RUN scons -j$(nproc) --ignore-style build/X86/gem5.opt && \
    rm -f /usr/local/bin/gem5.opt

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
