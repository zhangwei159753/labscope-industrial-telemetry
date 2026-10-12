FROM debian:bookworm-slim AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ git libjsoncpp-dev libpq-dev libssl-dev libsodium-dev \
    uuid-dev libc-ares-dev ninja-build pkg-config zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --depth 1 --branch v1.9.13 --recurse-submodules --shallow-submodules \
    https://github.com/drogonframework/drogon.git /opt/drogon-master/drogon-master
COPY . /src/labscope
RUN cmake -S /src/labscope -B /src/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DDROGON_SOURCE_DIR=/opt/drogon-master/drogon-master \
      -DBUILD_TESTING=OFF -DLABSCOPE_MQTT_TLS=ON \
    && cmake --build /src/build --target lab_telemetry --parallel 2

FROM debian:bookworm-slim AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libc-ares2 libjsoncpp25 libpq5 libssl3 libsodium23 libuuid1 wget zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 labscope \
    && useradd --system --uid 10001 --gid labscope --home-dir /app labscope \
    && mkdir -p /app/data/mqtt /app/web \
    && chown -R labscope:labscope /app
COPY --from=build /src/build/lab_telemetry /usr/local/bin/lab_telemetry
WORKDIR /app
USER 10001:10001
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/lab_telemetry"]
CMD ["8080"]
