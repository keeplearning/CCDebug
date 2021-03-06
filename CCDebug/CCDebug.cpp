// CCDebug.cpp: 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "debugRegisters.h"
#include <windows.h>
#include <vector>
#include <DbgHelp.h>
#include <exception> 
#include <TlHelp32.h>
#include <winternl.h>
#include <map>
#include <iostream>
#include <string>

//汇编引擎
#include "XEDParse/XEDParse.h"

#ifdef _WIN64
#pragma comment (lib,"XEDParse/x64/XEDParse_x64.lib")
#else
#pragma comment (lib,"XEDParse/x86/XEDParse_x86.lib")
#endif // _WIN64

//反汇编引擎
#define BEA_ENGINE_STATIC
#define BEA_USE_STDCALL
#include "BeaEngine_4.1\\Win32\\headers\\BeaEngine.h"
#ifdef _WIN64
#pragma comment(lib,"BeaEngine_4.1\\Win64\\Win64\\Lib\\BeaEngine.lib")
#else
#pragma comment(lib,"BeaEngine_4.1\\Win32\\Win32\\Lib\\BeaEngine.lib")
#endif // _WIN32
#pragma comment(linker, "/NODEFAULTLIB:\"crt.lib\"")
#pragma comment(lib, "legacy_stdio_definitions.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "Dbghelp.lib")

typedef struct _BREAKPOINT
{
	LPVOID address; // 原数据所在的地址
	BYTE oldData; //保存int3覆盖的数据， 如果没有数据就是硬件断点

}BREAKPOINT;

std::vector<BREAKPOINT> g_vBP;//用vector保存断点

typedef void(*MyPlugin)(char*);

HANDLE g_hProc = 0;
HANDLE g_hThread = 0;
BOOL g_isDbgTF = FALSE;
int g_nDR_L = -1; //保存设置了那个L位的断点
ULONG_PTR g_pMemAddr = NULL;//保存内存断点的地址
DWORD g_nOldProtect = 0; //旧的内存页属性
DWORD g_nNewProtect = 0;//新的内存页属性
BOOL g_isMBPTF = FALSE; //用来设置内存断点
char g_filePath[MAX_PATH] = { 0 };//保存文件路径
DWORD g_OEP = 0;//程序入口点地址
DWORD g_conAddr = 0; //条件断点地址
DWORD g_conValue = 0;//条件断点的值  判断断点是否断下来
HINSTANCE g_hDll = 0;//动态库句柄
std::map<std::string, DWORD> g_map;//保存模块名字和模块基址

void UserInput(LPVOID pAddress);
DWORD DispatchEvent(DEBUG_EVENT *pDbgEvent);
DWORD DispatchException(EXCEPTION_DEBUG_INFO *pDebugInfo);
void ShowDisasm(LPVOID pAddress, DWORD nLen);
void SetBP_int3(LPVOID pAddress);
void ClearBP_int3(LPVOID pAddress);
void SetBP_tf();
BOOL SetBP_hardExec(ULONG_PTR pAddress);
BOOL SetBP_hardRW(ULONG_PTR pAddress, DWORD type, DWORD dwlen);
void SetHardBPWork();
void ClearHardBP();
void SetBP_mem(LPVOID pAddress, DWORD protect);
void StepBy();
BOOL ModifyAsm(LPVOID pAddress);
void ShowMemory(LPVOID pAddress);
void printOpcode(LPBYTE pOpcode, int nSize);
void ModifyMemory(LPBYTE pAddress);
void ShowStack();
void ShowRegisters();
void ModifyResisters(const char *pStr, DWORD value);
void ShowModule();
void GetModuleTable(LPCSTR pszName);
void OnDebugEvent_LOAD_DLL_DEBUG_EVENT(DEBUG_EVENT* dbgEvent);

//显示反汇编
void ShowDisasm(LPVOID pAddress, DWORD nLen)
{
	LPBYTE pOpCode = new BYTE[nLen * 15];
	SIZE_T nRead = 0;
	//获取机器码
	if (!ReadProcessMemory(g_hProc, pAddress, pOpCode, nLen*15, &nRead))
	{
		printf("读取进程内存失败");
		exit(0);
	}
	//使用反汇编引擎获取机器码对应的汇编
	DISASM da = { 0 };
	da.EIP = (UINT_PTR)pOpCode;
	da.VirtualAddr = (UINT64)pAddress;
#ifdef _WIN64
	da.Archi = 64;
#else
	da.Archi = 0;
#endif // _WIN64
	while (nLen--)
	{
		int len = Disasm(&da);
		if (len == -1)
		{
			break;
		}
		//输出
		printf("%I64X | %s\n", da.VirtualAddr, da.CompleteInstr);
		da.VirtualAddr += len;
		da.EIP += len;
	}
}

