## Build DPDK 20.11.0 (tested with RDMA v22)

### testPMD (NOTE: macswap is pinned to core 10. Can be changed in ./app/test-pmd/macswap.c. Only work with 1 NIC port) use macswap-libdpdk-do_macswap-working.c for ./app/test-pmd/macswap.c
```
cd dpdk-pipeline-macswap
meson setup build
cd build
ninja
sudo ninja install
cd .. && export RTE_SDK=$(pwd)
sudo ./build/app/dpdk-testpmd -l 0,1 -n 4 -- --portmask=0x1 --forward-mode=macswap --txpkts=64 --rxpkts=64 --txd=1024 --rxd=1024 --stats-period 1
```
client
```
sudo ./build/app/dpdk-testpmd -l 0-19 -n 4 -a 40:00.1 -a 40:00.0 --socket-mem=4096,0,0,0 -- --portmask=0x1 --socket-num=0 --burst=64 --txd=4096 --rxd=4096 --mbcache=512 --rxq=19 --txq=19 --nb-cores=19 -a --rss-ip --no-numa --forward=txonly --txonly-multi-flow --txpkts=64 --stats-period 1
```


### Memcached (NOTE: process_through_memcached is pinned to core 5. Can be changed in ./memcached/memcached.c)
```
cd memcached_dpdk_pipeline
mkdir build
cd build
cmake ..
make
sudo ../memcached/memcached -u root -m 10240
```
client
```
cd build
sudo ./memcached_client --server_mac="08:C0:EB:BF:EE:B6" --batching=16 --dataset_size=2000000 --dataset_key_size="10-100-0.9" --dataset_val_size="10-100-0.5" --populate_workload_size=2000000 --workload_config="2000000-0.8" --check_get_correctness=false
```

CPU_SET({cpuid}, &cpuset);
