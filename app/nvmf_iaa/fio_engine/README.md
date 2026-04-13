Set up:

```
./configure --with-rdma --with-fio=/path/to/fio/ 

on target:
sudo ./start_target.sh
```



To run fio baseline:

```
on client /spdk-iaa/app/nvmf_iaa/fio_engine/:
sudo LD_PRELOAD=build/fio/spdk_nvme ~/fio/fio run_baseline.fio
```



To run iaa-nic perf:

``` 
on client /spdk-iaa/app/nvmf_iaa/fio_engine/:
./clean.sh
./build.sh
RW=write ./run_bandwidth.sh 128k
```