//分发事件
DWORD DispatchEvent(DEBUG_EVENT *pDbgEvent)
{
	//框架第二层
	//第二层框架将调试事件分为两部分来处理
	DWORD dwRet = 0;
	switch (pDbgEvent->dwDebugEventCode)
	{
	case EXCEPTION_DEBUG_EVENT:
		dwRet = DispatchException(&pDbgEvent->u.Exception);
		return dwRet;//返回到框架第一层
	case CREATE_PROCESS_DEBUG_EVENT:
		printf("被调试进程有进程被创建\n");
		break;
	case CREATE_THREAD_DEBUG_EVENT:
		printf("被调试进程有一个新线程被创建\n");
		break;
	case EXIT_PROCESS_DEBUG_EVENT:
		printf("被调试进程有一个进程退出\n");
		break;
	case EXIT_THREAD_DEBUG_EVENT:
		printf("被调试进程有一个线程退出\n");
		break;
	case LOAD_DLL_DEBUG_EVENT:
	{
		printf("被调试进程加载了一个新的DLL\n");
		OnDebugEvent_LOAD_DLL_DEBUG_EVENT(pDbgEvent);
	}
		break;
	case UNLOAD_DLL_DEBUG_EVENT:
	{
		printf("被调试进程卸载了一个DLL\n");
	}
		break;
	case OUTPUT_DEBUG_STRING_EVENT: break;
	case RIP_EVENT: break;
	default:
		break;
	}
	return DBG_CONTINUE;//其他调试事件
}

//分发异常
DWORD DispatchException(EXCEPTION_DEBUG_INFO *pDebugInfo)
  {
	//将软件断点再重新设置回去 为了实现永久断点
	SIZE_T read = 0;
	for (auto &bp : g_vBP)
	{
		//将上一个int3重新写入内存
		if (!WriteProcessMemory(g_hProc, bp.address, "\xCC", 1, &read))
		{
			printf("写入进程内存失败");
			exit(0);
		}
	}

	CONTEXT ct = { CONTEXT_CONTROL };
	//框架的第三层
	//第三层是专门修复异常的
	//如果是调试器自身设置的异常，可以修复，返回DEBUG_CONTINUE
	//如果不是，不能修复，返回DBG_EXCEPTION_NOT_HANDLED
	switch (pDebugInfo->ExceptionRecord.ExceptionCode)
	{
		//软件断点
	case EXCEPTION_BREAKPOINT:
	{
		//第一次进来是系统断点
		static bool isSysBP = true;
		if (isSysBP)
		{
			isSysBP = false;
			//在被调试程序的OEP下一个软件断点
			DWORD dwPage = 0;
			VirtualProtectEx(g_hProc, (LPVOID)g_OEP, 1, PAGE_EXECUTE_READWRITE, &dwPage);
			SetBP_int3((LPVOID)(g_OEP));
			DWORD dwPage1 = 0;
			VirtualProtectEx(g_hProc, (LPVOID)g_OEP, 1, dwPage, &dwPage1);

			//反反调试 peb隐藏
			PROCESS_BASIC_INFORMATION pbi = { 0 };
			NtQueryInformationProcess(g_hProc, ProcessBasicInformation, &pbi,
				sizeof(PROCESS_BASIC_INFORMATION), NULL);
			LPBYTE addr = (LPBYTE)pbi.PebBaseAddress + 2;
			SIZE_T write = 0;
			if (!WriteProcessMemory(g_hProc, addr, "\0", 1, &write))
			{
				exit(0);
			}
		}
		else
		{
			//断点断下来之后将int3指令还原 这样g之后就能继续执行代码
			ClearBP_int3(pDebugInfo->ExceptionRecord.ExceptionAddress);
			//如果设置了条件断点 判断条件是否成立 如果不成立直接跳过 成立就断下来
			if (g_conAddr == (DWORD)pDebugInfo->ExceptionRecord.ExceptionAddress)
			{
				CONTEXT ct = { CONTEXT_ALL };
				if (!GetThreadContext(g_hThread, &ct))
				{
					exit(0);
				}
				if (ct.Eax != g_conValue)
				{
					goto _DONE;
				}
			}
		}
	}
		break;
		//硬件断点和TF断点
	case EXCEPTION_SINGLE_STEP:
	{
		if (g_isMBPTF)
		{
			VirtualProtectEx(g_hProc, (LPVOID)g_pMemAddr, 1, g_nNewProtect, &g_nOldProtect);
			goto _DONE;
		}

		if (g_isDbgTF)
		{
			SetHardBPWork();
			goto _DONE;
		}

		//如果硬件断点被触发 用DR6来判断哪个硬件断点触发  然后用DR7让他们失效
		ClearHardBP();
	}
		break;
		//内存断点
	case EXCEPTION_ACCESS_VIOLATION:
	{
		BOOL isFind = FALSE;
		//如果找到了设置内存断点的地址 就让程序断下来 没有找到就继续执行
		if (g_pMemAddr == pDebugInfo->ExceptionRecord.ExceptionInformation[1])
		{
			isFind = TRUE;
		}
		
		if (!isFind)
		{
			VirtualProtectEx(g_hProc, (LPVOID)g_pMemAddr, 1, g_nOldProtect, &g_nNewProtect);
			SetBP_tf();
			g_isMBPTF = TRUE;
			goto _DONE;
		}
	}
		break;
	default:
		return DBG_EXCEPTION_NOT_HANDLED;
		break;
	}
	
	//获取线程上下文
	if (!GetThreadContext(g_hThread, &ct)) {
		printf("获取线程上下文失败");
		exit(0);
	}
	ShowDisasm((LPVOID)ct.Eip, 10);
	//和用户进行交互
	UserInput((LPVOID)ct.Eip);
	
	//返回到第二层框架中
_DONE:
	return DBG_CONTINUE;
}

