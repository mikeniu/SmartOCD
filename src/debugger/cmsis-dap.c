/*
 * cmsis-dap.c
 *
 *  Created on: 2018-2-18
 *      Author: virusv
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "misc/log.h"
#include "debugger/cmsis-dap.h"
#include "arch/ARM/ADI/DAP.h"

#include "misc/misc.h"

/**
 * 这个宏是用来作为与cmsis-dap通信函数的外包部分
 * 该宏实现自动申请cmsis-dap返回包缓冲空间并且带函数执行结束后
 * 释放该空间
 */
#define CMDAP_FUN_WARP(pa,func,...) ({	\
	uint8_t *responseBuff; BOOL result = FALSE;	\
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap*, (pa));	\
	assert(cmsis_dapObj->PacketSize >0);	\
	if((responseBuff = calloc(cmsis_dapObj->PacketSize, sizeof(uint8_t))) == NULL){	\
		log_warn("Alloc reply packet buffer memory failed.");	\
		result = FALSE;	\
	}else{	\
		result = (func)(responseBuff, ##__VA_ARGS__);	\
		free(responseBuff);	\
	}	\
	result;	\
})

// CMSIS-DAP支持的仿真传输协议类型
static const enum transportType supportTrans[] = {JTAG, SWD, UNKNOW_NULL};	// 必须以UNKNOW_NULL结尾
static BOOL init(AdapterObject *adapterObj);
static BOOL deinit(AdapterObject *adapterObj);
static BOOL selectTrans(AdapterObject *adapterObj, enum transportType type);
static BOOL operate(AdapterObject *adapterObj, int operate, ...);
static void destroy(AdapterObject *adapterObj);

/**
 * 创建新的CMSIS-DAP仿真器对象
 */
BOOL NewCMSIS_DAP(struct cmsis_dap *cmsis_dapObj){
	assert(cmsis_dapObj != NULL);
	AdapterObject *adapterObj;
	adapterObj = CAST(AdapterObject *, cmsis_dapObj);
	// 清空数据
	memset(cmsis_dapObj, 0x0, sizeof(struct cmsis_dap));
	// 构造Adapter对象
	if(__CONSTRUCT(Adapter)(adapterObj, "ARM CMSIS-DAP") == FALSE){
		log_warn("Failed to Init AdapterObject.");
		return FALSE;
	}
	// 构造USB对象
	if(__CONSTRUCT(USB)(GET_USBOBJ(adapterObj)) == FALSE){
		__DESTORY(Adapter)(adapterObj);
		log_warn("Failed to create USB object.");
		return FALSE;
	}
	adapterObj->ConnObject.type = USB;
	adapterObj->ConnObject.connectFlag = 0;

	// 设置该设备支持的传输类型
	adapterObj->supportedTrans = supportTrans;
	adapterObj->currTrans = UNKNOW_NULL;
	// 配置方法函数
	adapterObj->Init = init;
	adapterObj->Deinit = deinit;
	adapterObj->SelectTrans = selectTrans;
	adapterObj->Operate = operate;
	adapterObj->Destroy = destroy;	// 警告，指针不匹配
	return TRUE;
}

// 释放CMSIS-DAP对象
static void destroy(AdapterObject *adapterObj){
	assert(adapterObj != NULL);
	// 关闭USB对象
	if(adapterObj->ConnObject.connectFlag){
		USBClose(GET_USBOBJ(adapterObj));
		adapterObj->ConnObject.connectFlag = 0;
	}
	// 释放USB对象
	__DESTORY(USB)(GET_USBOBJ(adapterObj));
	// 反初始化Adapter对象
	__DESTORY(Adapter)(adapterObj);
}

