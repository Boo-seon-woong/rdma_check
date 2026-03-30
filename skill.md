일반 노드 간 rdma 통신을 확인하기 위해 작은 단위의 데이터를 송수신하는 rdma connection check용 디렉토리

config file에 세부사항을 정할 수 있어야 하며, 이미 rdma infiniband fabric에 붙어있는 rdma 사이에서 connection check을 진행할 것이기에 IPoIB를 이용한 확인방법은 사용을 금함

송수신은 단순 소규모 패킷부터 대규모 벤치마크까지 다양히 하며, 이는 config file에서 구분한다.

코드규모는 최대한 쉽고 단순하고 짧게.

RDMA 데이터 경로 외 별도 연결은 기본적으로 두지 않되, QP 정보 교환을 위한 최소 TCP bootstrap은 허용한다. 단, control IP는 IPoIB가 아닌 별도 관리망 주소를 사용한다.