void UserInput(LPVOID pAddress)
{
	//输出信息
	char buff[100];
	while (true)
	{
		printf("命令：");
		gets_s(buff, 100);
		if (!_stricmp(buff, "t"))//单步步入
		{
			SetBP_tf();
			g_isDbgTF = FALSE;
			g_isMBPTF = FALSE;
			break;
		}
		else if (!_stricmp(buff, "p"))//单步步过
		{
			StepBy();
			break;
		}
		else if (!_stricmp(buff, "g"))//继续执行
		{
			break;
		}
		else if (!_stricmp(buff, "bp"))//设置断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			SetBP_int3((LPVOID)addr);
		}
		else if (!_stricmp(buff, "hbp exec"))//设置硬件可执行断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			SetBP_hardExec((ULONG_PTR)addr);
		}
		else if (!_stricmp(buff, "hbp rw"))//设置硬件可读写断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			SetBP_hardRW((ULONG_PTR)addr, 3, 0);
		}
		else if (!_stricmp(buff, "mbp exec"))//设置内存可执行断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			g_pMemAddr = (ULONG_PTR)addr;
			SetBP_mem((LPVOID)addr, PAGE_READWRITE);
		}
		else if (!_stricmp(buff, "mbp rw"))//设置内存可读写断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			g_pMemAddr = (ULONG_PTR)addr;
			SetBP_mem((LPVOID)addr, PAGE_EXECUTE);
		}
		else if (!_stricmp(buff, "c"))//修改汇编代码
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			if (ModifyAsm((LPVOID)addr))
			{
				ShowDisasm(pAddress, 10);
			}
		}
		else if (!_stricmp(buff, "m"))//查看内存
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			ShowMemory((LPVOID)addr);
		}
		else if (!_stricmp(buff, "cm"))//修改内存
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			ModifyMemory((LPBYTE)addr);
			//ShowDisasm(pAddress, 10);
		}
		else if (!_stricmp(buff, "s"))//查看栈内存
		{
			ShowStack();
		}
		else if (!_stricmp(buff, "r"))//查看寄存器
		{
			ShowRegisters();
		}
		else if (!_stricmp(buff, "cr"))//修改寄存器
		{
			char name[12] = { 0 };
			scanf_s("%s", &name, 12);
			SIZE_T va = 0;
			scanf_s("%8X", &va);
			ModifyResisters(name, va);
		}
		else if (!_stricmp(buff, "module"))//查看进程模块
		{
			ShowModule();
		}
		else if (!_stricmp(buff, "q"))//查看进程模块导入导出表
		{
			printf("输入模块名:");
			char name[MAX_PATH] = { 0 };
			scanf_s("%s", &name, MAX_PATH);
			GetModuleTable(name);
		}
		else if (!_stricmp(buff, "u"))//设置条件断点
		{
			SIZE_T addr = 0;
			scanf_s("%8X", &addr);
			g_conAddr = addr;
			printf("eax == ");
			DWORD value = 0;
			scanf_s("%d", &value);
			g_conValue = value;
			SetBP_int3((LPVOID)addr);
		}
		else if (!_stricmp(buff, "i"))//调用插件
		{
			MyPlugin fun = (MyPlugin)GetProcAddress(g_hDll, "Plugin");
			char szBuff[32] = { "我是插件，虽然没用\n" };
			fun(szBuff);
		}
	}
}

