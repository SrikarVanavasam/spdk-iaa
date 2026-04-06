下面是整理好的 **Markdown 版 README**，可以直接复制给另一个 chat，或者保存成 `README_fio_phase1.md`。

````markdown
# fio Phase 1 Engine for SNIC + IAA + NVMe-oF

## 1. 这个是干什么的

这是一个 **fio external ioengine**，用于把 fio 生成的 workload 接到我们现有的 **SNIC client API** 上，从而用 fio 来 benchmark 这条端到端数据路径：

**fio → external engine → snic_client API → SNIC server → IAA compress/decompress → NVMe-oF target**

当前这个 Phase 1 版本的目标是：

- 先让 fio 能调用我们现有的 API
- 先跑通 write/read benchmark
- 先拿到端到端性能结果

它**不是**一个标准文件系统 I/O engine，也**不是**普通块设备 ioengine。  
它只是一个 **wrapper**，把 fio 的 `offset + xfer_buf + xfer_buflen` 转成我们自己的：

- `lba`
- `buf`
- `len`
- `req_id`

---

## 2. 当前架构

### Host 端
fio 通过 external engine 调用：

- `snic_client_init()`
- `snic_client_write()`
- `snic_client_read()`
- `snic_client_poll()`

### SNIC 端
SNIC server 收到请求后：

- 调 IAA 做 compress / decompress
- 再通过 SPDK/NVMe-oF 对后端 target 做读写

### Target 端
后端是一个 SPDK NVMe-oF target。

---

## 3. 当前目录结构

当前约定目录如下：

```text
app/nvmf_iaa/
├── client/
│   └── snic_client_lib.c
├── fio/
│   └── fio 源码树
├── fio_engine/
│   ├── fio_snic_phase1.c
│   ├── build.sh
│   ├── write_phase1.fio
│   └── read_phase1.fio
├── snic/
│   └── SNIC server
├── snic_client.h
└── nvmf_iaa.h
````

关键路径：

* fio engine: `app/nvmf_iaa/fio_engine/`
* client library: `app/nvmf_iaa/client/snic_client_lib.c`
* 公共头文件：`app/nvmf_iaa/snic_client.h`, `app/nvmf_iaa/nvmf_iaa.h`

---

## 4. Phase 1 的设计特点

当前 Phase 1 是一个 **同步 external engine**：

* `queue()` 里直接 submit
* 然后在 engine 内部阻塞等待 completion
* 只适合 `iodepth=1`

### 当前支持

* `read`
* `write`

### 当前不建议

* `iodepth > 1`
* `numjobs > 1`
* `trim`
* `verify`
* `randrw`
* 复杂文件语义

### 当前使用 bounce buffer

为了避免 fio 自己分配的 `xfer_buf` 和我们当前 client 内存模型不匹配，Phase 1 先使用 engine 内部 bounce buffer。

---

## 5. hugepages 怎么分配

### 5.1 查看当前 hugepages

```bash
grep -i Huge /proc/meminfo
mount | grep hugetlbfs
ls -ld /dev/hugepages
```

如果看到：

* `HugePages_Total: 0`
* `HugePages_Free: 0`

说明还没有可用 hugepages。

### 5.2 分配 2MB hugepages

先分 1024 个 2MB hugepages（约 2GB）：

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep -i Huge /proc/meminfo
```

如果需要按 NUMA node0 分：

```bash
echo 1024 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
cat /sys/devices/system/node/node0/hugepages/hugepages-2048kB/free_hugepages
```

### 5.3 挂载 hugetlbfs

如果没挂载：

```bash
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs nodev /dev/hugepages -o pagesize=2M
```

### 5.4 修复 `/dev/hugepages` 权限

如果 DPDK 报：

* `open '/dev/hugepages/spdk0map_0' failed: Permission denied`

执行：

```bash
sudo rm -f /dev/hugepages/spdk*map*
sudo chmod 1777 /dev/hugepages
```

