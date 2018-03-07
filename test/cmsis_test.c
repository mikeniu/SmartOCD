/*
 * cmsis_test.c
 *
 *  Created on: 2018-2-15
 *      Author: virusv
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "smart_ocd.h"
#include "misc/log.h"
#include "debugger/cmsis-dap.h"
#include "arch/ARM/ADI/DAP.h"

extern int print_bulk(char *data, int length, int rowLen);

uint16_t vids[] = {0xc251, 0};
uint16_t pids[] = {0xf001, 0};

static void printAPInfo(uint32_t APIDR){
	AP_IDRParse parse;
	parse.regData = APIDR;
	if(parse.regInfo.Class == 0x0){	// JTAG AP
		printf("JTAG-AP:JTAG Connection to this AP\n");
	}else if(parse.regInfo.Class ==0x8){	// MEM-AP
		printf("MEM-AP:");
		switch(parse.regInfo.Type){
		case 0x1:
			printf("AMBA AHB bus");
			break;
		case 0x2:
			printf("AMBA APB2 or APB3 bus");
			break;
		case 0x4:
			printf("AMBA AXI3 or AXI4 bus, with optional ACT-Lite support");
		}
		printf(" connection to this AP.\n");
	}
	printf("Revision:0x%x, Manufacturer:0x%x, Variant:0x%x.\n",
			parse.regInfo.Revision,
			parse.regInfo.JEP106Code,
			parse.regInfo.Variant);

}

// 测试入口点
int main(){
	AdapterObject *adapterObj;
	struct cmsis_dap *cmsis_dapObj;
	uint32_t idcode = 0, tmp;
	int32_t int_val, idx;
	uint8_t irLen[] = {4,4};
	log_set_level(LOG_DEBUG);
	cmsis_dapObj = NewCMSIS_DAP();
	if(cmsis_dapObj == NULL){
		log_fatal("failed to new.");
		exit(1);
	}
	// 连接CMSIS-DAP
	if(ConnectCMSIS_DAP(cmsis_dapObj, vids, pids, NULL) == FALSE){
		log_fatal("failed to connect.");
		exit(1);
	}
	adapterObj = CAST(AdapterObject *, cmsis_dapObj);
	// 初始化
	adapterObj->Init(adapterObj);
	// 配置传输参数
	adapterObj->Operate(adapterObj, CMDAP_TRANSFER_CONFIG, 5, 10, 10);

//#define USE_SWD
#ifdef USE_SWD
	// 设置SWJ频率，最大30MHz
	adapterObj->Operate(adapterObj, AINS_SET_CLOCK, 1000u);
	// 切换到SWD模式
	adapterObj->SelectTrans(adapterObj, SWD);
	// 读取id code 寄存器
	adapterObj->Operate(adapterObj, CMDAP_READ_DP_REG, 0, DP_IDCODE, &idcode);
	log_info("SWD-DP with ID :0x%08X.", idcode);
	// 清除上次的stick error （SW Only）
	// 写Abort寄存器请用CMDAP_WRITE_ABORT命令
	adapterObj->Operate(adapterObj, CMDAP_WRITE_ABORT, 0, STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR);
#else
	// 设置SWJ频率，最大30MHz
	adapterObj->Operate(adapterObj, AINS_SET_CLOCK, 100000u);
	// 切换到JTAG模式
	adapterObj->SelectTrans(adapterObj, JTAG);
	irLen[1] = 5;
	adapterObj->Operate(adapterObj, CMDAP_JTAG_CONFIGURE, 2, irLen);
	adapterObj->Operate(adapterObj, CMDAP_JTAG_IDCODE, 0, &idcode);
	log_info("JTAG-DP with ID:0x%08X.", idcode);
#endif
	// 确保DPBANKSEL = 0，选中CTRL/STAT寄存器
	adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_SELECT, 0);
	// 上电
	adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_CTRL_STAT, CSYSPWRUPREQ | CDBGPWRUPREQ);
	// 等待上电完成
	do {
		if(adapterObj->Operate(adapterObj, CMDAP_READ_DP_REG, 0, DP_CTRL_STAT, &tmp) == FALSE){
			log_fatal("Read CTRL_STAT Failed.");
			goto EXIT;
		}
	} while((tmp & (CDBGPWRUPACK | CSYSPWRUPACK)) != (CDBGPWRUPACK | CSYSPWRUPACK));
	log_info("Power up.");
	// 写入一些初始化数据
	adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_CTRL_STAT, CSYSPWRUPREQ | CDBGPWRUPREQ | TRNNORMAL | MASKLANE);
	// 读取AP的IDR
	do{
		time_t t_start, t_end;
		uint16_t apsel = 0;
		uint32_t select = 0xf0;
		uint32_t romtable = 0;
		uint32_t tmp = 0, addr;

		BOOL result;
		t_start = time(NULL) ;
		for(;apsel < 256; apsel ++){
			select &= ~0xff000000u;
			select |= apsel << 24;
			// 修改select选中ap
			result = adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_SELECT, select);
			if(result == FALSE) {
				// write abort;
				adapterObj->Operate(adapterObj, CMDAP_WRITE_ABORT, 0, DAPABORT);
				// SWD下错误标志在CTRL/STAT寄存器，清除错误标志是在ABORT寄存器
				// JTAG下错误标志是在CTRL/STAT寄存器，清除错误标志直接向CTRL/STAT寄存器错误标志位写0
				adapterObj->Operate(adapterObj, CMDAP_READ_DP_REG, 0, DP_CTRL_STAT, &tmp);
				log_warn("Write SELECT Failed, CTRL : 0x%08X.", tmp);
				break;
			}
			// 读取AP的IDR
			result = adapterObj->Operate(adapterObj, CMDAP_READ_AP_REG, 0, AP_IDR, &tmp);
			if(result == FALSE) {
				// write abort;
				adapterObj->Operate(adapterObj, CMDAP_WRITE_ABORT, 0, DAPABORT);
				adapterObj->Operate(adapterObj, CMDAP_READ_DP_REG, 0, DP_CTRL_STAT, &tmp);
				log_warn("Read AP_IDR Failed, CTRL : 0x%08X.", tmp);
				break;
			}
			if(tmp != 0){
				printf("probe Address: 0x%08X => 0x%08X\n", select, tmp);
				printAPInfo(tmp);
			}
		}
		t_end = time(NULL);
		printf("time: %.0f s\n", difftime(t_end,t_start));
		// 读取ROM Table
		adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_SELECT, 0xf0);
		adapterObj->Operate(adapterObj, CMDAP_READ_AP_REG, 0, AP_ROM_LSB, &romtable);
		adapterObj->Operate(adapterObj, CMDAP_WRITE_DP_REG, 0, DP_SELECT, 0x0);	// 选择AP bank0
		adapterObj->Operate(adapterObj, CMDAP_WRITE_AP_REG, 0, AP_CSW, 0x23000050 | CSW_SIZE32);
		// ROM Table的首地址
		addr = romtable & ~0xfff;
		log_info("ROM Table Base: 0x%08X.", addr);
		do{
			// ROM Table[11:0]不在BaseAddr里面，[1]为1代表该寄存器是ADIv5的类型，[0]代表是否存在DebugEntry
			// 注意还有Lagacy format要看一下资料
			adapterObj->Operate(adapterObj, CMDAP_WRITE_AP_REG, 0, AP_TAR_LSB, addr);
			adapterObj->Operate(adapterObj, CMDAP_READ_AP_REG, 0, AP_DRW, &int_val);
			if(int_val == 0) break;
			int_val &= 0xfffff000u;	// Address Offset
			int_val = (int32_t)addr + int_val;
			log_info("Component Base Addr: 0x%08X.", int_val);
			addr += 4;
		}while(1);
		addr = (romtable & ~0xfff) | 0xFCC;
		adapterObj->Operate(adapterObj, CMDAP_WRITE_AP_REG, 0, AP_TAR_LSB, addr);
		adapterObj->Operate(adapterObj, CMDAP_READ_AP_REG, 0, AP_DRW, &int_val);
		log_info("Mem Type: %x.", int_val);
		for(idx=0xFD0; idx < 0xFFF; idx += 4){
			addr = (romtable & ~0xfff) | idx;
			adapterObj->Operate(adapterObj, CMDAP_WRITE_AP_REG, 0, AP_TAR_LSB, addr);
			adapterObj->Operate(adapterObj, CMDAP_READ_AP_REG, 0, AP_DRW, &int_val);
			log_info("0x%03X:0x%08X.", idx, int_val);
		}
	}while(0);
EXIT:
	adapterObj->Deinit(adapterObj);
	FreeCMSIS_DAP(cmsis_dapObj);
	exit(0);
}