int main()
{
	printf("1.open\n");
	printf("2.attach\n");
	DWORD type = 0;
	scanf_s("%d", &type);

	if (type == 1)
	{
		getchar();
		char path[MAX_PATH] = { 0 };
		printf("路径>");
		gets_s(path, MAX_PATH);
		strcpy_s(g_filePath, MAX_PATH, path);
		STARTUPINFOA sui = { sizeof(STARTUPINFOA) };
		PROCESS_INFORMATION pi = { 0 };

		/*创建调试进程*/
		CreateProcessA(path,
			0, 0, 0, FALSE, DEBUG_ONLY_THIS_PROCESS | CREATE_NEW_CONSOLE, 0, 0, &sui, &pi
		);
	}
	else if (type == 2)
	{
		printf("输入进程id：");
		DWORD pid = 0;
		scanf_s("%d", &pid);
		if (pid != 0)
		{
			if (!DebugActiveProcess(pid))
			{
				printf("打开错误！");
				exit(0);
			}
		}
	}
	else
	{
		printf("打开错误！");
		exit(0);
	}
	
	/*建立调试循环*/
	DEBUG_EVENT dbgEvent = { 0 };
	DWORD dwRet = DBG_CONTINUE;
	//加载动态库
	g_hDll = LoadLibrary(L"MyPlugin.dll");
	while (true)
	{
		/*框架的第一层*/
		//等待调试事件
		WaitForDebugEvent(&dbgEvent, -1);
		if (g_OEP == 0)
		{
			g_OEP = (DWORD)dbgEvent.u.CreateProcessInfo.lpStartAddress;
		}
		//打开进程线程
		g_hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, dbgEvent.dwProcessId);
		g_hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, dbgEvent.dwThreadId);
		dwRet = DispatchEvent(&dbgEvent);
		//回复调试子系统
		ContinueDebugEvent(dbgEvent.dwProcessId, dbgEvent.dwThreadId, dwRet);
		//关闭进程线程
		CloseHandle(g_hProc);
		CloseHandle(g_hThread);
	}

    return 0;
}

//设置软件断点，第一个参数是地址，第二个参数是否需要记录保存永久断点,如果是步过断点就不需要保存
void SetBP_int3(LPVOID pAddress)
{
	BREAKPOINT bp = { 0 };
	//将断点数据备份
	SIZE_T read = 0;
	if (!ReadProcessMemory(g_hProc, pAddress, &bp.oldData, 1, &read))
	{
		printf("读取进程内存失败");
		exit(0);
	}

	//将int3写入断点
	if (!WriteProcessMemory(g_hProc, pAddress, "\xCC", 1, &read))
	{
		printf("写入进程内存失败");
		exit(0);
	}

	bp.address = pAddress;
	g_vBP.push_back(bp);
}

void ClearBP_int3(LPVOID pAddress)
{
	//将容器保存的数据还原回去
	SIZE_T write = 0;
	for (auto &bp : g_vBP)
	{
		if (bp.address == pAddress)
		{
			if (!(WriteProcessMemory(g_hProc, pAddress, &bp.oldData, 1, &write)))
			{
				printf("写入进程内存失败");
				exit(0);
			}

			CONTEXT ct = { CONTEXT_CONTROL };
			if (!GetThreadContext(g_hThread, &ct))
			{
				printf("获取线程上下文失败");
				exit(0);
			}

			ct.Eip--;
			if (!SetThreadContext(g_hThread, &ct))
			{
				printf("设置线程上下文失败");
				exit(0);
			}
			//用于重新安装int3
			SetBP_tf();
			g_isDbgTF = TRUE;
		}
	}
}

//单步步入
void SetBP_tf()
{
	//获取线程上下文
	CONTEXT ct = { CONTEXT_CONTROL };
	if (!GetThreadContext(g_hThread, &ct))
	{
		printf("获取线程上下文失败");
		exit(0);
	}
	//修改TF标志位
	EFLAGS *pflag = (EFLAGS*)&ct.EFlags;
	pflag->TF = 1;

	if (!SetThreadContext(g_hThread, &ct))
	{
		printf("设置线程上下文失败");
		exit(0);
	}
}