// 连接CMSIS-DAP设备，只支持USB
BOOL ConnectCMSIS_DAP(struct cmsis_dap *cmsis_dapObj, const uint16_t *vids, const uint16_t *pids, const char *serialNum){
	assert(cmsis_dapObj != NULL && CAST(AdapterObject *, cmsis_dapObj)->ConnObject.type == USB);
	int idx = 0;
	USBObject *usbObj = GET_USBOBJ(CAST(AdapterObject *, cmsis_dapObj));
	for(; vids[idx] && pids[idx]; idx++){
		log_debug("Try connecting vid: 0x%02x, pid: 0x%02x usb device.", vids[idx], pids[idx]);
		if(USBOpen(usbObj, vids[idx], pids[idx], serialNum)){
			log_info("Successful connection vid: 0x%02x, pid: 0x%02x usb device.", vids[idx], pids[idx]);
			// 复位设备
			USBResetDevice(usbObj);
			// 标志已连接
			CAST(AdapterObject *, cmsis_dapObj)->ConnObject.connectFlag = 1;
			// 选择配置和声明接口
			if(USBSetConfiguration(usbObj, 0) != TRUE){
				log_warn("USBSetConfiguration Failed.");
				return FALSE;
			}
			if(USBClaimInterface(usbObj, 3, 0, 0, 3) != TRUE){
				log_warn("Claim interface failed.");
				return FALSE;
			}
			return TRUE;
		}
	}
	log_warn("No suitable device found.");
	return FALSE;
}

/**
 * 从仿真器读数据
 * buff是保存数据的缓冲区
 * 注意：该函数会读取固定大小 cmsis_dapObj->PacketSize
 */
static int DAPRead(AdapterObject *adapterObj, uint8_t *buff){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	USBObject *usbObj = GET_USBOBJ(adapterObj);
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	assert(cmsis_dapObj->PacketSize != 0);
	int length = usbObj->Read(usbObj, buff, cmsis_dapObj->PacketSize, 0);
	log_trace("Read %d byte(s) from %s.", length, adapterObj->DeviceDesc);
	return length;
}

/**
 * 从仿真器写数据
 * data数数据缓冲区指针
 * len是要发送的数据长度
 */
static int DAPWrite(AdapterObject *adapterObj, uint8_t *data, int len){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	USBObject *usbObj = GET_USBOBJ(adapterObj);
	int length =  usbObj->Write(usbObj, data, len, 0);
	log_trace("Write %d byte(s) to %s.", length, adapterObj->DeviceDesc);
	return length;
}

#define DAP_EXCHANGE_DATA(pa,data,len,buff) \
	DAPWrite((pa), (data), (len)); \
	DAPRead((pa), (buff));

