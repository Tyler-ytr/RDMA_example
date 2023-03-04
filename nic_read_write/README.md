<!--
 * @Descripttion: 
 * @version: 
 * @Author: tylerytr
 * @Date: 2022-10-30 22:25:57
 * @LastEditTime: 2023-03-04 15:37:45
 * @LastEditors: tylerytr
 * @FilePath: /mytest/nic_cas/README.md
 * Email:601576661@qq.com
 * Copyright (c) 2022 by tyleryin, All Rights Reserved. 
-->
# RDMA 片上内存操作案例
编译库的需求：`libibverbs `
编译参数:GCC <文件名>  -o service  -libverbs
运行方式：
1. 有IB网络支持：
       服务端：./service
       客户端：./service 服务端IP
 2. 走ROCE:
       服务端：./service   -g  0
       客户端：./service -g 0  服务端IP

服务端(!config.server_name)开辟片上内存并注册为memory region，客户端(config.server_name)对其读写;
目前内存区域的大小是128KB(kLockChipMemSize),可以根据nic_mem显示的大小自行修改;
## 对应接口
1. ibv_exp_alloc_dm()/ibv_exp_free_dm() - to allocate/free device memory
2. ibv_exp_reg_mr - to register the allocated device memory buffer as a memory region and get a memory key for local/remote access by the device(将已分配的设备内存缓冲区注册为一个内存区域，并获得一个用于设备本地/远程访问的内存区域(mr))
3. ibv_exp_memcpy_dm - to copy data to/from a device memory buffer(从/往设备内存拷贝数据)
4. ibv_post_send/ibv_post_receive - to request the device to perform a send/receive operation using the memory key

## 样例流程
1. 接受输入变量，初始化自定义结构体resource；
2. 创建资源(`resources_create`)，实际上是创建系统资源，初始化RDMA设备，注册到自定义结构体resource变量res中；
      1. 客户端：根据服务器名称进行socket连接，存到res里面；服务端：在指定port上面进行监听并且存入res；
      2. 获取RDMA设备列表(`ibv_get_device_list`)，根据列表得到可用的设备
      3. 打开1中的设备(`ibv_open_device`)，获取设备上下文存入res；释放设备列表(`ibv_free_device_list`)
      4. 查询RDMA设备端口信息(`ibv_query_port`)
      5. 分配一个Protection Domain(`ibv_alloc_pd`)
      6. 创建一个Complete Queue（`ibv_create_cq`）
      7. 分配内存空间，在片上内存中开辟空间(`ibv_exp_alloc_dm`),把对应指针存入resource的mydm中;并且注册内存区域(`ibv_exp_reg_mr`);通过buffer往on-chip内存中写入`MSG`(`ibv_exp_memcpy_dm`);
      8. 创建Queue pair(`ibv_create_qp`)

3. 连接QP，注意第2步结束的时候QP状态是RESET，
   1. 用sock同步信息，包括rkey,addr,qp_num,lid,gid；
   2. 转换QP状态，从RESET到INIT，进入rtr之后可以进行接受处理；
      1. 这部分初始化了qp状态，设置了端口号，pkey_indx,qp_access_flags；
   3. 从INIT转换到RTR
   4. 如果是客户端，post_receive，创建接受任务；
   5. 从RTR转换到RTS
   6. 使用socket发送Q保证双方处于可以连接的状态；

4.  进行数据收发：(这里的buf对于服务端指的是片上内存上面的内容,对于客户端指的是一般开辟的内存空间)
    1.  如果是服务器，因为3.4客户端创建了接受任务，所以可以使用send，这里使用`IBV_WR_SEND`进行send操作；
    2.  两端进行轮询拉取数据
    3.  如果是客户端，输出buf里面的数据；如果是服务端，往自己的buf里面放东西RDMAMSGR；
    4.  使用sokcet发送R保证双方可以连接；
    5.  如果是客户端：
        1. 使用RDMA read操作读取server端的buf;
        2. 轮询拉取数据，输出数据
        3. 使用RDMA WRITE发送数据
        4. 轮询拉去数据
    6.  如果是服务端：
        1. 输出对应的片上内存内容(show_onchip_memory)
5. 释放各类资源

## 实验室实验命令
1. 使用ROCE：
   1. 服务端`./service   -g  0`
   2. 客户端`./service -g 0 192.168.42.10` `192.168.42.10表示服务端的IP地址，用show_gids看`

2. 抓到RDMA数据包的方法:
   1. 升级tcpdump到4.99.1及以上版本(最新版)
		git clone https://github.com/the-tcpdump-group/tcpdump 
		git clone https://github.com/the-tcpdump-group/libpcap
		确保上面两个在同一个文件夹里面，进libpcap 根据它github的INSTALL.md进行操作，无需make install;然后进入tcpdump根据INSTALL.md操作即可;
		
	2. sudo tcpdump -i mlx5_1 -w RDMA1.cap
      这里的mlx5_1来自于ibv_devinfo；不能用ifconfigure里面的那个网卡号（不然抓不到ROCE协议只能看见TCP)
   1. 启动服务端以及客户端

## 片上内存使用参考
1. https://docs.nvidia.com/networking/display/MLNXOFEDv461000/Programming
2. https://github.com/thustorage/Sherman/blob/main/src/rdma/Resource.cpp#L139