//设置硬件可执行断点
BOOL SetBP_hardExec(ULONG_PTR pAddress)
{
	g_nDR_L = -1;
	CONTEXT ct = { CONTEXT_DEBUG_REGISTERS};
	GetThreadContext(g_hThread, &ct);
	DBG_REG7 *pDr7 = (DBG_REG7*)&ct.Dr7;
	if (pDr7->L0 == 0)
	{
		ct.Dr0 = pAddress;
		pDr7->RW0 = 0;
		pDr7->LEN0 = 0;
		pDr7->L0 = 1;
		g_nDR_L = 0;
	}
	else if (pDr7->L1 == 0)
	{
		ct.Dr1 = pAddress;
		pDr7->RW1 = 0;
		pDr7->LEN1 = 0;
		pDr7->L1 = 1;
		g_nDR_L = 1;
	}
	else if (pDr7->L2 == 0)
	{
		ct.Dr2 = pAddress;
		pDr7->RW2 = 0;
		pDr7->LEN2 = 0;
		pDr7->L2 = 1;
		g_nDR_L = 2;
	}
	else if (pDr7->L3 == 0)
	{
		ct.Dr3 = pAddress;
		pDr7->RW3 = 0;
		pDr7->LEN3 = 0;
		pDr7->L3 = 1;
		g_nDR_L = 3;
	}
	else
	{
		return FALSE;
	}
	SetThreadContext(g_hThread, &ct);
	return TRUE;
}

//设置硬件读写断点
BOOL SetBP_hardRW(ULONG_PTR pAddress, DWORD type, DWORD dwlen)
{
	CONTEXT ct = { CONTEXT_DEBUG_REGISTERS };
	GetThreadContext(g_hThread, &ct);

	if (dwlen == 1)
	{
		pAddress = pAddress - pAddress % 2;
	}
	else if (dwlen == 3)
	{
		pAddress = pAddress - pAddress % 4;
	}
	else if(dwlen > 3)
	{
		return FALSE;
	}

	DBG_REG7 *pDr7 = (DBG_REG7*)&ct.Dr7;
	if (pDr7->L0 == 0)
	{
		ct.Dr0 = pAddress;
		pDr7->RW0 = type;
		pDr7->LEN0 = dwlen;
		pDr7->L0 = 1;
		g_nDR_L = 0;
	}
	else if (pDr7->L1 == 0)
	{
		ct.Dr1 = pAddress;
		pDr7->RW1 = type;
		pDr7->LEN1 = dwlen;
		pDr7->L1 = 1;
		g_nDR_L = 1;
	}
	else if (pDr7->L2 == 0)
	{
		ct.Dr2 = pAddress;
		pDr7->RW2 = type;
		pDr7->LEN2 = dwlen;
		pDr7->L2 = 1;
		g_nDR_L = 2;
	}
	else if (pDr7->L3 == 0)
	{
		ct.Dr3 = pAddress;
		pDr7->RW3 = type;
		pDr7->LEN3 = dwlen;
		pDr7->L3 = 1;
		g_nDR_L = 3;
	}
	else
	{
		return FALSE;
	}
	SetThreadContext(g_hThread, &ct);
	return TRUE;
}

//将上次设置硬件断点启用 实现永久断点
void SetHardBPWork()
{
	if (g_nDR_L == -1)
	{
		return;
	}
	CONTEXT ct = { CONTEXT_DEBUG_REGISTERS };
	GetThreadContext(g_hThread, &ct);
	DBG_REG7 *pDr7 = (DBG_REG7*)&ct.Dr7;
	if (g_nDR_L == 0)
	{
		pDr7->L0 = 1;
	}
	else if (g_nDR_L == 1)
	{
		pDr7->L1 = 1;
	}
	else if (g_nDR_L == 2)
	{
		pDr7->L2 = 1;
	}
	else if (g_nDR_L == 3)
	{
		pDr7->L3 = 1;
	}
	SetThreadContext(g_hThread, &ct);
}

//禁用相应的硬件断点
void ClearHardBP()
{
	CONTEXT ct = { CONTEXT_DEBUG_REGISTERS };
	GetThreadContext(g_hThread, &ct);
	DBG_REG7 *pDr7 = (DBG_REG7*)&ct.Dr7;
	if (g_nDR_L == 0)
	{
		pDr7->L0 = 0;
	}
	else if (g_nDR_L == 1)
	{
		pDr7->L1 = 0;
	}
	else if (g_nDR_L == 2)
	{
		pDr7->L2 = 0;
	}
	else if (g_nDR_L == 3)
	{
		pDr7->L3 = 0;
	}
	SetThreadContext(g_hThread, &ct);
}