// 初始化CMSIS-DAP设备
static BOOL init(AdapterObject *adapterObj){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	// 判断是否已经执行过初始化
	if(adapterObj->isInit == TRUE){
		return TRUE;
	}
	char capablity;
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	USBObject *usbObj = GET_USBOBJ(adapterObj);
	uint8_t command[2] = {ID_DAP_Info, DAP_ID_PACKET_SIZE}, *resp;	// 命令存储缓冲和返回值缓冲区
	if((resp = calloc(usbObj->readEPMaxPackSize, sizeof(uint8_t))) == NULL){
		log_warn("Alloc response buffer failed.");
		return FALSE;
	}
	// 获得DAP_Info 判断
	log_info("CMSIS-DAP Init.");
	// 先以endpoint最大包长读取packet大小，然后读取剩下的
	usbObj->Write(usbObj, command, 2, 0);
	usbObj->Read(usbObj, resp, usbObj->readEPMaxPackSize, 0);

	// 判断返回值是否是一个16位的数字
	if(resp[0] != 0 || resp[1] != 2){
		free(resp);
		log_warn("Get packet size failed.");
		return FALSE;
	}
	// 取得包长度
	cmsis_dapObj->PacketSize = *CAST(uint16_t *, resp+2);
	if(cmsis_dapObj->PacketSize == 0){
		free(resp);
		log_warn("Packet Size is Zero!!!");
		return FALSE;
	}
	// 重新分配缓冲区大小
	if((resp = realloc(resp, cmsis_dapObj->PacketSize * sizeof(uint8_t))) == NULL){
		log_warn("Alloc response buffer failed.");
		return FALSE;
	}
	log_info("CMSIS-DAP the maximum Packet Size is %d.", cmsis_dapObj->PacketSize);
	// 读取剩下的内容
	if(cmsis_dapObj->PacketSize > usbObj->readEPMaxPackSize){
		int rest_len = cmsis_dapObj->PacketSize - usbObj->readEPMaxPackSize;
		usbObj->Read(usbObj, resp, rest_len > usbObj->readEPMaxPackSize ? rest_len : usbObj->readEPMaxPackSize, 0);
	}
	// 获得CMSIS-DAP固件版本
	command[1] = DAP_ID_FW_VER;
	DAP_EXCHANGE_DATA(adapterObj, command, 2, resp);

	cmsis_dapObj->Version = (int)(atof(resp+2) * 100); // XXX 改成了整数
	log_info("CMSIS-DAP FW Version is %f.", cmsis_dapObj->Version / 100.0);

	// 获得CMSIS-DAP的最大包长度和最大包个数
	command[1] = DAP_ID_PACKET_COUNT;
	DAP_EXCHANGE_DATA(adapterObj, command, 2, resp);
	cmsis_dapObj->MaxPcaketCount = *CAST(uint8_t *, resp+2);
	log_info("CMSIS-DAP the maximum Packet Count is %d.", cmsis_dapObj->MaxPcaketCount);

	// Capabilities. The information BYTE contains bits that indicate which communication methods are provided to the Device.
	command[1] = DAP_ID_CAPABILITIES;
	DAP_EXCHANGE_DATA(adapterObj, command, 2, resp);

	// XXX Version改成了整数，乘以100
	switch(cmsis_dapObj->Version){
	case 110: // 1.1f
		// 获得info0
		capablity = *CAST(uint8_t *, resp+2);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 2, CMDAP_CAP_SWO_UART);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 3, CMDAP_CAP_SWO_MANCHESTER);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 4, CMDAP_CAP_ATOMIC);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 5, CMDAP_CAP_SWD_SEQUENCE);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 6, CMDAP_CAP_TEST_DOMAIN_TIMER);

		if(*CAST(uint8_t *, resp+1) == 2){	// 是否存在info1
			// 获得info1
			capablity = *CAST(uint8_t *, resp+3);
			ADAPTER_SET_CAP(cmsis_dapObj, capablity, 0, CMDAP_CAP_TRACE_DATA_MANAGE);
		}
		/* no break */

	case 100: // 1.0f
		// 获得info0
		capablity = *CAST(uint8_t *, resp+2);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 0, CMDAP_CAP_SWD);
		ADAPTER_SET_CAP(cmsis_dapObj, capablity, 1, CMDAP_CAP_JTAG);
		break;

	default: break;
	}
	log_info("CMSIS-DAP Capabilities 0x%X.", cmsis_dapObj->capablityFlag);
	// 在初始化过程中不选择transport
	adapterObj->currTrans = UNKNOW_NULL;
	free(resp);
	// 标记当前Adapter已经执行过初始化
	adapterObj->isInit = TRUE;
	return TRUE;
}

/**
 * Disconnect
 */
static BOOL DAP_Disconnect(uint8_t *respBuff, AdapterObject *adapterObj){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	uint8_t command[1] = {ID_DAP_Disconnect};
	DAP_EXCHANGE_DATA(adapterObj, command, 1, respBuff);
	// 检查返回值
	if(respBuff[1] != DAP_OK){
		log_warn("Disconnect execution failed.");
		return FALSE;
	}
	return TRUE;
}

// 反初始化
static BOOL deinit(AdapterObject *adapterObj){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	// 判断是否初始化
	if(adapterObj->isInit == FALSE){
		return TRUE;
	}
	// 断开连接
	if(CMDAP_FUN_WARP(adapterObj, DAP_Disconnect, adapterObj) == TRUE){
		adapterObj->isInit = FALSE;
		return TRUE;
	}else{
		return FALSE;
	}
}

/**
 * JTAG协议转SWD
 */
