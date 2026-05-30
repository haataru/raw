FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CC=clang-18
ENV CXX=clang++-18

RUN apt-get update -qq && apt-get install -y -qq \
    # Compilers and build tools
    clang-18 \
    clang-tidy-18 \
    clang-format-18 \
    lld-18 \
    libc++-18-dev \
    libc++abi-18-dev \
    cmake \
    ninja-build \
    make \
    git \
    # Testing
    libgtest-dev \
    libbenchmark-dev \
    # Sanitizers
    llvm-18-dev \
    libclang-rt-18-dev \
    # Fuzzing
    # Documentation
    doxygen \
    # Coverage
    lcov \
    # Utils
    curl \
    pkg-config \
    && ln -sf /usr/bin/clang-format-18 /usr/local/bin/clang-format \
    && ln -sf /usr/bin/clang-tidy-18 /usr/local/bin/clang-tidy \
    && ln -sf /usr/bin/clang++-18 /usr/local/bin/clang++ \
    && ln -sf /usr/bin/clang-18 /usr/local/bin/clang \
    && ln -sf /usr/bin/lld-18 /usr/local/bin/lld \
    && ln -sf /usr/bin/ld.lld-18 /usr/local/bin/ld.lld \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
CMD ["/bin/bash"]
