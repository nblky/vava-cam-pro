# Build context: the vava-cam-pro repository root.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config libssl-dev libsqlite3-dev libavformat-dev libavcodec-dev libswresample-dev libavutil-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY shared ./shared
COPY myvava-sdk ./myvava-sdk
COPY third_party ./third_party
COPY vava-server ./vava-server
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel \
      -DVAVA_BUILD_AGENT=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr \
    && cmake --build build -j2 \
    && cmake --install build --strip --prefix /stage/usr

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 libsqlite3-0 ffmpeg ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 vava && useradd --uid 10001 --gid vava --no-create-home vava
COPY --from=build /stage/usr/ /usr/
COPY vava-server/deploy/run-server.sh /usr/local/bin/vava-server-run
ENV VAVA_SERVER_BIN=/usr/bin/vava-server VAVA_WEB_DIR=/usr/share/vava-server/web
USER 10001:10001
EXPOSE 3731/udp 8443/tcp
ENTRYPOINT ["/usr/local/bin/vava-server-run"]