static BOOL SWJ_JTAG2SWD(uint8_t *respBuff, AdapterObject *adapterObj){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	//SWJ发送 0xE79E，老版本的ARM发送0xB76D强制切换
	uint8_t switchSque[] = {ID_DAP_SWJ_Sequence, 144,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 56 bit
			0x9e, 0xe7,	// 16 bit
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 56 bit
			0x00, 0x00,	// 16 bit
	};	// DAP_SWJ_Sequence Command
	DAP_EXCHANGE_DATA(adapterObj, switchSque, sizeof(switchSque), respBuff);
	return TRUE;
}

/**
 * SWD协议转JTAG
 */
static BOOL SWJ_SWD2JTAG(uint8_t *respBuff, AdapterObject *adapterObj){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	//SWJ发送 0xE79E，老版本的ARM发送0xB76D强制切换
	uint8_t switchSque[] = {ID_DAP_SWJ_Sequence, 80,
			0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 56 bit
			0x3c, 0xe7,	// 16 bit
			0xff,	// 8 bit
	};	// DAP_SWJ_Sequence Command
	DAP_EXCHANGE_DATA(adapterObj, switchSque, sizeof(switchSque), respBuff);
	return TRUE;
}

// 选择和切换传输协议
static BOOL selectTrans(AdapterObject *adapterObj, enum transportType type){
	int idx;
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	assert(cmsis_dapObj->PacketSize != 0);

	uint8_t command[2] = {ID_DAP_Connect, DAP_PORT_AUTODETECT}, *resp;	// 命令存储缓冲和返回值缓冲区
	if(type == UNKNOW_NULL) return FALSE;
	if(adapterObj->currTrans == type) return TRUE;
	// 如果不支持该协议
	if(adapter_HaveTransmission(adapterObj, type) == FALSE) return FALSE;

	if((resp = calloc(cmsis_dapObj->PacketSize, sizeof(uint8_t))) == NULL){
		log_error("Failed to calloc receive buffer space.");
		return FALSE;
	}
	// 切换协议
	switch(type){
	case SWD:
		if(!ADAPTER_HAS_CAPALITY(cmsis_dapObj, CMDAP_CAP_SWD)){
			goto EXIT_FAILED;
		}
		// 切换到SWD模式
		command[1] = DAP_PORT_SWD;
		DAP_EXCHANGE_DATA(adapterObj, command, 2, resp);
		if(*CAST(uint8_t *, resp+1) != DAP_PORT_SWD){
			log_warn("Switching SWD mode failed.");
			goto EXIT_FAILED;
		}else{
			// 发送切换swd序列
			CMDAP_FUN_WARP(adapterObj, SWJ_JTAG2SWD, adapterObj);
			adapterObj->currTrans = SWD;
			log_info("Switch to SWD mode.");
			goto EXIT_TRUE;
		}
		break;

	case JTAG:
		if(!ADAPTER_HAS_CAPALITY(cmsis_dapObj, CMDAP_CAP_JTAG)){
			goto EXIT_FAILED;
		}
		// 切换到JTAG模式
		command[1] = DAP_PORT_JTAG;
		DAP_EXCHANGE_DATA(adapterObj, command, 2, resp);
		if(*CAST(uint8_t *, resp+1) != DAP_PORT_JTAG){
			log_warn("Switching JTAG mode failed.");
			goto EXIT_FAILED;
		}else{
			// 发送切换JTAG序列
			CMDAP_FUN_WARP(adapterObj, SWJ_SWD2JTAG, adapterObj);
			adapterObj->currTrans = JTAG;
			log_info("Switch to JTAG mode.");
			goto EXIT_TRUE;
		}
		break;
	default :
		goto EXIT_FAILED;
	}

EXIT_FAILED:
	// 协议不支持
	free(resp);
	return FALSE;
EXIT_TRUE:
	free(resp);
	return TRUE;
}

/**
 * 设置传输参数
 * 在调用DAP_Transfer和DAP_TransferBlock之前要先调用该函数
 * idleCycle：每一次传输后面附加的空闲时钟周期数
 * waitRetry：如果收到WAIT响应，重试的次数
 * matchRetry：如果在匹配模式下发现值不匹配，重试的次数
 * SWD、JTAG模式下均有效
 */
