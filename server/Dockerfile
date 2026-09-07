# Tarquin server image: Go binary + otter-vox (Audio8 TTS on ggml) + Audio8 weights from the PikaOS PPA.
# STT runs in a separate parakeet-server container (see docker-compose.yml).

FROM golang:1.24-bookworm AS build
WORKDIR /src
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/tarquin ./cmd/tarquin

FROM debian:sid-slim
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
# PikaOS PPA provides otter-vox (+ model) — glibc >= 2.43 hence debian:sid
COPY deploy/pika-keyring.gpg.key /etc/apt/keyrings/pika-keyring.gpg.key
RUN printf 'Types: deb\nURIs: https://ppa.pika-os.com/\nSuites: pika\nComponents: cockatiel\nSigned-By: /etc/apt/keyrings/pika-keyring.gpg.key\n' \
      > /etc/apt/sources.list.d/pika.sources \
    && apt-get update \
    && apt-get install -y --no-install-recommends otter-vox otter-vox-model libvulkan1 mesa-vulkan-drivers \
    && rm -rf /var/lib/apt/lists/*
# piper (rhasspy) — fast native TTS, default engine. British male voices from rhasspy/piper-voices.
ARG PIPER_VERSION=2023.11.14-2
RUN mkdir -p /opt/piper/voices && cd /opt/piper \
    && curl -fsSL "https://github.com/rhasspy/piper/releases/download/${PIPER_VERSION}/piper_linux_x86_64.tar.gz" | tar xz --strip-components=1 \
    && ln -s /opt/piper/piper /usr/local/bin/piper \
    && for v in alan/medium/en_GB-alan-medium northern_english_male/medium/en_GB-northern_english_male-medium; do \
         f=$(basename "$v"); d=$(dirname "$v"); \
         curl -fsSL -o "/opt/piper/voices/$f.onnx" "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/$d/$f.onnx"; \
         curl -fsSL -o "/opt/piper/voices/$f.onnx.json" "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/$d/$f.onnx.json"; \
       done
COPY --from=build /out/tarquin /usr/local/bin/tarquin
# extra voices (e.g. tarquin.codes + tarquin.txt) are read from the model dir's voices/ folder
COPY deploy/voices/ /usr/share/otter-shell/models/vox/audio8/voices/
RUN useradd -r -m -d /var/lib/tarquin tarquin && mkdir -p /data && chown tarquin /data
USER tarquin
ENV OTTER_HOST=0.0.0.0 OTTER_PORT=8480 OTTER_DATA_DIR=/data OTTER_STT_URL=http://stt:8080 \
    OTTER_TTS_ENGINE=piper OTTER_PIPER_MODEL=/opt/piper/voices/en_GB-northern_english_male-medium.onnx \
    OTTER_VOX_BIN=/usr/bin/otter-vox OTTER_VOX_ARGS="--cpu --cpu-threads 8" HOME=/var/lib/tarquin \
    MESA_SHADER_CACHE_DIR=/data/mesa_cache XDG_CACHE_HOME=/data/cache
EXPOSE 8480
HEALTHCHECK --interval=30s --timeout=5s --start-period=90s CMD curl -fsS http://127.0.0.1:8480/health || exit 1
ENTRYPOINT ["/usr/local/bin/tarquin"]
