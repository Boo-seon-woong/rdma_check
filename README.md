# rdma_check

`rdma_check`는 InfiniBand fabric 위에서 RC QP를 직접 연결한 뒤 `SEND/RECV`로 RDMA 연결을 확인하는 최소 도구다.

- RDMA 데이터 경로는 `libibverbs`만 사용한다.
- QP 정보 교환은 아주 작은 TCP bootstrap으로만 수행한다.
- `control_host`에는 IPoIB 주소가 아니라 별도 관리망 IP를 넣어야 한다.
- `workload=check|bench`를 config에서 구분한다.

## Build

```bash
make
```

## Config

형식은 단순 `key=value`다.

필수 키:

- `mode=server|client`
- `control_host=<server management ip or 0.0.0.0>`
- `control_port=<tcp port>`
- `ib_device=<mlx5_0 같은 RDMA device>`

주요 선택 키:

- `workload=check|bench`
- `ib_port=1`
- `gid_index=-1`
- `service_level=0`
- `message_size=32`
- `iterations=1`
- `queue_depth=1`
- `validate=true`

`workload=check`는 내부적으로 `iterations=1`, `queue_depth=1`, `validate=true`로 고정된다.

## Run

서버:

```bash
./bin/rdma_check config/check_server.conf
```

클라이언트:

```bash
./bin/rdma_check config/check_client.conf
```

대용량 스트리밍 벤치마크:

```bash
./bin/rdma_check config/bench_server.conf
./bin/rdma_check config/bench_client.conf
```

## Notes

- 샘플 config의 `10.0.0.10`, `mlx5_0` 값은 실제 환경에 맞게 바꿔야 한다.
- `gid_index=-1`이면 LID 기반으로 연결한다.
- RoCE가 아니라 InfiniBand fabric 전제를 두고 있다.
- 클라이언트는 전송 시간과 처리량을 출력하고, 서버는 수신 검증 결과를 출력한다.