static BOOL DAP_TransferConfigure(uint8_t *respBuff, AdapterObject *adapterObj, uint8_t idleCycle, uint16_t waitRetry, uint16_t matchRetry){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	uint8_t DAPTransfer[6] = {ID_DAP_TransferConfigure};
	DAPTransfer[1] = idleCycle;	// Number of extra idle cycles after each transfer.
	DAPTransfer[2] = BYTE_IDX(waitRetry, 0);
	DAPTransfer[3] = BYTE_IDX(waitRetry, 1);
	DAPTransfer[4] = BYTE_IDX(matchRetry, 0);
	DAPTransfer[5] = BYTE_IDX(matchRetry, 1);

	DAP_EXCHANGE_DATA(adapterObj, DAPTransfer, sizeof(DAPTransfer), respBuff);
	// 增加判断是否成功
	if(respBuff[1] != DAP_OK){
		log_warn("Transfer config execution failed.");
		return FALSE;
	}
	return TRUE;
}

/**
 * 发送一个或多个SWD时序
 */
static BOOL DAP_SWD_Sequence(uint8_t *respBuff, AdapterObject *adapterObj, uint8_t sequenceCount, uint8_t *data, uint8_t *response){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	assert(cmsis_dapObj->PacketSize != 0);
	if(cmsis_dapObj->Version < 110 || !ADAPTER_HAS_CAPALITY(cmsis_dapObj, CMDAP_CAP_SWD_SEQUENCE)){
		// 不支持发送SWD时序
		return FALSE;
	}
	// TODO 实现SWD_Sequence
	return FALSE;
}

/**
 * 必须实现指令之 AINS_SET_STATUS
 * DAP_HostStatus 设置仿真器状态灯
 * 参数：status 状态
 */
static BOOL DAP_HostStatus(uint8_t *respBuff, AdapterObject *adapterObj, int status){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	uint8_t DAP_HostStatusPack[] = {ID_DAP_HostStatus, 0, 0};
	switch(status){
	case ADAPTER_STATUS_CONNECTED:
		DAP_HostStatusPack[1] = 0;
		DAP_HostStatusPack[2] = 1;
		break;
	case ADAPTER_STATUS_DISCONNECT:
		DAP_HostStatusPack[1] = 0;
		DAP_HostStatusPack[2] = 0;
		break;
	case ADAPTER_STATUS_RUNING:
		DAP_HostStatusPack[1] = 1;
		DAP_HostStatusPack[2] = 1;
		break;
	case ADAPTER_STATUS_IDLE:
		DAP_HostStatusPack[1] = 1;
		DAP_HostStatusPack[2] = 0;
		break;
	default:
		return FALSE;
	}
	DAP_EXCHANGE_DATA(adapterObj, DAP_HostStatusPack,  3, respBuff);
	return TRUE;
}

/**
 * 必须实现指令之 AINS_SET_CLOCK
 * 设置SWJ的最大频率
 * 参数 clockHz 时钟频率，单位是 Hz
 */
static BOOL DAP_SWJ_Clock (uint8_t *respBuff, AdapterObject *adapterObj, uint32_t clockHz){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	uint8_t clockHzPack[5] = {ID_DAP_SWJ_Clock};

	clockHzPack[1] = BYTE_IDX(clockHz, 0);
	clockHzPack[2] = BYTE_IDX(clockHz, 1);
	clockHzPack[3] = BYTE_IDX(clockHz, 2);
	clockHzPack[4] = BYTE_IDX(clockHz, 3);

	DAP_EXCHANGE_DATA(adapterObj, clockHzPack, sizeof(clockHzPack), respBuff);
	// 增加判断是否成功
	if(respBuff[1] != DAP_OK){
		log_warn("SWJ Clock execution failed.");
		return FALSE;
	}
	return TRUE;
}

/**
 * 必须实现指令之 AINS_JTAG_SEQUENCE
 * 产生JTAG时序
 * sequenceCount:需要产生多少组时序
 * data ：时序表示数据
 * response：TDO返回数据
 */
