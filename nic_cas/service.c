#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>

#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#define MAX_POLL_CQ_TIMEOUT 2000
#define MSG "SEND operation "
#define RDMAMSGR "RDMA read operation "
#define RDMAMSGW "RDMA write operation"
#define MSG_SIZE (strlen(MSG) + 1)

#if __BYTE_ORDER == __LITTLE_ENDIAN
static inline uint64_t htonll(uint64_t x) { return bswap_64(x); }
static inline uint64_t ntohll(uint64_t x) { return bswap_64(x); }
#elif __BYTE_ORDER == __BIG_ENDIAN
static inline uint64_t htonll(uint64_t x) { return x; }
static inline uint64_t ntohll(uint64_t x) { return x; }
#else
#error __BYTE_ORDER is neither __LITTLE_ENDIAN nor __BIG_ENDIAN
#endif
/* structure of test parameters */
struct config_t
{
	const char *dev_name; /* IB 设备名称 */
	char *server_name;	/* 服务器名称 */
	u_int32_t tcp_port;   /* 服务器TCP端口号*/
	int ib_port;		  /* IB端口号 */
	int gid_idx;		  /* 使用的gid */
};
/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
	uint64_t addr;   /* Buffer address */
	uint32_t rkey;   /* Remote key */
	uint32_t qp_num; /* QP number */
	uint16_t lid;	/* LID of the IB port */
	uint8_t gid[16]; /* gid */
}__attribute__((packed));//参数表示取消内存对齐，或者说按照一字节对齐

/* structure of system resources */
struct resources
{
	struct ibv_device_attr
		device_attr;
	/* Device attributes */
	struct ibv_port_attr port_attr;	/* IB port attributes */
	struct cm_con_data_t remote_props; /* values to connect to remote side */
	struct ibv_context *ib_ctx;		   /* device handle */
	struct ibv_pd *pd;				   /* PD handle */
	struct ibv_cq *cq;				   /* CQ handle */
	struct ibv_qp *qp;				   /* QP handle */
	struct ibv_mr *mr;				   /* MR handle for buf */
	char *buf;						   /* memory buffer pointer, used for RDMA and send
ops */
	int sock;						   /* TCP socket file descriptor */
};
struct config_t config = {
	NULL,  /* dev_name */
	NULL,  /* server_name */
	19873, /* tcp_port */
	1,	 /* ib_port */
	-1 /* gid_idx */
};
/******************************************************************************
Socket operations
For simplicity, the example program uses TCP sockets to exchange control
information. If a TCP/IP stack/connection is not available, connection manager
(CM) may be used to pass this information. Use of CM is beyond the scope of
this example
******************************************************************************/
/******************************************************************************
* Function: sock_connect
*
* Input
* servername: URL of server to connect to (NULL for server mode)
* port: port of service
*
* Output
* none
*
* Returns
* socket (fd) on success, negative error code on failure
*
* Description
* 连接socket，如果指定了服务器名称，那么会启动一个client连接到指定的服务器以及端口；
* 否则会在指定窗口上监听可能进入的连接。
* Connect a socket. If servername is specified a client connection will be
* initiated to the indicated server and port. Otherwise listen on the
* indicated port for an incoming connection.
*
******************************************************************************/
static int sock_connect(const char *servername, int port){
    struct addrinfo *resolved_addr = NULL;
	struct addrinfo *iterator;
    char service[6];
	int sockfd = -1;
	int listenfd = 0;
	int tmp;
	struct addrinfo hints =
		{
			.ai_flags = AI_PASSIVE,
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM /* Sequenced, reliable, connection-based byte streams.  */
        };

	if (sprintf(service, "%d", port) < 0)
		goto sock_connect_exit;

	/* Resolve DNS address, use sockfd as temp storage */
    //参考：https://www.cnblogs.com/fnlingnzb-learner/p/7542770.html
    //以及https://blog.csdn.net/drdairen/article/details/52794043
    //https://man7.org/linux/man-pages/man3/getaddrinfo.3.html
    //servername:IP地址，如果是null那么适合作为主机，service:端口号,hint里面是配置，结果在resolved_addr里面是一个链表，通过ai_next连接
	//返回值0表示成功，非0表示出错
    sockfd = getaddrinfo(servername, service, &hints, &resolved_addr);  
	
       
	if (sockfd < 0)
	{
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(sockfd), servername, port);
		goto sock_connect_exit;
	}   

	/* 遍历resolved_addr链表，如果是服务端就绑定 */
	for (iterator = resolved_addr; iterator; iterator = iterator->ai_next)
	{
		sockfd = socket(iterator->ai_family, iterator->ai_socktype, iterator->ai_protocol);
		if (sockfd >= 0)
		{
			if (servername){
				/* Client mode. Initiate connection to remote */
				if ((tmp = connect(sockfd, iterator->ai_addr, iterator->ai_addrlen)))
				{
					fprintf(stdout, "failed connect \n");
					close(sockfd);
					sockfd = -1;
				}
            }
			else
			{
					/* Server mode. Set up listening socket an accept a connection */
					listenfd = sockfd;
					sockfd = -1;
					if (bind(listenfd, iterator->ai_addr, iterator->ai_addrlen))
						goto sock_connect_exit;
					listen(listenfd, 1);
					sockfd = accept(listenfd, NULL, 0);
			}
		}
	}
