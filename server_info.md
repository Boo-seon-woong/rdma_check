#### Server Genie(native)

- opensm status
    
    ```jsx
    seonung@genie:~/2026/tee-dist$ systemctl status opensm
    ● opensm.service - Starts the OpenSM InfiniBand fabric Subnet Managers
         Loaded: loaded (/usr/lib/systemd/system/opensm.service; enabled; preset: enabled)
         Active: active (exited) since Tue 2026-03-24 10:27:47 UTC; 2 days ago
           Docs: man:opensm(8)
       Main PID: 7993 (code=exited, status=0/SUCCESS)
            CPU: 20ms
    
    Mar 24 10:27:47 genie systemd[1]: Starting opensm.service - Starts the OpenSM InfiniBand fabric Subnet Managers...
    Mar 24 10:27:47 genie sh[7993]: Starting opensm on following ports: 0x58a2e10300086f3c
    Mar 24 10:27:47 genie systemd[1]: Finished opensm.service - Starts the OpenSM InfiniBand fabric Subnet Managers.
    ```
    
- ibstat result
    
    ```jsx
    seonung@genie:~/2026/tee-dist$ ibstat
    CA 'ibp23s0'
            CA type: MT4129
            Number of ports: 1
            Firmware version: 28.34.1002
            Hardware version: 0
            Node GUID: 0x58a2e10300086f3c
            System image GUID: 0x58a2e10300086f3c
            Port 1:
                    State: Active
                    Physical state: LinkUp
                    Rate: 200
                    Base lid: 2
                    LMC: 0
                    SM lid: 2
                    Capability mask: 0xa651e84a
                    Port GUID: 0x58a2e10300086f3c
                    Link layer: InfiniBand
    CA 'roceo1'
            CA type: Broadcom NetXtreme-C/E RoCE Driver HCA
            Number of ports: 1
            Firmware version: 226.0.145.0
            Hardware version: 0x14e4
            Node GUID: 0x7ec255fffe871f04
            System image GUID: 0x7ec255fffe871f04
            Port 1:
                    State: Active
                    Physical state: LinkUp
                    Rate: 2.5
                    Base lid: 0
                    LMC: 0
                    SM lid: 0
                    Capability mask: 0x001d0000
                    Port GUID: 0x7ec255fffe871f04
                    Link layer: Ethernet
    CA 'roceo2'
            CA type: Broadcom NetXtreme-C/E RoCE Driver HCA
            Number of ports: 1
            Firmware version: 226.0.145.0
            Hardware version: 0x14e4
            Node GUID: 0x7ec255fffe871f05
            System image GUID: 0x7ec255fffe871f05
            Port 1:
                    State: Down
                    Physical state: Disabled
                    Rate: 2.5
                    Base lid: 0
                    LMC: 0
                    SM lid: 0
                    Capability mask: 0x001d0000
                    Port GUID: 0x7ec255fffe871f05
                    Link layer: Ethernet
    ```
    
- rdma link
    
    ```jsx
    seonung@genie:~/2026/tee-dist$ rdma link
    link ibp23s0/1 subnet_prefix fe80:0000:0000:0000 lid 2 sm_lid 2 lmc 0 state ACTIVE physical_state LINK_UP 
    link roceo1/1 state ACTIVE physical_state LINK_UP netdev eno1np0 
    link roceo2/1 state DOWN physical_state DISABLED netdev eno2np1
    ```
    
    ibp23s0 lid 2 sm_lid 2
    

---

#### Server Simba(native)

- ibstat result
    
    ```jsx
    seonung@simba:~/2026$ ibstat
    CA 'ibp111s0'
            CA type: MT4129
            Number of ports: 1
            Firmware version: 28.34.1002
            Hardware version: 0
            Node GUID: 0x58a2e10300086efc
            System image GUID: 0x58a2e10300086efc
            Port 1:
                    State: Active
                    Physical state: LinkUp
                    Rate: 200
                    Base lid: 1
                    LMC: 0
                    SM lid: 2
                    Capability mask: 0xa651e848
                    Port GUID: 0x58a2e10300086efc
                    Link layer: InfiniBand
    ```
    
- rdma link
    
    ```jsx
    seonung@simba:~/2026$ rdma link
    link ibp111s0/1 subnet_prefix fe80:0000:0000:0000 lid 1 sm_lid 2 lmc 0 state ACTIVE physical_state LINK_UP
    ```
    
    ibp111s0 lid 1 sm_lid 2