static BOOL DAP_JTAG_Sequence(uint8_t *respBuff, AdapterObject *adapterObj, int sequenceCount, uint8_t *data, uint8_t *response){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	// 判断当前是否是JTAG模式
	if(adapterObj->currTrans != JTAG) return FALSE;
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	assert(cmsis_dapObj->PacketSize != 0);
	/**
	 * 分配所有缓冲区，包括max packet count和packet buff
	 */
	uint8_t *buff = calloc(sizeof(int) * cmsis_dapObj->MaxPcaketCount + cmsis_dapObj->PacketSize, sizeof(uint8_t));
	if(buff == NULL){
		log_warn("Unable to allocate send packet buffer, the heap may be full.");
		return FALSE;
	}
	// 记录每次分包需要接收的result
	int *resultLength = CAST(int *,buff);
	// 发送包缓冲区
	uint8_t *sendPackBuff = buff + sizeof(int) * cmsis_dapObj->MaxPcaketCount;
	int inputIdx = 0,outputIdx = 0, seqIdx = 0;	// data数据索引，response数据索引，sequence索引
	int currIdx = 0;	// 当前的data的偏移
	int sendPackCnt = 0;	// 当前发包计数
	int readPayloadLen, sendPayloadLen;	// 要发送和接收的载荷总字节，不包括头部
	uint8_t seqCount;	// 统计当前有多少个seq
	sendPackBuff[0] = ID_DAP_JTAG_Sequence;	// 指令头部 0

MAKE_PACKT:
	seqCount = 0;
	readPayloadLen = 0;
	sendPayloadLen = 0;
	currIdx = inputIdx;
	// 计算空间长度
	for(; seqIdx < sequenceCount; seqIdx++){
		// GNU编译器下面可以直接这样用：uint8_t tckCount = data[idx] & 0x3f ? : 64;
		uint8_t tckCount = data[inputIdx] & 0x3f;
		tckCount = tckCount ? tckCount : 64;
		uint8_t tdiByte = (tckCount + 7) >> 3;	// 将TCK个数圆整到字节，表示后面跟几个byte的tdi数据
		// log_debug("Seq:0x%02x, tck:%d, tdiByte:%d, TMS:%d.", data[inputIdx], tckCount, tdiByte, !!(data[inputIdx] & 0x40));
		// 如果当前数据长度加上tdiByte之后大于包长度，+3的意思是两个指令头部和SeqInfo字节
		if(sendPayloadLen + tdiByte + 3 > cmsis_dapObj->PacketSize){
			break;
		}else sendPayloadLen += tdiByte + 1;	// FIX:+1是要计算头部
		//如果TDO Capture标志置位，则从TDO接收tdiByte字节的数据
		if((data[inputIdx] & 0x80)){
			readPayloadLen += tdiByte;	// readByteCount 一定会比sendByteCount少，所以上面检查过sendByteCount不超过最大包长度，readByteCount必不会超过
		}
		inputIdx += tdiByte + 1;	// 跳到下一个sequence info
		seqCount++;
		if(seqCount == 255){	// 判断是否到达最大序列数目
			break;
		}
	}
	/**
	 * 到达这儿，有三种情况：
	 * 1、seqIdx == sequenceCount
	 * 2、发送的总数据马上要大于包长度
	 * 3、seqCount == 255
	 */
	sendPackBuff[1] = seqCount;	// sequence count
	memcpy(sendPackBuff + 2, data + currIdx, sendPayloadLen);
	log_trace("Trasmission load: Send %d bytes, receive %d bytes.", sendPayloadLen, readPayloadLen);
	// misc_PrintBulk(sendPackBuff+2, sendPayloadLen, 16);

	// 发送包
	DAPWrite(adapterObj, sendPackBuff, sendPayloadLen + 2);
	resultLength[sendPackCnt++] = readPayloadLen;	// 本次包的响应包包含多少个数据

	/**
	 * 如果没发完，而且没有达到最大包数量，则再构建一个包发送过去
	 */
	if(seqIdx < (sequenceCount-1) && sendPackCnt < cmsis_dapObj->MaxPcaketCount) goto MAKE_PACKT;

	// 读数据
	for(int readPackCnt = 0; readPackCnt < sendPackCnt; readPackCnt++){
		DAPRead(adapterObj, respBuff);
		if(respBuff[1] == DAP_OK){
			// misc_PrintBulk(respBuff+2, resultLength[readPackCnt], 16);
			// 拷贝数据
			if(response){
				memcpy(response + outputIdx, respBuff + 2, resultLength[readPackCnt]);
				outputIdx += resultLength[readPackCnt];
			}
		}else{
			free(buff);
			return FALSE;
		}
	}
	// 判断是否处理完，如果没有则跳回去重新处理
	if(seqIdx < (sequenceCount-1)) {
		// 将已发送包清零
		sendPackCnt = 0;
		goto MAKE_PACKT;
	}

	free(buff);
	return TRUE;
}

