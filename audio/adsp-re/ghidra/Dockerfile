# SPDX-License-Identifier: GPL-2.0-or-later
# REA + Ghidra (mounted at /opt/ghidra) for offline Hexagon ADSP firmware RE.
FROM ubuntu:latest
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      openjdk-21-jdk-headless procps ca-certificates jq file python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=node:24 /usr/local/bin/node /usr/local/bin/node
COPY --from=node:24 /usr/local/lib/node_modules /usr/local/lib/node_modules
RUN ln -s ../lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
 && ln -s ../lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx \
 && npm install -g rea-agents@6.5.0 && npm cache clean --force
ENV GHIDRA_INSTALL_DIR=/opt/ghidra \
    JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
    HOME=/work/.home
WORKDIR /work