如果权限由 mount 选项控制，可以 remount：

```bash
sudo mount -o remount,mode=1777 /dev/hugepages
```

---

## 6. target 和 SNIC 都要启动吗

**要。两个都要启动。**

### target 必须启动

因为 `snic_client_init()` 会先连接后端 NVMe-oF target：

* `target_ip`
* `target_port`
* `subnqn=nqn.2016-06.io.spdk:cnode1`

如果 target 没启动，fio 一开始就在 init 阶段失败。

### SNIC 也必须启动

因为 fio engine 最终调的是：

* `snic_client_write()`
* `snic_client_read()`

这些请求要发给 SNIC server。

### 启动顺序建议

1. 先启动 target
2. 再启动 SNIC
3. 最后运行 fio

---

## 7. 怎么编译

### 7.1 依赖路径

当前使用：

* `FIO_DIR=/home/xuanboj2/spdk-iaa/app/nvmf_iaa/fio`
* `SPDK_DIR=/fast-lab-share/srikarv2/spdk-iaa-x86`

### 7.2 build.sh 的核心思路

编译时使用：

* fio 头文件
* 本项目头文件
* SPDK 头文件
* `pkg-config` 获取 SPDK/DPDK link flags
* `rpath` 指向 SPDK 和 DPDK 的 build/lib

### 7.3 build.sh 关键点

* 使用：

  * `pkg-config --cflags spdk_nvme spdk_env_dpdk spdk_rdma_provider`
  * `pkg-config --libs spdk_nvme spdk_env_dpdk spdk_rdma_provider`
* 额外加：

  * `-lrdmacm -libverbs -luuid -lcrypto -lssl -ldl -lpthread -lnuma -lm`
* 链接时加：

  * `-Wl,--no-as-needed`
  * `-Wl,-rpath,<spdk build lib>`
  * `-Wl,-rpath,<dpdk build lib>`

### 7.4 编译命令

在 `app/nvmf_iaa/fio_engine/` 下：

```bash
./build.sh
```

不要用 `sudo ./build.sh`。

---

## 8. 怎么运行

### 8.1 先导出运行时库路径

```bash
export LD_LIBRARY_PATH=/fast-lab-share/srikarv2/spdk-iaa-x86/build/lib:/fast-lab-share/srikarv2/spdk-iaa-x86/dpdk/build/lib:$LD_LIBRARY_PATH
```

### 8.2 运行 fio

用当前项目里的 fio，不要先用系统 `/usr/local/bin/fio`：

```bash
~/spdk-iaa/app/nvmf_iaa/fio/fio write_phase1.fio
```

或带状态输出：

```bash
~/spdk-iaa/app/nvmf_iaa/fio/fio --status-interval=2 write_phase1.fio
```

---

## 9. 推荐的 Phase 1 job file 设置

为了减少 fake-file 语义带来的干扰，建议 job file 里加：

```ini
invalidate=0
direct=0
create_on_open=0
end_fsync=0
fsync_on_close=0
```

一个更适合调试的 write job 例子：

```ini
[global]
ioengine=external:./snic_phase1.so
thread=1
group_reporting=1
time_based=1
runtime=5
ramp_time=0
iodepth=1
numjobs=1

rw=write
bs=128k
size=1m
filename=dummy

invalidate=0
direct=0
create_on_open=0
end_fsync=0
fsync_on_close=0

snic_ip=192.168.200.11
target_ip=192.168.200.20
target_port=4420
wq_path=/dev/iax/wq1.0
lba_shift=9
max_xfer_size=2097152
poll_usleep=1000
verbose=0

[job0]
```

---

## 10. 当前已知限制

### 10.1 这是同步 engine

当前是同步 wrapper：

* `queue()` submit 后阻塞等待 completion
* 只适合 `iodepth=1`

### 10.2 `read` 依赖先前 `write`

当前 server 端 read 路径依赖内存里的：

* `g_lba_comp_len[]`
* `g_lba_orig_len[]`

