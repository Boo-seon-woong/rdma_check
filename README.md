# rdma_check

InfiniBand RC QP를 직접 연결해서 `SEND/RECV`만 확인하는 최소 구현이다.

- RDMA 데이터 경로: `libibverbs`
- bootstrap: 최소 TCP 1회
- fabric: InfiniBand LID 기반만 지원
- client는 server가 미리 등록한 remote region 안의 지정 slot에만 `RDMA_WRITE` 한다

## Build

```bash
make
```

## Config

지원 키는 아래만 남겼다.

- `mode=server|client`
- `workload=check|bench`
- `control_host=<server management ip or 0.0.0.0>`
- `control_port=<tcp port>`
- `ib_device=<ibp23s0 같은 RDMA device>`
- `ib_port=1`
- `message_size=<bytes>`
- `iterations=<count>`
- `queue_depth=<count>`

`workload=check`는 자동으로 `iterations=1`, `queue_depth=1`로 고정된다.

`queue_depth`는 server가 미리 열어 두는 remote slot 수다.
client는 `slot = seq % queue_depth` 규칙으로 같은 지정 영역들만 반복해서 덮어쓴다.

## Verified Setup

- server `genie`: `ibp23s0`, management IP `10.20.26.87`
- client `simba`: `ibp111s0`
- bootstrap port: `7301`

## Run

```bash
./bin/rdma_check config/check_server.conf
./bin/rdma_check config/check_client.conf
./bin/rdma_check config/bench_server.conf
./bin/rdma_check config/bench_client.conf
```
