FROM ubuntu:bionic

# This is for testing purposes

# Install clang 10.0 for Linux (intended not to be in the package)
# From: https://apt.llvm.org/
RUN apt-get update
RUN apt-get --yes install --no-install-recommends wget ca-certificates gnupg
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key| apt-key add -
RUN echo deb http://apt.llvm.org/bionic/ llvm-toolchain-bionic-10 main >> /etc/apt/sources.list
RUN echo deb-src http://apt.llvm.org/bionic/ llvm-toolchain-bionic-10 main >> /etc/apt/sources.list
RUN apt-get update
RUN apt-get --yes install --no-install-recommends clang-10 python make
RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-10 20
RUN update-alternatives --set clang /usr/bin/clang-10
RUN update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-10 20
RUN update-alternatives --set clang++ /usr/bin/clang++-10
RUN clang --version
RUN clang++ --version

# Create a new user and give them sudo rights
RUN useradd -d /home/test test
RUN echo 'test ALL=NOPASSWD: ALL' >> /etc/sudoers
RUN mkdir /home/test
RUN chown test:test /home/test
USER test
ENV HOME /home/test
WORKDIR $HOME
SHELL ["/bin/bash", "--login", "-c"]

# CI: Do not use cache from this point on
ARG BUILD_ID=0
RUN echo $BUILD_ID

# Add files
ADD --chown=test . z3
WORKDIR z3

ENV CXX clang++
ENV CC clang
RUN python scripts/mk_make.py
WORKDIR build
ARG BUILD_THREADS=1
RUN make -j $Z3_THREADS