所以必须先 write，再 read。

### 10.3 fake file 语义还不完整

当前 `filename=dummy` 只是为了让 fio 生成 offset。
因为 engine 不是真正基于 OS fd 的文件引擎，所以要通过：

* `invalidate=0`
* `direct=0`

等参数减少 fio 对 dummy file 的额外文件操作。

### 10.4 当前日志打印会严重影响 benchmark

`client/snic_client_lib.c` 里的 `snic_client_poll()` 原本会打印：

* `Completion Received!`
* `dump_iax_cr()`
* `dump_iax_cr_raw()`

这些日志要关掉，否则会严重拖慢 benchmark。

---

## 11. 当前必须注意的代码点

### 11.1 要改 `client/snic_client_lib.c`

如果 completion 还在疯狂打印，改的是：

```text
app/nvmf_iaa/client/snic_client_lib.c
```

不是只改 `fio_snic_phase1.c`。

### 11.2 `snic_client_fini()` 目前只有声明，没有实现

目前确认：

* `snic_client.h` 里有声明
* 但工程里没有真正实现

所以 Phase 1 里要么：

* 暂时注释掉 `snic_client_fini()` 调用
* 要么后续补一个真正 cleanup 实现

---

## 12. 常见报错与含义

### `engine ./snic_phase1.so not loadable`

含义：external engine `.so` 没有成功被 fio `dlopen()`。

常见原因：

* `.so` 依赖没链全
* `LD_LIBRARY_PATH` 没带 SPDK/DPDK 库
* 运行时用错了 fio binary
* `.so` 里有 unresolved symbol

### `Bad option <snic_ip=...>`

含义：不是 option 真错，而是 **engine 没 load 成功**，fio 不认识 engine 自定义参数。

### `Failed to initialize DPDK`

含义：hugepages 没准备好，或者 `/dev/hugepages` 权限有问题。

### `open '/dev/hugepages/spdk0map_0' failed: Permission denied`

含义：`/dev/hugepages` 权限不对，或者有 root 残留文件。

修法：

```bash
sudo rm -f /dev/hugepages/spdk*map*
sudo chmod 1777 /dev/hugepages
```

### `RDMA_CM_EVENT_REJECTED`

含义：client 连后端 NVMe-oF target 时被拒绝。

要检查：

* target 是否启动
* `192.168.200.20:4420` 是否监听
* NQN 是否是 `nqn.2016-06.io.spdk:cnode1`

### `cache invalidation of dummy failed: Bad file descriptor`

含义：fio 还把 `dummy` 当真实文件做 cache invalidation，但 engine 没有真实文件 fd。

Phase 1 的缓解方式：

```ini
invalidate=0
direct=0
create_on_open=0
end_fsync=0
fsync_on_close=0
```

---

## 13. 当前已经确认成功的东西

已经确认：

* external fio engine 可以成功编译
* `.so` 可以成功加载
* fio 可以识别 engine 自定义参数
* hugepages / DPDK 环境可以初始化
* target attach 可以成功
* SNIC RDMA connect 可以成功
* setup message 可以成功发送
* write path 可以收到 completion，IAA CR 显示 SUCCESS

所以当前 Phase 1 已经不是“能不能接起来”的问题，而是：

* 怎么把 benchmark 跑得更干净
* 怎么减少 fake-file 副作用
* 怎么为后续 async Phase 2 做准备

---

## 14. 推荐下一步

1. 关闭 `snic_client_poll()` 里的 completion/CR 打印
2. 用小 workload 跑通：

   * `runtime=5`
   * `size=1m`
   * `bs=128k`
   * `iodepth=1`
3. 确认 write 能稳定跑完
4. 再跑 read
5. 最后再考虑：

   * async engine
   * `iodepth>1`
   * 更规范的 file semantics
   * 更完整的 cleanup (`snic_client_fini`)

```

如果你要，我下一条可以把它再压缩成一个更短的“交接版”，专门给另一个 chat 快速读。
```
