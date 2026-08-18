FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG VERILATOR_VERSION=5.048
ARG VERILATOR_JOBS=4

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
    autoconf \
    bison \
    ca-certificates \
    ccache \
    cmake \
    flex \
    g++ \
    git \
    help2man \
    libfl-dev \
    make \
    ninja-build \
    perl \
    python3 \
    python3-pip \
    python3-pytest \
    python3-yaml \
    wget \
    xz-utils \
    zlib1g-dev \
 && python3 -m pip install --break-system-packages --no-cache-dir jsonschema==4.25.1 \
 && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "v${VERILATOR_VERSION}" https://github.com/verilator/verilator.git /tmp/verilator \
 && cd /tmp/verilator \
 && autoconf \
 && ./configure \
 && make -j"${VERILATOR_JOBS}" \
 && make install \
 && cd / \
 && rm -rf /tmp/verilator

RUN if getent passwd 1000 >/dev/null; then \
      existing_user="$(getent passwd 1000 | cut -d: -f1)"; \
      usermod -l agent -d /home/agent -m "${existing_user}"; \
    else \
      useradd -m -u 1000 -s /bin/bash agent; \
    fi \
 && mkdir -p /home/agent/noc_build /home/agent/noc_sim_build /home/agent/.cache/ccache \
 && chown -R 1000:1000 /home/agent

USER agent
WORKDIR /workspace

ENV BUILD_ROOT=/home/agent/noc_build
ENV PYTHON3=python3
ENV VERILATOR=verilator

RUN git config --global --add safe.directory /workspace \
 && git config --global --add safe.directory /home/agent/workspace

CMD ["sleep", "infinity"]
