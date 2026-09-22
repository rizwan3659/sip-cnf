# sip-cnf

A small SIP network function in C, built to behave well on Kubernetes. The
SIP side is deliberately simple: stateless answers to OPTIONS and a REGISTER
stub. The point of the repo is everything around it, which decides whether
a telecom function survives rolling upgrades, node drains and autoscaling
without dropping traffic.

- **One thread, one epoll loop** for SIP over UDP, HTTP probes and metrics,
  and signals (via `signalfd`, so signal handling never interrupts
  half-finished work).
- **Kubernetes probes:** `/healthz` for liveness and `/readyz` for readiness.
  They are kept separate on purpose. A draining pod is *not ready*, so it
  leaves the Service, but it is still *alive*, so it isn't restarted.
- **Graceful drain:** on `SIGTERM`, readiness flips to 503 at once, SIP keeps
  being answered for `DRAIN_SECONDS` while endpoint removal spreads through
  the cluster, then the process exits 0. A second signal exits immediately.
- **Prometheus metrics:** `sip_requests_total{method}`,
  `sip_responses_total{code}`, `sip_malformed_total`, `draining`,
  `uptime_seconds`.
- **Container image:** a static binary in a `scratch` image, running as a
  non-root user with a read-only root filesystem and no Linux capabilities.
- **Rollout safety:** `maxUnavailable: 0`, a `preStop` pause,
  `terminationGracePeriodSeconds` longer than the drain, a PodDisruptionBudget
  (`minAvailable: 2`), topology spread across nodes, and an HPA on CPU.

> This is a clean-room, small-scale version of the operational patterns
> from the cloud-native IMS work I led at C-DOT (microservices and
> containerised IMS functions on Kubernetes). It contains no C-DOT code or
> configuration.

## Build and run

```sh
make test              # SIP responder unit tests + pod lifecycle test
make asan
make static            # static binary for the scratch image
SIP_PORT=5060 HTTP_PORT=8080 DRAIN_SECONDS=10 ./sip-cnf
docker build -t sip-cnf .
kubectl apply -f deploy/sip-cnf.yaml
```

The lifecycle test (`tests/test_lifecycle.c`) runs the real binary through
what the kubelet does:

```
ready → SIGTERM → /readyz 503, /healthz 200, SIP still answered → exit 0 after the drain
```

## What has and hasn't been verified

| Item | Status |
| --- | --- |
| SIP responder, lifecycle, drain timing | Tested locally and in CI (gcc and clang, ASan/UBSan) |
| Static build | Built locally (~850 KB) |
| Docker image | Built and smoke-tested in CI (`docker stop` must exit 0); no Docker daemon was available locally |
| Kubernetes manifests | Schema-validated in CI with kubeconform; **not yet run on a cluster** |

## Design notes

- **Why drain at all?** When a pod is deleted, the kubelet sends SIGTERM
  while kube-proxy and ingress are still removing the pod's endpoints. For a
  short window traffic can still arrive, and exiting immediately drops it.
  `preStop` plus the drain cover that window.
- **Why UDP SIP is load-balanced per packet.** A stateless responder doesn't
  care which pod answers. A stateful SIP function would need affinity by
  Call-ID or dialog, which a plain Service does not give you.
- **Why `scratch`?** The image contains nothing but the binary, so there is
  no shell to exploit and far fewer CVEs to track.

MIT licensed.