/**
 * 必须实现指令之 AINS_JTAG_PINS
 * pinSelect：选择哪一个pin
 * pinOutput：引脚将要输出的信号
 * pinInput：pinOutput的信号稳定后，读取全部的引脚数据
 * pinWait：死区时间
 * 0 = no wait
 * 1 .. 3000000 = time in µs (max 3s)
 * 返回值 TRUE；
 */
static BOOL DAP_SWJ_Pins(uint8_t *respBuff, AdapterObject *adapterObj, uint8_t pinSelect, uint8_t pinOutput, uint8_t *pinInput, uint32_t pinWait){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	uint8_t DAP_PinPack[7] = {ID_DAP_SWJ_Pins};
	// 构造包数据
	DAP_PinPack[1] = pinOutput;
	DAP_PinPack[2] = pinSelect;
	DAP_PinPack[3] = BYTE_IDX(pinWait, 0);
	DAP_PinPack[4] = BYTE_IDX(pinWait, 1);
	DAP_PinPack[5] = BYTE_IDX(pinWait, 2);
	DAP_PinPack[6] = BYTE_IDX(pinWait, 3);

	DAP_EXCHANGE_DATA(adapterObj, DAP_PinPack, sizeof(DAP_PinPack), respBuff);
	if(pinInput)
		*pinInput = *(respBuff + 1);
	return TRUE;
}

/**
 * 必须实现指令之 AINS_DAP_TRANSFER
 * SWD和JTAG模式下均有效
 * 具体手册参考CMSIS-DAP DAP_Transfer这一小节
 * 关于response 参数所指向的内存区域，resquest中有多少个读指令，就应该备好多少个WORD
 * 如果指定了TimeStamp标志位，则还需要多个WORD用来存放时间戳
 */
static BOOL DAP_Transfer(uint8_t *respBuff, AdapterObject *adapterObj, uint8_t index, uint8_t transferCount, uint8_t *data, uint8_t *response){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	assert(response != NULL);
	struct cmsis_dap *cmsis_dapObj = CAST(struct cmsis_dap *, adapterObj);
	assert(cmsis_dapObj->PacketSize != 0);

	uint8_t *DAP_TransferPacket, seqIdx;
	int readCount = 0, writeCount = transferCount, idx;

	// 统计一些信息
	for(idx = 0, seqIdx = 0; seqIdx < transferCount; seqIdx ++){
		data[idx] &= 0xf;	// 只保留[3:0]位
		// 判断是否是读寄存器
		if((data[idx] & DAP_TRANSFER_RnW) == DAP_TRANSFER_RnW){
			idx += 1;
			readCount += 4;
		}else{	// 写寄存器
			idx += 5;
			writeCount += 4;
		}
	}
	// 判断是否超出最大包长度
	if((3 + writeCount) > cmsis_dapObj->PacketSize || (3 + readCount) > cmsis_dapObj->PacketSize){
		log_error("Packet too large.");
		return FALSE;
	}
	if((DAP_TransferPacket = calloc(3 + writeCount, sizeof(uint8_t))) == NULL){
		log_error("Failed to alloc Packet buffer.");
		return FALSE;
	}
	// 构造数据包头部
	DAP_TransferPacket[0] = ID_DAP_Transfer;
	DAP_TransferPacket[1] = index;	// DAP index, JTAG ScanChain 中的位置，在SWD模式下忽略该参数
	DAP_TransferPacket[2] = transferCount;	// 传输多少个request
	// 将数据拷贝到包中
	memcpy(DAP_TransferPacket + 3, data, writeCount);
	DAP_EXCHANGE_DATA(adapterObj, DAP_TransferPacket, 3 + writeCount, respBuff);
	// 拷贝数据
	memcpy(response, respBuff + 1, readCount + 2);
	free(DAP_TransferPacket);
	return TRUE;
}