//设置内存断点
void SetBP_mem(LPVOID pAddress, DWORD protect)
{
	g_nNewProtect = protect;
	VirtualProtectEx(g_hProc, pAddress, 1, g_nNewProtect, &g_nOldProtect);
}

//单步步过
void StepBy()
{
	//当前指令执行到的地址
	CONTEXT ct = { CONTEXT_CONTROL };
	if (!GetThreadContext(g_hThread, &ct))
	{
		printf("获取线程上下文失败");
		exit(0);
	}
	LPVOID addr = (LPVOID)ct.Eip;

	PBYTE pOpCode = new BYTE[15];
	SIZE_T nRead = 0;
	//获取机器码
	if (!ReadProcessMemory(g_hProc, addr, pOpCode, 15, &nRead))
	{
		printf("读取进程内存失败");
		exit(0);
	}

	//使用反汇编引擎获取机器码对应的汇编
	DISASM da = { 0 };
	da.EIP = (UINT_PTR)pOpCode;
	da.VirtualAddr = (UINT64)addr;
#ifdef _WIN64
	da.Archi = 64;
#else
	da.Archi = 0;
#endif // _WIN64
	int len = Disasm(&da);
	if (!strcmp(da.Instruction.Mnemonic, "call "))
	{
		da.VirtualAddr += len;
		da.EIP += len;
		//在下一条指令设置上int3断点
		SetBP_int3((LPVOID)da.VirtualAddr);
	}
	else
	{
		g_isDbgTF = FALSE;
		SetBP_tf();
	}
}

//修改汇编代码 参数1需要修改汇编代码的首地址
BOOL ModifyAsm(LPVOID pAdress)
{
	XEDPARSE xed = { 0 };
	xed.cip = (ULONGLONG)pAdress;

	// 接收指令
	printf("指令：");
	getchar();
	gets_s(xed.instr, XEDPARSE_MAXBUFSIZE);

	// xed.cip, 汇编带有跳转偏移的指令时,需要配置这个字段
	if (XEDPARSE_OK != XEDParseAssemble(&xed))
	{
		printf("指令错误：%s\n", xed.error);
		return FALSE;
	}

	//使用反汇编引擎获取机器码对应的汇编
	int nLen = 10;
	int nCount = 0;
	LPBYTE pOpCode = new BYTE[64];
	SIZE_T read = 0;
	//获取机器码
	if (!ReadProcessMemory(g_hProc, pAdress, pOpCode, 64, &read))
	{
		printf("读取进程内存失败");
		exit(0);
	}

	DISASM da = { 0 };
	da.EIP = (UINT_PTR)pOpCode;
	da.VirtualAddr = (UINT64)pAdress;
#ifdef _WIN64
	da.Archi = 64;
#else
	da.Archi = 0;
#endif // _WIN64
	while (nLen--)
	{
		//获取需要用NOP填充的字节数
		int len = Disasm(&da);
		if (nCount >= xed.dest_size)
		{
			break;
		}
		da.VirtualAddr += len;
		da.EIP += len;
		nCount += len;
	}

	SIZE_T write = 0;
	//将NOP填充进入被调试程序
	if (!WriteProcessMemory(g_hProc, pAdress, "\90", nCount, &write))
	{
		printf("写入进程内存失败");
		exit(0);
	}

	//将OPCODE写入内存
	SIZE_T write1 = 0;
	if (!WriteProcessMemory(g_hProc, pAdress, xed.dest, xed.dest_size, &write1))
	{
		printf("写入进程内存失败");
		exit(0);
	}
	return TRUE;
}

// 打印opcode
void printOpcode(LPBYTE pOpcode, int nSize)
{
	for (int i = 0; i < nSize; ++i)
	{
		DWORD tmp = pOpcode[i];
		printf("%02X ", tmp);
	}
	printf("\n");
}

//查看内存
void ShowMemory(LPVOID pAddress)
{
	LPBYTE pByte = new BYTE[15];
	SIZE_T read = 0;
	//获取机器码
	if (!ReadProcessMemory(g_hProc, pAddress, pByte, 15, &read))
	{
		printf("读取内存失败");
		exit(0);
	}

	printOpcode(pByte, 15);
}

//修改内存
void ModifyMemory(LPBYTE pAddress)
{
	for (DWORD i = 0; i < 5; ++i)
	{
		DWORD tmp;
		scanf_s("%X", &tmp);

		SIZE_T write = 0;
		if (!WriteProcessMemory(g_hProc, (pAddress+i), &tmp, 1, &write))
		{
			printf("写入进程内存失败");
			exit(0);
		}
	}
}

