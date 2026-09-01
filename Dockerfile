# syntax=docker/dockerfile:1.7
FROM debian:bookworm-slim AS build

ARG OPEN62541_VERSION=v1.5.0
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates cmake git ninja-build python3 wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch ${OPEN62541_VERSION} https://github.com/open62541/open62541.git
RUN cmake -S open62541 -B /build/open62541 -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/open62541 \
      -DBUILD_SHARED_LIBS=OFF \
      -DUA_ENABLE_PUBSUB=ON \
      -DUA_ENABLE_PUBSUB_INFORMATIONMODEL=ON \
      -DUA_ENABLE_AMALGAMATION=OFF \
      -DUA_BUILD_EXAMPLES=OFF \
      -DUA_BUILD_UNIT_TESTS=OFF \
    && cmake --build /build/open62541 --parallel \
    && cmake --install /build/open62541

RUN git clone --depth 1 --branch v2.6.0 https://github.com/CLIUtils/CLI11.git
# 构建并安装 CLI11，生成 CLI11Config.cmake（find_package(CLI11) 需要）
RUN cmake -S /src/CLI11 -B /build/CLI11 -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCLI11_BUILD_TESTS=OFF \
      -DCLI11_BUILD_EXAMPLES=OFF \
    && cmake --build /build/CLI11 --parallel \
    && cmake --install /build/CLI11 --prefix /opt/CLI11
COPY . /src/TsnHub
RUN cmake -S /src/TsnHub -B /build/tsnhub -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DTSNHUB_USE_OPEN62541_PUBSUB=ON \
      -DTSNHUB_BUILD_TESTS=ON \
      -DCMAKE_PREFIX_PATH="/opt/open62541;/opt/CLI11" \
    && cmake --build /build/tsnhub --parallel \
    && ctest --test-dir /build/tsnhub --output-on-failure

FROM debian:bookworm-slim AS runtime
RUN useradd --system --uid 10001 --create-home tsnhub
COPY --from=build /build/tsnhub/TsnHub /usr/local/bin/TsnHub
USER tsnhub
EXPOSE 4841/udp 4842/udp
ENTRYPOINT ["TsnHub"]
CMD ["--mode", "native", "--subscribe-url", "opc.udp://0.0.0.0:4841", "--publish-url", "opc.udp://127.0.0.1:4842"]
