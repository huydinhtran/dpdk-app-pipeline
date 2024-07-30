## build DPDK 20.11.0 (tested with RDMA v22)
```
cd dpdk-pipeline-macswap
meson setup build
cd build
ninja
sudo ninja install
cd .. && export RTE_SDK=$(pwd)
```
NOTE: macswap is pinned to core 10. Can be changed in ./app/test-pmd/macswap.c
