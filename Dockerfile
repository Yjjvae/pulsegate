FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
# Standard build arguments keep dependency downloads usable behind a corporate
# proxy. Values are supplied only by the caller and are not copied to runtime.
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libboost-dev \
        ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY include ./include
COPY src ./src
COPY app ./app

RUN cmake --preset release \
    && cmake --build --preset release --parallel

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 pulsegate \
    && useradd --system --uid 10001 --gid pulsegate \
        --home-dir /nonexistent --shell /usr/sbin/nologin pulsegate

COPY --from=build /src/build/release/app/pulsegate /usr/local/bin/pulsegate
COPY config/pulsegate.docker.yaml /etc/pulsegate/config.yaml

USER 10001:10001
EXPOSE 8080
STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/local/bin/pulsegate"]
CMD ["--config", "/etc/pulsegate/config.yaml"]

FROM ubuntu:24.04 AS mock-upstream

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --system --gid 10001 mock \
    && useradd --system --uid 10001 --gid mock \
        --home-dir /nonexistent --shell /usr/sbin/nologin mock

COPY --chmod=0555 tools/mock_upstream.py /usr/local/bin/mock_upstream.py

USER 10001:10001
EXPOSE 9000

ENTRYPOINT ["python3", "/usr/local/bin/mock_upstream.py"]
