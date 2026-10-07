# syntax=docker/dockerfile:1.7
#
# 86Box-Next: isp-server in a container -- the virtual ISP and telephone
# exchange that 86Box-Next's modems dial, with its status page.  Built from
# the repository's root (it needs src/char/modem_voice.c beside it):
#
#   docker build -f isp-server/Dockerfile -t xeon3d/86box-next-isp .
#   docker run -d --name isp -p 2323:2323 -p 2324:2324 -v isp-data:/data xeon3d/86box-next-isp
#
# Then open http://<host>:2324/ and make the first user (the super admin)
# with the setup token from `docker logs isp`.  Modems dial <host>:2323.
#
# The same build gives a fully static Linux binary (musl), with no container:
#
#   docker build -f isp-server/Dockerfile --target linux-binary --output out .

ARG ALPINE=3.22

# ------------------------------------------------------------------ build --
FROM alpine:${ALPINE} AS build
RUN apk add --no-cache build-base cmake ninja pkgconf meson linux-headers \
        glib-dev glib-static zlib-dev zlib-static pcre2-dev pcre2-static libffi-dev gettext-static

# libslirp, static: Alpine packages only the shared one.
ADD --checksum=sha256:3970542143b7c11e6a09a4d2b50f30a133473c41f15ed0bdcc3b7a1c450d9a5c \
    https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v4.9.1/libslirp-v4.9.1.tar.gz /tmp/libslirp.tar.gz
RUN tar xzf /tmp/libslirp.tar.gz -C /tmp && cd /tmp/libslirp-v4.9.1 \
    && meson setup build --prefix=/usr --default-library=static --buildtype=release \
    && ninja -C build install

WORKDIR /src
COPY src/char/modem_voice.c src/char/
COPY src/include/86box/modem_voice.h src/include/86box/
COPY isp-server/ isp-server/
RUN cmake -S isp-server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=ON -DBUILD_TESTING=ON \
    && cmake --build /build \
    && cd /build && ctest --output-on-failure \
    && strip /build/isp-server \
    && /build/isp-server --help >/dev/null

# ----------------------------------------------- the static Linux binary --
FROM scratch AS linux-binary
COPY --from=build /build/isp-server /isp-server

# ---------------------------------------------------------- the container --
FROM alpine:${ALPINE}
LABEL org.opencontainers.image.title="86Box-Next ISP" \
      org.opencontainers.image.description="The virtual ISP and telephone exchange for 86Box-Next's modems: PPP with PAP, CHAP and MS-CHAP, MPPE, compression, Multilink, NAT, and a status page with logins." \
      org.opencontainers.image.source="https://github.com/Xeon3D/86Box-Next" \
      org.opencontainers.image.licenses="GPL-2.0-or-later"
RUN apk add --no-cache tini \
    && adduser -D -H -u 10086 isp \
    && mkdir /data && chown isp:isp /data
COPY --from=build /build/isp-server /usr/local/bin/isp-server
USER isp
VOLUME /data
# Modems (PPP and the telephone exchange), and the status page.
EXPOSE 2323 2324
HEALTHCHECK --interval=30s --timeout=5s CMD wget -q -O /dev/null http://127.0.0.1:2324/api/session || exit 1
# Settings, accounts and users live in /data/isp-server.ini.  Both ports on
# every interface: the page asks for logins once there is a user.  Further
# options (--auth accounts, --account NAME:PASSWORD, --mppe ...) may follow
# `docker run ... xeon3d/86box-next-isp`.
ENTRYPOINT ["/sbin/tini", "--", "isp-server", "--no-open", "--config", "/data/isp-server.ini", \
            "--listen", "0.0.0.0", "--http-listen", "0.0.0.0"]
