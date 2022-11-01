<!--
 * @Descripttion: 
 * @version: 
 * @Author: tylerytr
 * @Date: 2022-10-30 22:25:57
 * @LastEditTime: 2022-11-01 19:56:33
 * @LastEditors: tylerytr
 * @FilePath: /tyleryin/worker/test/RDMA-EXAMPLE/mytest/README.md
 * Email:601576661@qq.com
 * Copyright (c) 2022 by tyleryin, All Rights Reserved. 
-->
# Mellanox RDMA文档中的样例以及抓包分析
编译库的需求：`libibverbs `
编译参数:GCC <文件名>  -o service  -libverbs
运行方式：
1. 有IB网络支持：
       服务端：./service
       客户端：./service 服务端IP
 2. 走ROCE:
       服务端：./service   -g  0
       客户端：./service -g 0  服务端IP

## 样例流程
1. 接受输入变量，初始化自定义结构体resource；
2. 创建资源(`resources_create`)，实际上是创建系统资源，初始化RDMA设备，注册到自定义结构体resource变量res中；
      1. 客户端：根据服务器名称进行socket连接，存到res里面；服务端：在指定port上面进行监听并且存入res；
      2. 获取RDMA设备列表(`ibv_get_device_list`)，根据列表得到可用的设备
      3. 打开1中的设备(`ibv_open_device`)，获取设备上下文存入res；释放设备列表(`ibv_free_device_list`)
      4. 查询RDMA设备端口信息(`ibv_query_port`)
      5. 分配一个Protection Domain(`ibv_alloc_pd`)
      6. 创建一个Complete Queue（`ibv_create_cq`）
      7. 分配内存空间，注册Mmeory Region来存放数据（`ibv_reg_mr`）
      8. 创建Queue pair(`ibv_create_qp`)

3. 连接QP，注意第2步结束的时候QP状态是RESET，
   1. 用sock同步信息，包括rkey,addr,qp_num,lid,gid；
   2. 转换QP状态，从RESET到INIT，进入rtr之后可以进行接受处理；
      1. 这部分初始化了qp状态，设置了端口号，pkey_indx,qp_access_flags；
   3. 从INIT转换到RTR
   4. 如果是客户端，post_receive，创建接受任务；
   5. 从RTR转换到RTS
   6. 使用socket发送Q保证双方处于可以连接的状态；

4.  进行数据收发：
    1.  如果是服务器，因为3.4客户端创建了接受任务，所以可以使用send，这里使用`IBV_WR_SEND`进行send操作；
    2.  两端进行轮询拉取数据
    3.  如果是客户端，输出buf里面的数据；如果是服务端，往自己的buf里面放东西RDMAMSGR；
    4.  使用sokcet发送R保证双方可以连接；
    5.  如果是客户端：
        1. 使用RDMA read操作读取server端的buf；
        2. 轮询拉取数据，输出数据
        3. 使用RDMA WRITE发送数据
        4. 轮询拉去数据
    6.  如果是服务端：
        1. 输出buf数据
5. 释放各类资源