/**
 * 执行仿真器指令
 */
static BOOL operate(AdapterObject *adapterObj, int operate, ...){
	assert(adapterObj != NULL && adapterObj->ConnObject.type == USB);
	va_list parames;
	BOOL result = FALSE;
	va_start(parames, operate);	// 确定可变参数起始位置
	// 根据指令选择操作
	switch(operate){
	case AINS_SET_STATUS:
		log_trace("Execution command: Set Status.");
		do{
			int status = va_arg(parames, int);
			result = CMDAP_FUN_WARP(adapterObj, DAP_HostStatus, adapterObj, status);
		}while(0);
		break;

	case AINS_SET_CLOCK:
		log_trace("Execution command: Set SWJ Clock.");
		do{
			uint32_t clockHz = va_arg(parames, uint32_t);
			log_debug("clockHz: %d.", clockHz);
			result = CMDAP_FUN_WARP(adapterObj, DAP_SWJ_Clock, adapterObj, clockHz);
		}while(0);
		break;

	case AINS_JTAG_SEQUENCE:
		log_trace("Execution command: JTAG Sequence.");
		do{
			int sequenceCount = va_arg(parames, int);
			uint8_t *data = va_arg(parames, uint8_t *);
			uint8_t *response = va_arg(parames, uint8_t *);
			result = CMDAP_FUN_WARP(adapterObj, DAP_JTAG_Sequence, adapterObj, sequenceCount, data, response);
		}while(0);
		break;

	case AINS_JTAG_PINS:
		log_trace("Execution command: JTAG Pins.");
		do{
			uint8_t pinSelect = (uint8_t)va_arg(parames, int);
			uint8_t pinOutput = (uint8_t)va_arg(parames, int);
			uint8_t *pinInput = va_arg(parames, uint8_t *);
			uint32_t pinWait = (uint32_t)va_arg(parames, int);
			result = CMDAP_FUN_WARP(adapterObj, DAP_SWJ_Pins, adapterObj, pinSelect, pinOutput, pinInput, pinWait);
		}while(0);
		break;

	case AINS_DAP_TRANSFER:
		log_trace("Execution command: DAP Transfer.");
		do {
			uint8_t dap_index = (uint8_t)va_arg(parames, int);
			uint8_t count = (uint8_t)va_arg(parames, int);
			uint8_t *data = va_arg(parames, uint8_t *);
			uint8_t *response = va_arg(parames, uint8_t *);
			result = CMDAP_FUN_WARP(adapterObj, DAP_Transfer, adapterObj, dap_index, count, data, response);
		}while(0);
		break;

	case CMDAP_TRANSFER_CONFIG:
		log_trace("Execution command: Transfer Config.");
		do{
			uint8_t idleCycle = (uint8_t)va_arg(parames, int);	// 在每次传输后面加多少个空闲时钟周期
			uint16_t waitRetry = (uint16_t)va_arg(parames, int);	// 得到WAIT Response后重试次数
			uint16_t matchRetry = (uint16_t)va_arg(parames, int);	// 在匹配模式下不匹配时重试次数
			log_debug("idle:%d, wait:%d, match:%d.", idleCycle, waitRetry, matchRetry);
			result = CMDAP_FUN_WARP(adapterObj, DAP_TransferConfigure, adapterObj, idleCycle, waitRetry, matchRetry);
		}while(0);
		break;

	default:
		log_warn("Unsupported operation: %d.", operate);
		break;
	}
	va_end(parames);
	return result;
}

