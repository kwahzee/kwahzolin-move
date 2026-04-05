FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu \
    make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
CMD ["./scripts/build.sh"]