sock_connect_exit:
    //这部分不确定为啥要关闭listenfd有可能这块的意思是一次只和一个client建立连接；
	if (listenfd)
		close(listenfd);
	if (resolved_addr)
		freeaddrinfo(resolved_addr);
	if (sockfd < 0)
	{
		if (servername)
			fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		else
		{
			perror("server accept");
			fprintf(stderr, "accept() failed\n");
		}
	}
	return sockfd;
}
/******************************************************************************
* Function: sock_sync_data
*
* Input
* sock socket to transfer data on 要传输的socket
* xfer_size size of data to transfer 要传输的数据大小
* local_data pointer to data to be sent to remote 指向要传输数据的指针
*
* Output
* remote_data pointer to buffer to receive remote data 指向接受数据buffer的指针
*
* Returns
* 0 on success, negative error code on failure 
*
* Description
* 跨套接字同步数据。指定的本地数据会被发送到远端，然后它会等待远端把数据发回来。
* 它假设双方同步并且用正确的数据调用这个函数，不然会出现混乱
* Sync data across a socket. The indicated local data will be sent to the
* remote. It will then wait for the remote to send its data back. It is
* assumed that the two sides are in sync and call this function in the proper
* order. Chaos will ensue if they are not. :)
*
* 另外这是一个阻塞的函数会等待从远端接受完整的数据。
* Also note this is a blocking function and will wait for the full data to be
* received from the remote.
*
******************************************************************************/
int sock_sync_data(int sock, int xfer_size, char *local_data, char *remote_data)
{
	int rc;
	int read_bytes = 0;
	int total_read_bytes = 0;
	rc = write(sock, local_data, xfer_size);
	if (rc < xfer_size)
		fprintf(stderr, "Failed writing data during sock_sync_data\n");
	else
		rc = 0;
	while (!rc && total_read_bytes < xfer_size)
	{
		read_bytes = read(sock, remote_data, xfer_size);
		if (read_bytes > 0)
			total_read_bytes += read_bytes;
		else
			rc = read_bytes;
	}
	return rc;
}
/******************************************************************************
End of socket operations
******************************************************************************/
/* poll_completion */
/******************************************************************************
* Function: poll_completion
*
* Input
* res pointer to resources structure
*
* Output
* none
*
* Returns
* 0 on success, 1 on failure
*
* Description
* 轮询完成队列中的单个事件，函数会轮询队列直到过了MAX_POLL_CQ_TIMEOUT milliseconds
* Poll the completion queue for a single event. This function will continue to
* poll the queue until MAX_POLL_CQ_TIMEOUT milliseconds have passed.
*
******************************************************************************/
static int poll_completion(struct resources *res)
{
    
	struct ibv_wc wc;
	unsigned long start_time_msec;
	unsigned long cur_time_msec;
	struct timeval cur_time;
	int poll_result;
	int rc = 0;
	/* poll the completion for a while before giving up of doing it .. */
	gettimeofday(&cur_time, NULL);
	start_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	do
	{
		poll_result = ibv_poll_cq(res->cq, 1, &wc);
		gettimeofday(&cur_time, NULL);
		cur_time_msec = (cur_time.tv_sec * 1000) + (cur_time.tv_usec / 1000);
	} while ((poll_result == 0) && ((cur_time_msec - start_time_msec) < MAX_POLL_CQ_TIMEOUT));
	if (poll_result < 0)
	{
		/* poll CQ failed */
		fprintf(stderr, "poll CQ failed\n");
		rc = 1;
	}
	else if (poll_result == 0)
	{ /* the CQ is empty */
		fprintf(stderr, "completion wasn't found in the CQ after timeout\n");
		rc = 1;
	}
	else
	{
		/* CQE found */
		fprintf(stdout, "completion was found in CQ with status 0x%x\n", wc.status);
		/* check the completion status (here we don't care about the completion opcode */
		if (wc.status != IBV_WC_SUCCESS)
		{
			fprintf(stderr, "got bad completion with status: 0x%x, vendor syndrome: 0x%x\n", wc.status,
					wc.vendor_err);
			rc = 1;
		}
	}
	return rc;
}
/******************************************************************************
* Function: post_send
*
* Input
* res pointer to resources structure
* opcode IBV_WR_SEND, IBV_WR_RDMA_READ or IBV_WR_RDMA_WRITE
*
* Output
* none
*
* Returns
* 0 on success, error code on failure
*
* Description
* This function will create and post a send work request
******************************************************************************/
static int post_send(struct resources *res, int opcode)
{
    //创建发送任务ibv_send_wr
	struct ibv_send_wr sr;
	struct ibv_sge sge;
	struct ibv_send_wr *bad_wr = NULL;
	int rc;
	/* 
    把要发送的数据的内存地址，大小，密钥告诉HCA
    prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;
	sge.length = MSG_SIZE;
	sge.lkey = res->mr->lkey;
	/* prepare the send work request */
	memset(&sr, 0, sizeof(sr));
	sr.next = NULL;
	sr.wr_id = 0;
	sr.sg_list = &sge;
	sr.num_sge = 1;
	sr.opcode = opcode;
	sr.send_flags = IBV_SEND_SIGNALED;
    //Read/Write还需要告诉HCA远程的内存地址和密钥
	if (opcode != IBV_WR_SEND)
	{
		sr.wr.rdma.remote_addr = res->remote_props.addr;
		sr.wr.rdma.rkey = res->remote_props.rkey;
	}
	/* there is a Receive Request in the responder side, so we won't get any into RNR flow */
	rc = ibv_post_send(res->qp, &sr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post SR\n");
	else
	{
		switch (opcode)
		{
		case IBV_WR_SEND:
			fprintf(stdout, "Send Request was posted\n");
			break;
		case IBV_WR_RDMA_READ:
			fprintf(stdout, "RDMA Read Request was posted\n");
			break;
		case IBV_WR_RDMA_WRITE:
			fprintf(stdout, "RDMA Write Request was posted\n");
			break;
		default:
			fprintf(stdout, "Unknown Request was posted\n");
			break;
		}
	}
	return rc;
}
/******************************************************************************
* Function: post_receive
*
* Input
* res pointer to resources structure
*
* Output
* none
*
* Returns
* 0 on success, error code on failure
*
* Description
*
******************************************************************************/
static int post_receive(struct resources *res)
{
    // 创建接收任务ibv_resv_wr
	struct ibv_recv_wr rr;//reciver work request
	struct ibv_sge sge;
	struct ibv_recv_wr *bad_wr;//pointer to first rejected WR
	int rc;
	/* prepare the scatter/gather entry */
	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)res->buf;//buffer的位置
	sge.length = MSG_SIZE;//buffer的长度
	sge.lkey = res->mr->lkey;//local key (lkey) of buffer from ibv_reg_mr
	/* prepare the receive work request */
	memset(&rr, 0, sizeof(rr));
	rr.next = NULL;//指向下一个WR
	rr.wr_id = 0;//WR 的 id
	rr.sg_list = &sge;//Scatter Gather List 分散聚合表，每一个SGE(Scatter/Gather Element)都是一个数据段
	rr.num_sge = 1;//sg_list里面元素的数量
	/* 
    把Receive Request发布到RQ，这个里面包含了接受缓冲区
    post the Receive Request to the RQ */
	rc = ibv_post_recv(res->qp, &rr, &bad_wr);
	if (rc)
		fprintf(stderr, "failed to post RR\n");
	else
		fprintf(stdout, "Receive Request was posted\n");
	return rc;
}
/******************************************************************************
* Function: resources_init
*
* Input
* res pointer to resources structure
*
* Output
* res is initialized
*
* Returns
* none
*
* Description
* res is initialized to default values
******************************************************************************/
static void resources_init(struct resources *res)
{
	memset(res, 0, sizeof *res);
	res->sock = -1;
}
/******************************************************************************
* Function: resources_create
*
* Input
* res pointer to resources structure to be filled in
*
* Output
* res filled in with resources
*
* Returns
* 0 on success, 1 on failure
*
* Description
*
* This function creates and allocates all necessary system resources. These
* are stored in res.
*****************************************************************************/
static int resources_create(struct resources *res){
	struct ibv_device **dev_list = NULL;
	struct ibv_qp_init_attr qp_init_attr;
	struct ibv_device *ib_dev = NULL;
	size_t size;
	int i;
	int mr_flags = 0;
	int cq_size = 0;
	int num_devices;
	int rc = 0;//0表示资源创建失败，1表示成功 

    /* if client side */
    if (config.server_name){
        res->sock = sock_connect(config.server_name, config.tcp_port);
		if (res->sock < 0)
		{
			fprintf(stderr, "failed to establish TCP connection to server %s, port %d\n",
					config.server_name, config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}

    }
    else{
        fprintf(stdout, "waiting on port %d for TCP connection\n", config.tcp_port);
        res->sock = sock_connect(NULL, config.tcp_port);
    	if (res->sock < 0)
		{
			fprintf(stderr, "failed to establish TCP connection with client on port %d\n",
					config.tcp_port);
			rc = -1;
			goto resources_create_exit;
		}   
    }
	fprintf(stdout, "TCP connection was established\n");
	fprintf(stdout, "searching for IB devices in host\n");
    //获取RDMA设备列表
    dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list)
	{
		fprintf(stderr, "failed to get IB devices list\n");
		rc = 1;
		goto resources_create_exit;
	}
	/* if there isn't any IB device in host */
	if (!num_devices)
	{
		fprintf(stderr, "found %d device(s)\n", num_devices);
		rc = 1;
		goto resources_create_exit;
	}
	fprintf(stdout, "found %d device(s)\n", num_devices);    
	/* search for the specific device we want to work with */
	for (i = 0; i < num_devices; i++)
	{
		if (!config.dev_name)
		{
			config.dev_name = strdup(ibv_get_device_name(dev_list[i]));
			fprintf(stdout, "device not specified, using first one found: %s\n", config.dev_name);
		}
		if (!strcmp(ibv_get_device_name(dev_list[i]), config.dev_name))
		{
			ib_dev = dev_list[i];
			break;
		}
	}
	/* if the device wasn't found in host */
	if (!ib_dev)
	{
		fprintf(stderr, "IB device %s wasn't found\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}
	/* 打开设备，获取设备上下文 */
	res->ib_ctx = ibv_open_device(ib_dev);
	if (!res->ib_ctx)
	{
		fprintf(stderr, "failed to open device %s\n", config.dev_name);
		rc = 1;
		goto resources_create_exit;
	}
	/* 释放RDMA设备列表占用的资源 */
	ibv_free_device_list(dev_list);
	dev_list = NULL;
	ib_dev = NULL;
	/* 查询RDMA设备端口信息 */
	if (ibv_query_port(res->ib_ctx, config.ib_port, &res->port_attr))
	{
		fprintf(stderr, "ibv_query_port on port %u failed\n", config.ib_port);
		rc = 1;
		goto resources_create_exit;
	}
	/* 分配一个Protection Domain */
	res->pd = ibv_alloc_pd(res->ib_ctx);
	if (!res->pd)
	{
		fprintf(stderr, "ibv_alloc_pd failed\n");
		rc = 1;
		goto resources_create_exit;
	}
	/*创建Completion Queue;
     each side will send only one WR, so Completion Queue with 1 entry is enough */
	cq_size = 1;
	res->cq = ibv_create_cq(res->ib_ctx, cq_size, NULL, NULL, 0);
	if (!res->cq)
	{
		fprintf(stderr, "failed to create CQ with %u entries\n", cq_size);
		rc = 1;
		goto resources_create_exit;
	}
	/* 注册Mmeory Region来存放数据
       分配内存空间 */
	size = MSG_SIZE;
	res->buf = (char *)malloc(size);
	if (!res->buf)
	{
		fprintf(stderr, "failed to malloc %Zu bytes to memory buffer\n", size);
		rc = 1;
		goto resources_create_exit;
	}
	memset(res->buf, 0, size);
	/* only in the server side put the message in the memory buffer */
	if (!config.server_name)
	{
		strcpy(res->buf, MSG);
		fprintf(stdout, "going to send the message: '%s'\n", res->buf);
	}
	else
		memset(res->buf, 0, size);
    /* register the memory buffer */
	//注册on-chip片上内存，
	//非片上内存版本:
	// mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	// res->mr = ibv_reg_mr(res->pd, res->buf, size, mr_flags);
	//1. 从设备Allocate on-chip memory
	struct ibv_exp_alloc_dm_attr dm_attr;
	memset(&dm_attr, 0, sizeof(dm_attr));
	const uint64_t kLockChipMemSize = 128 * 1024;//片上内存大小
	const uint64_t kLockStartAddr = 0;//片上内存起始地址
	uint64_t mmSize=kLockChipMemSize;
	uint64_t mm=kLockStartAddr;
	dm_attr.length = mmSize;
	struct ibv_exp_dm *dm = ibv_exp_alloc_dm(res->ib_ctx, &dm_attr);
	if (!dm) {
		fprintf(stderr,"Allocate on-chip memory failed");
		goto resources_create_exit;
  	}
	//2. 注册内存区域
	/* Device memory registration as memory region */
	struct ibv_exp_reg_mr_in mr_in;
	memset(&mr_in, 0, sizeof(mr_in));
	mr_in.pd = res->pd, mr_in.addr = (void *)mm, mr_in.length = mmSize,
    mr_in.exp_access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC,

	mr_in.create_flags = 0;
	mr_in.dm = dm;
	mr_in.comp_mask = IBV_EXP_REG_MR_DM;
	struct ibv_mr *mr = ibv_exp_reg_mr(&mr_in);
	if (!mr) {
		fprintf(stderr,"Memory registration failed");
		goto resources_create_exit;
  	}
	//3. 往内存拷贝数据(清0)，其中ibv_exp_dm_memcpy_dir cpy_attr中的两个数值表示了是设备里面拷贝(IBV_EXP_DM_CPY_TO_DEVICE),还是拷贝到宿主(IBV_EXP_DM_CPY_TO_HOST)
	char *buffer = (char *)malloc(mmSize);
 	memset(buffer, 0, mmSize);
	
	struct ibv_exp_memcpy_dm_attr cpy_attr;
	memset(&cpy_attr, 0, sizeof(cpy_attr));
	cpy_attr.memcpy_dir = IBV_EXP_DM_CPY_TO_DEVICE;
	cpy_attr.host_addr = (void *)buffer;
	cpy_attr.length = mmSize;
	cpy_attr.dm_offset = 0;
	ibv_exp_memcpy_dm(dm, &cpy_attr);
	free(buffer);

	res->mr=mr;


	if (!res->mr)
	{
		fprintf(stderr, "ibv_reg_mr failed with mr_flags=0x%x\n", mr_flags);
		rc = 1;
		goto resources_create_exit;
	}
	fprintf(stdout, "MR was registered with addr=%p, lkey=0x%x, rkey=0x%x, flags=0x%x\n",
			res->buf, res->mr->lkey, res->mr->rkey, mr_flags);
    
	/* 创建Queue pair */
	memset(&qp_init_attr, 0, sizeof(qp_init_attr));
	qp_init_attr.qp_type = IBV_QPT_RC;
	qp_init_attr.sq_sig_all = 1;
	qp_init_attr.send_cq = res->cq;
	qp_init_attr.recv_cq = res->cq;
	qp_init_attr.cap.max_send_wr = 1;
	qp_init_attr.cap.max_recv_wr = 1;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	res->qp = ibv_create_qp(res->pd, &qp_init_attr);
	if (!res->qp)
	{
		fprintf(stderr, "failed to create QP\n");
		rc = 1;
		goto resources_create_exit;
	}
	fprintf(stdout, "QP was created, QP number=0x%x\n", res->qp->qp_num);
resources_create_exit:
	if (rc)
	{
		/* Error encountered, cleanup */
		if (res->qp)
		{
			ibv_destroy_qp(res->qp);
			res->qp = NULL;
		}
		if (res->mr)
		{
			ibv_dereg_mr(res->mr);
			res->mr = NULL;
		}
		if (res->buf)
		{
			free(res->buf);
			res->buf = NULL;
		}
		if (res->cq)
		{
			ibv_destroy_cq(res->cq);
			res->cq = NULL;
		}
		if (res->pd)
		{
			ibv_dealloc_pd(res->pd);
			res->pd = NULL;
		}
		if (res->ib_ctx)
		{
			ibv_close_device(res->ib_ctx);
			res->ib_ctx = NULL;
		}
		if (dev_list)
		{
			ibv_free_device_list(dev_list);
			dev_list = NULL;
		}
		if (res->sock >= 0)
		{
			if (close(res->sock))
				fprintf(stderr, "failed to close socket\n");
			res->sock = -1;
		}
	}
	return rc;
}
/******************************************************************************
* Function: modify_qp_to_init
*
* Input
* qp QP to transition
*
* Output
* none
*
* Returns
* 0 on success, ibv_modify_qp failure code on failure
*
* Description
* QP刚创建时状态为RESET
* 把QP从RESET状态转换到INIT状态
* Transition a QP from the RESET to INIT state
******************************************************************************/
static int modify_qp_to_init(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_INIT;
	attr.port_num = config.ib_port;
	attr.pkey_index = 0;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
	flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to INIT\n");
	return rc;
}
/******************************************************************************
* Function: modify_qp_to_rtr
*
* Input
* qp QP to transition
* remote_qpn remote QP number
* dlid destination LID
* dgid destination GID (mandatory for RoCEE)
*
* Output
* none
*
* Returns
* 0 on success, ibv_modify_qp failure code on failure
*
* Description
* Transition a QP from the INIT to RTR state, using the specified QP number
******************************************************************************/
static int modify_qp_to_rtr(struct ibv_qp *qp, uint32_t remote_qpn, uint16_t dlid, uint8_t *dgid)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_256;
	attr.dest_qp_num = remote_qpn;
	attr.rq_psn = 0;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 0x12;
	attr.ah_attr.is_global = 0;
	attr.ah_attr.dlid = dlid;
	attr.ah_attr.sl = 0;
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num = config.ib_port;
	if (config.gid_idx >= 0)
	{
		attr.ah_attr.is_global = 1;
		attr.ah_attr.port_num = 1;
		memcpy(&attr.ah_attr.grh.dgid, dgid, 16);
		attr.ah_attr.grh.flow_label = 0;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.sgid_index = config.gid_idx;
		attr.ah_attr.grh.traffic_class = 0;
	}
	flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTR\n");
	return rc;
}
/******************************************************************************
* Function: modify_qp_to_rts
*
* Input
* qp QP to transition
*
* Output
* none
*
* Returns
* 0 on success, ibv_modify_qp failure code on failure
*
* Description
* Transition a QP from the RTR to RTS state
******************************************************************************/
static int modify_qp_to_rts(struct ibv_qp *qp)
{
	struct ibv_qp_attr attr;
	int flags;
	int rc;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 0x12;
	attr.retry_cnt = 6;
	attr.rnr_retry = 0;
	attr.sq_psn = 0;
	attr.max_rd_atomic = 1;
	flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
	rc = ibv_modify_qp(qp, &attr, flags);
	if (rc)
		fprintf(stderr, "failed to modify QP state to RTS\n");
	return rc;
}
/******************************************************************************
* Function: connect_qp
*
* Input
* res pointer to resources structure
*
* Output
* none
*
* Returns
* 0 on success, error code on failure
*
* Description
* 连接QP，把服务端的状态变成RTR，发送端变成RTS
* Connect the QP. Transition the server side to RTR, sender side to RTS
******************************************************************************/
static int connect_qp(struct resources *res){
	
    struct cm_con_data_t local_con_data;
	struct cm_con_data_t remote_con_data;
	struct cm_con_data_t tmp_con_data;
	int rc = 0;
	char temp_char;
	union ibv_gid my_gid;
	if (config.gid_idx >= 0)
	{
        //根据给定的gid的index修改ibv_gid结构体my_gid。0表示成功，-1表示失败
		rc = ibv_query_gid(res->ib_ctx, config.ib_port, config.gid_idx, &my_gid);
		if (rc)
		{
			fprintf(stderr, "could not get gid for port %d, index %d\n", config.ib_port, config.gid_idx);
			return rc;
		}
	}
    else{
		memset(&my_gid, 0, sizeof my_gid);
    }
    /*使用TCP socket来交换连接QP必要的信息，自己的信息在local_con_data里面，
    远端写的信息在tmp_con_data里面，之后再复制到remote_con_data里面*/
    local_con_data.addr = htonll((uintptr_t)res->buf);
	local_con_data.rkey = htonl(res->mr->rkey);
	local_con_data.qp_num = htonl(res->qp->qp_num);
	local_con_data.lid = htons(res->port_attr.lid);    
    memcpy(local_con_data.gid, &my_gid, 16);
    fprintf(stdout, "\nLocal LID = 0x%x\n", res->port_attr.lid);
	if (sock_sync_data(res->sock, sizeof(struct cm_con_data_t), (char *)&local_con_data, (char *)&tmp_con_data) < 0)
	{
		fprintf(stderr, "failed to exchange connection data between sides\n");
		rc = 1;
		goto connect_qp_exit;
	}
	remote_con_data.addr = ntohll(tmp_con_data.addr);
	remote_con_data.rkey = ntohl(tmp_con_data.rkey);
	remote_con_data.qp_num = ntohl(tmp_con_data.qp_num);
	remote_con_data.lid = ntohs(tmp_con_data.lid);
    memcpy(remote_con_data.gid, tmp_con_data.gid, 16);
    /* save the remote side attributes, we will need it for the post SR */
	res->remote_props = remote_con_data;
	fprintf(stdout, "Remote address = 0x%" PRIx64 "\n", remote_con_data.addr);
	fprintf(stdout, "Remote rkey = 0x%x\n", remote_con_data.rkey);
	fprintf(stdout, "Remote QP number = 0x%x\n", remote_con_data.qp_num);
	fprintf(stdout, "Remote LID = 0x%x\n", remote_con_data.lid);
    
    if (config.gid_idx >= 0)
	{
		uint8_t *p = remote_con_data.gid;
		fprintf(stdout, "Remote GID =%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n ",p[0],
				  p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}
    
    //转换QP状态
    /*
    刚刚创建好的QP状态是RESET,
    INIT之后就可以调用ibv_post_recv提交一个receive buffer了
    当 QP进入RTR(ready to receive)状态以后，便开始进行接收处理
    RTR之后便可以转为RTS(ready to send)，RTS状态下可以调用ibv_post_send
    */
    /*
    RESET   Newly created, queues empty.
    INIT    Basic information set. Ready for posting to receive queue.
    RTR     Ready to Receive. Remote address info set for connected QPs,QP may now receive packets.
    RTS     Ready to Send. Timeout and retry parameters set, QP may now send packets.
    */
    //RESET -> INIT
    rc = modify_qp_to_init(res->qp);
    if (rc)
	{
		fprintf(stderr, "change QP state to INIT failed\n");
		goto connect_qp_exit;
	}
	/* let the client post RR to be prepared for incoming messages */
	if (config.server_name)
	{
		rc = post_receive(res);
		if (rc)
		{
			fprintf(stderr, "failed to post RR\n");
			goto connect_qp_exit;
		}
	}
    //INIT -> RTR(Ready To Receive)
    rc = modify_qp_to_rtr(res->qp, remote_con_data.qp_num, remote_con_data.lid, remote_con_data.gid);
	if (rc)
	{
		fprintf(stderr, "failed to modify QP state to RTR\n");
		goto connect_qp_exit;
	}
    //RTR -> RTS(Ready To Send)
	rc = modify_qp_to_rts(res->qp);
	if (rc)
	{
		fprintf(stderr, "failed to modify QP state to RTR\n");
		goto connect_qp_exit;
	}
	fprintf(stdout, "QP state was change to RTS\n");
	/* 
    同步以确保双方都处于可以连接的状态以防止数据包丢失
    sync to make sure that both sides are in states that they can connect to prevent packet loose */
	if (sock_sync_data(res->sock, 1, "Q", &temp_char)) /* just send a dummy char back and forth */
	{
		fprintf(stderr, "sync error after QPs are were moved to RTS\n");
		rc = 1;
	}
    connect_qp_exit:
	return rc;
}


/******************************************************************************
* Function: resources_destroy
*
* Input
* res pointer to resources structure
*
* Output
* none
*
* Returns
* 0 on success, 1 on failure
*
* Description
* Cleanup and deallocate all resources used
******************************************************************************/
static int resources_destroy(struct resources *res)
{
	int rc = 0;
	if (res->qp)
		if (ibv_destroy_qp(res->qp))
		{
			fprintf(stderr, "failed to destroy QP\n");
			rc = 1;
		}
	if (res->mr)
		if (ibv_dereg_mr(res->mr))
		{
			fprintf(stderr, "failed to deregister MR\n");
			rc = 1;
		}
	if (res->buf)
		free(res->buf);
	if (res->cq)
		if (ibv_destroy_cq(res->cq))
		{
			fprintf(stderr, "failed to destroy CQ\n");
			rc = 1;
		}
	if (res->pd)
		if (ibv_dealloc_pd(res->pd))
		{
			fprintf(stderr, "failed to deallocate PD\n");
			rc = 1;
		}
	if (res->ib_ctx)
		if (ibv_close_device(res->ib_ctx))
		{
			fprintf(stderr, "failed to close device context\n");
			rc = 1;
		}
	if (res->sock >= 0)
		if (close(res->sock))
		{
			fprintf(stderr, "failed to close socket\n");
			rc = 1;
		}
	return rc;
}
/******************************************************************************
* Function: print_config
*
* Input
* none
*
* Output
* none
*
* Returns
* none
*
* Description
* Print out config information
******************************************************************************/
static void print_config(void)
{
	fprintf(stdout, " ------------------------------------------------\n");
	fprintf(stdout, " Device name : \"%s\"\n", config.dev_name);
	fprintf(stdout, " IB port : %u\n", config.ib_port);
	if (config.server_name)
		fprintf(stdout, " IP : %s\n", config.server_name);
	fprintf(stdout, " TCP port : %u\n", config.tcp_port);
	if (config.gid_idx >= 0)
		fprintf(stdout, " GID index : %u\n", config.gid_idx);
	fprintf(stdout, " ------------------------------------------------\n\n");
}
/******************************************************************************
* Function: usage
*
* Input
* argv0 command line arguments
*
* Output
* none
*
* Returns
* none
*
* Description
* print a description of command line syntax
******************************************************************************/
static void usage(const char *argv0)
{
	fprintf(stdout, "使用方式:\n");
	fprintf(stdout, " %s 启动服务器并且等待连接\n", argv0);
	fprintf(stdout, " %s <host> 连接到服务器 <host>\n", argv0);
	fprintf(stdout, "\n");
	fprintf(stdout, "Options:\n");
	fprintf(stdout, " -p, --port <port> listen on/connect to port <port> (default 18515)\n");
	fprintf(stdout, " -d, --ib-dev <dev> use IB device <dev> (default first device found)\n");
	fprintf(stdout, " -i, --ib-port <port> use port <port> of IB device (default 1)\n");
	fprintf(stdout, " -g, --gid_idx <git index> gid index to be used in GRH (default not used)\n");
}



int main(int argc, char *argv[]){
    struct resources res;
    int rc = 1;
    char temp_char;
    	/* parse the command line parameters */
	while (1)
	{
		int c;
		static struct option long_options[] = {
			{.name = "port", .has_arg = 1, .val = 'p'},
			{.name = "ib-dev", .has_arg = 1, .val = 'd'},
			{.name = "ib-port", .has_arg = 1, .val = 'i'},
			{.name = "gid-idx", .has_arg = 1, .val = 'g'},
			{.name = NULL, .has_arg = 0, .val = '\0'}
        };
		c = getopt_long(argc, argv, "p:d:i:g:", long_options, NULL);
		if (c == -1)
			break;
		switch (c)
		{
		case 'p':
			config.tcp_port = strtoul(optarg, NULL, 0);//把输入的字符串转换成数字，0表示是十进制
			break;
		case 'd':
			config.dev_name = strdup(optarg);//拷贝字符串s的一个副本，由函数返回值返回
			break;
		case 'i':
			config.ib_port = strtoul(optarg, NULL, 0);
			if (config.ib_port < 0)
			{
				usage(argv[0]);
				return 1;
			}
			break;
		case 'g':
			config.gid_idx = strtoul(optarg, NULL, 0);
			if (config.gid_idx < 0)
			{
				usage(argv[0]);
				return 1;
			}
			break;
		default:
			usage(argv[0]);
			return 1;
		}        
	}
	/* parse the last parameter (if exists) as the server name */
	if (optind == argc - 1)//argv的当前索引值
		config.server_name = argv[optind];
    if(config.server_name){
        printf("servername=%s\n",config.server_name);
    }
    else if (optind < argc)
	{
		usage(argv[0]);
		return 1;
	}
    /* print the used parameters for info*/
	print_config();   
    /* 初始化资源结构体 */
    resources_init(&res);
	/* 使用改函数进行RDMA资源创建，直到创建好Queue pair，并且更新resource结构体 */
	if (resources_create(&res))
	{
		fprintf(stderr, "failed to create resources\n");
		goto main_exit;
	}
    //连接QP
	if (connect_qp(&res))
	{
		fprintf(stderr, "failed to connect QPs\n");
		goto main_exit;
	}
	/* let the server post the sr(send request) */
    // 在连接QP的时候有:"let the client post RR to be prepared for incoming messages"；所以此时客户端已经准备好了
    // 发送任务有三种操作：Send,Read,Write；Send操作需要对方执行相应的Receive操作;Read/Write直接操作对方内存，对方无感知
	if (!config.server_name)
		if (post_send(&res, IBV_WR_SEND))
		{
			fprintf(stderr, "failed to post sr\n");
			goto main_exit;
		}
    
    /* in both sides we expect to get a completion */
	if (poll_completion(&res))
	{
		fprintf(stderr, "poll completion failed\n");
		goto main_exit;
	}
    /* after polling the completion we have the message in the client buffer too */
	if (config.server_name)
		fprintf(stdout, "Message is: '%s'\n", res.buf);
	else
	{
		/* setup server buffer with read message */
		strcpy(res.buf, RDMAMSGR);
	}
	/* Sync so we are sure server side has data ready before client tries to read it */
	if (sock_sync_data(res.sock, 1, "R", &temp_char)) /* just send a dummy char back and forth */
	{
		fprintf(stderr, "sync error before RDMA ops\n");
		rc = 1;
		goto main_exit;
	}     

	/* Now the client performs an RDMA read and then write on server.
Note that the server has no idea these events have occured */
	if (config.server_name)
	{
		/* First we read contens of server's buffer */
		if (post_send(&res, IBV_WR_RDMA_READ))
		{
			fprintf(stderr, "failed to post SR 2\n");
			rc = 1;
			goto main_exit;
		}
		if (poll_completion(&res))
		{
			fprintf(stderr, "poll completion failed 2\n");
			rc = 1;
			goto main_exit;
		}
		fprintf(stdout, "Contents of server's buffer: '%s'\n", res.buf);
		/* Now we replace what's in the server's buffer */
		strcpy(res.buf, RDMAMSGW);
		fprintf(stdout, "Now replacing it with: '%s'\n", res.buf);
		if (post_send(&res, IBV_WR_RDMA_WRITE))
		{
			fprintf(stderr, "failed to post SR 3\n");
			rc = 1;
			goto main_exit;
		}
		if (poll_completion(&res))
		{
			fprintf(stderr, "poll completion failed 3\n");
			rc = 1;
			goto main_exit;
		}
	}         
	/* Sync so server will know that client is done mucking with its memory */
	if (sock_sync_data(res.sock, 1, "W", &temp_char)) /* just send a dummy char back and forth */
	{
		fprintf(stderr, "sync error after RDMA ops\n");
		rc = 1;
		goto main_exit;
	}
	if (!config.server_name)
		fprintf(stdout, "Contents of server buffer: '%s'\n", res.buf);
	rc = 0;  
main_exit:
	if (resources_destroy(&res))
	{
		fprintf(stderr, "failed to destroy resources\n");
		rc = 1;
	}
	if (config.dev_name)
		free((char *)config.dev_name);
	fprintf(stdout, "\ntest result is %d\n", rc);
	return rc;
}