//查看栈
void ShowStack()
{
	printf("栈内存:\n");

	CONTEXT ct = {CONTEXT_ALL};
	//获取线程上下文
	if (!GetThreadContext(g_hThread, &ct)) {
		printf("获取线程上下文失败");
		exit(0);
	}

	LPBYTE pByte = new BYTE[100];
	SIZE_T read = 0;
	//获取机器码
	if (!ReadProcessMemory(g_hProc, (LPCVOID)ct.Esp, pByte, 100, &read))
	{
		printf("读取内存失败");
		exit(0);
	}

	printOpcode(pByte, 100);
}

//查看寄存器
void ShowRegisters()
{
	CONTEXT ct = { CONTEXT_ALL };

	//获取线程上下文
	if (!GetThreadContext(g_hThread, &ct)) {
		printf("获取线程上下文失败");
		exit(0);
	}

	//输出寄存器的值
	printf("Eax = %X\n", ct.Eax);
	printf("Ebx = %X\n", ct.Ebx);
	printf("Ecx = %X\n", ct.Ecx);
	printf("Edx = %X\n", ct.Edx);
	printf("Eip = %X\n", ct.Eip);
	printf("Edi = %X\n", ct.Edi);
	printf("Esi = %X\n", ct.Esi);
	printf("Ebp = %X\n", ct.Ebp);
	printf("Esp = %X\n", ct.Esp);
	printf("SegCs = %X\n", ct.SegCs);
	printf("SegSs = %X\n", ct.SegSs);
	
}

//修改寄存器
void ModifyResisters(const char *pStr, DWORD value)
{
	CONTEXT ct = { CONTEXT_ALL };
	//获取线程上下文
	if (!GetThreadContext(g_hThread, &ct)) {
		printf("获取线程上下文失败");
		exit(0);
	}

	if (!_stricmp(pStr, "eax"))
	{
		ct.Eax = value;
	}
	else if (!_stricmp(pStr, "ebx"))
	{
		ct.Ebx = value;
	}
	else if (!_stricmp(pStr, "ecx"))
	{
		ct.Ecx = value;
	}
	else if (!_stricmp(pStr, "edx"))
	{
		ct.Edx = value;
	}
	else if (!_stricmp(pStr, "esi"))
	{
		ct.Esi = value;
	}
	else if (!_stricmp(pStr, "edi"))
	{
		ct.Edi = value;
	}
	else if (!_stricmp(pStr, "esp"))
	{
		ct.Esp = value;
	}
	else if (!_stricmp(pStr, "eip"))
	{
		ct.Eip = value;
	}
	else if (!_stricmp(pStr, "ebp"))
	{
		ct.Ebp = value;
	}

	if (!SetThreadContext(g_hThread, &ct))
	{
		printf("设置线程上下文失败");
		exit(0);
	}
}

//显示进程模块
void ShowModule()
{
	for (auto &ele : g_map)
	{
		std::cout<< ele.first << std::endl;
	}
}

