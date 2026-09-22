# Build a static binary, ship it in an empty image: no shell, no libc, no
# package manager, which means a small attack surface and a ~1 MB image.
FROM gcc:13 AS build
WORKDIR /src
COPY . .
RUN make test && make static

FROM scratch
COPY --from=build /src/sip-cnf /sip-cnf
USER 65532:65532
EXPOSE 5060/udp 8080/tcp
ENTRYPOINT ["/sip-cnf"]