//获取模块的导入导出表
void GetModuleTable(LPCSTR pszName)
{
	//模块基址
	LPBYTE pBase = (LPBYTE)g_map[pszName];
	LPBYTE pBuff = new BYTE[2000];
	SIZE_T read = 0;
	if (!ReadProcessMemory(g_hProc, pBase, pBuff, 2000, &read))
	{
		exit(0);
	}

	IMAGE_DOS_HEADER *pDos = (IMAGE_DOS_HEADER*)pBuff;
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return;
	}
	IMAGE_NT_HEADERS *pNt = (IMAGE_NT_HEADERS*)(pBuff + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE)
	{
		return;
	}
	//读出整个模块的PE文件
	pBuff = new BYTE[pNt->OptionalHeader.SizeOfImage];
	if (!ReadProcessMemory(g_hProc, pBase, pBuff, pNt->OptionalHeader.SizeOfImage, &read))
	{
		exit(0);
	}
	//判断是否有导出表
	if (pNt->OptionalHeader.DataDirectory[0].VirtualAddress != 0)
	{
		printf("导出表:\n");
		//获取导出表的地址
		IMAGE_EXPORT_DIRECTORY *pExport = (IMAGE_EXPORT_DIRECTORY*)(pNt->OptionalHeader.DataDirectory[0].VirtualAddress
			+ pBuff);
		printf("函数数量:%d\n", pExport->NumberOfFunctions);
		printf("函数名称数量:%d\n", pExport->NumberOfNames);
		if (pExport->NumberOfFunctions != 0)
		{
			//获取序号表 函数地址表 函数名称表
			DWORD *pEAT = (DWORD*)(pExport->AddressOfFunctions + pBuff);
			DWORD *pENT = (DWORD*)(pExport->AddressOfNames + pBuff);
			WORD *pEOT = (WORD*)(pExport->AddressOfNameOrdinals + pBuff);
			printf("函数地址表:\n");
			for (DWORD i = 0; i < pExport->NumberOfFunctions; ++i)
			{
				printf("0x%x\n", pEAT[i]);
			}
			printf("函数名称表:\n");
			for (DWORD i = 0; i < pExport->NumberOfFunctions; ++i)
			{
				CHAR *szCh = (CHAR*)(pENT[i] + pBuff);
				printf("%s\n", szCh);
			}
			printf("函数序号表:\n");
			for (DWORD i = 0; i < pExport->NumberOfFunctions; ++i)
			{

				printf("%d\n", pEOT[i]);
			}
		}
	}
	else
	{
		printf("没有导出表\n");
	}
	
	//判断是否有导入表
	if (pNt->OptionalHeader.DataDirectory[1].VirtualAddress)
	{
		printf("导入表:\n");
		//获取导入表地址
		IMAGE_IMPORT_DESCRIPTOR *pImport = (IMAGE_IMPORT_DESCRIPTOR*)(pNt->OptionalHeader.DataDirectory[1].VirtualAddress
			+ pBuff);
		while (pImport->Name != 0)
		{
			printf("导入模块:%s", (char*)(pImport->Name + pBuff));
			//获取INT
			IMAGE_THUNK_DATA* pINT = (IMAGE_THUNK_DATA*)(pImport->OriginalFirstThunk + pBuff);
			while (pINT->u1.Function != 0)
			{
				//先判断导入的方式，以序号还是名称
				if (IMAGE_SNAP_BY_ORDINAL(pINT->u1.Ordinal))
				{
					printf("%d\n", pINT->u1.Ordinal);
				}
				else
				{
					IMAGE_IMPORT_BY_NAME *pIntName = (IMAGE_IMPORT_BY_NAME*)
						(pINT->u1.AddressOfData + pBuff);
					printf("%s\n", pIntName->Name);
				}
				pINT++;
			}

			pImport++;
		}

	}
	else
	{
		printf("没有导入表\n");
	}
}

void Wchar_tToString(std::string& szDst, wchar_t *wchar)
{
	wchar_t * wText = wchar;
	DWORD dwNum = WideCharToMultiByte(CP_OEMCP, NULL, wText, -1, NULL, 0, NULL, FALSE);// WideCharToMultiByte的运用
	char *psText; // psText为char*的临时数组，作为赋值给std::string的中间变量
	psText = new char[dwNum];
	WideCharToMultiByte(CP_OEMCP, NULL, wText, -1, psText, dwNum, NULL, FALSE);// WideCharToMultiByte的再次运用
	szDst = psText;// std::string赋值
	delete[]psText;// psText的清除
}

//获取被调试程序名字
void OnDebugEvent_LOAD_DLL_DEBUG_EVENT(DEBUG_EVENT* dbgEvent) 
{
	LOAD_DLL_DEBUG_INFO* pInfo = NULL;
	WCHAR szBuf[MAX_PATH];
	SIZE_T nNumberOfBytesRead = 0;
	DWORD dwAddrImageName = 0;

	pInfo = &dbgEvent->u.LoadDll;
	if ((NULL != pInfo) && (NULL != pInfo->lpImageName)) {
		// 读目标DLL导出表中的dll名称吧
		do {
			if (!ReadProcessMemory(g_hProc, pInfo->lpImageName, &dwAddrImageName, sizeof(dwAddrImageName), &nNumberOfBytesRead)) {
				break;
			}

			if (0 == dwAddrImageName) {
				break;
			}

			if (!ReadProcessMemory(g_hProc, (void*)dwAddrImageName, &szBuf, sizeof(szBuf), &nNumberOfBytesRead)) {
				break;
			}

			if (0 == pInfo->fUnicode) {
				// ansi
				printf("Load dll : %s\n", szBuf);
			}
			else {
				// unicode
				wprintf(L"Load dll : %s\n", szBuf);
				std::string str;
				Wchar_tToString(str, szBuf);
				g_map[str] = (DWORD)pInfo->lpBaseOfDll;
			}
		} while (0);
	}
}