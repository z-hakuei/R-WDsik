#include <Windows.h>
#include <iostream>
#include <process.h>
#include <ctime>

namespace{
	clock_t oldTime = clock();
	clock_t newTime = clock();

	using std::cout;
	using std::endl;

	const size_t BufferSize(1024*1024);
	const int nBuffer(4);
	const UINT SECTOR_SIZE(512);

	double ProgressOld(0);
	double ProgressNew(0);

	HANDLE hInFile;
	HANDLE hOutFile;

	LONGLONG nFileSize = 0;
	LONGLONG nCurrentRead = 0;
	LONGLONG nCurrentWritten = 0;
	LONGLONG nLastWritten = 0;

	BYTE* multTBuffer[2];
	CRITICAL_SECTION lockBuffer[2];
	CONDITION_VARIABLE BufferReadyToBeRead[2];
	CONDITION_VARIABLE BufferReadyToBeWrite[2];

	DWORD mnNumberOfBytesRead[2] = {0};
	DWORD mnNumberOfBytesWritten[2] = {0};

	enum {TO_BE_READ = 0, TO_BE_WRITE = 1};
	BOOL BufferState[2];
}
// 输出进度条
inline void ShowProgressBar(double Progress){
	int n = static_cast<int>(Progress);
	system("cls");
	for (int i = 0; i < n; i++)cout << ">";
	for (int i = 0; i < 99 - n; i++) cout << "-";
	cout << "||" << (int)Progress << "" << "% ";
}

// 获取磁盘大小
ULONGLONG GetDiskSpace(HANDLE hDevice){
	SENDCMDINPARAMS SendCmdInParams = {0};
	SendCmdInParams.irDriveRegs.bCommandReg = ID_CMD;   
	DWORD dwSize = sizeof(SENDCMDOUTPARAMS) + 512;
	PSENDCMDOUTPARAMS pSendCmdOutParams = (PSENDCMDOUTPARAMS)(new BYTE[dwSize]);
	pSendCmdOutParams->cBufferSize = 512;

	if(!::DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA, &SendCmdInParams, sizeof(SENDCMDINPARAMS),
		pSendCmdOutParams, dwSize, &dwSize, NULL))
	{
		delete pSendCmdOutParams;	
		pSendCmdOutParams = NULL;
		return FALSE;
	}

	// 获取设备容量
	ULONGLONG ullSize = 0;
	ullSize = *(ULONGLONG*)(pSendCmdOutParams->bBuffer + 0xC8);//C8

	delete pSendCmdOutParams;
	pSendCmdOutParams = NULL;
	return ullSize;
}

// 单线程单缓冲区拷贝
void Copy_1Thread1Buff(const wchar_t* inPath, const wchar_t* outPath){
	hInFile = CreateFile(inPath, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (hInFile == INVALID_HANDLE_VALUE){
		cout << GetLastError() << " in" << endl;
		return;
	}
	hOutFile = CreateFile(outPath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (hOutFile == INVALID_HANDLE_VALUE){
		cout << GetLastError() << " out" << endl;
		return;
	}
	nFileSize = GetDiskSpace(hInFile);
	BYTE* pBuffer = new BYTE[BufferSize];
	DWORD nNumberOfBytesRead;
	DWORD nNumberOfBytesWritten;
	do {
		ReadFile(hInFile, pBuffer, BufferSize, &nNumberOfBytesRead, NULL);
		WriteFile(hOutFile, pBuffer, nNumberOfBytesRead, &nNumberOfBytesWritten, NULL);
		nCurrentWritten += nNumberOfBytesWritten / SECTOR_SIZE;
		newTime = clock();
		if (newTime - oldTime >= 500)
		{
			oldTime = newTime;
			ProgressNew = 100 * ((double)nCurrentWritten / (double)nFileSize);
			ProgressOld = ProgressNew;
			ShowProgressBar(ProgressNew);
			cout << (nCurrentWritten - nLastWritten) / 2 / 1024 << "MB/S" << " " << nCurrentWritten << endl;
			nLastWritten = nCurrentWritten;
		}
	} while (nCurrentWritten != nFileSize);
	delete[] pBuffer;
	CloseHandle(hInFile);
	CloseHandle(hOutFile);
}

// 单线程多缓冲区拷贝
void Copy_1ThreadNBuff(const wchar_t* inPath, const wchar_t* outPath){
	hInFile = CreateFile(inPath, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	hOutFile = CreateFile(outPath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (hOutFile == INVALID_HANDLE_VALUE||hInFile == INVALID_HANDLE_VALUE)return;
	nFileSize = GetDiskSpace(hInFile);
	OVERLAPPED oBuffer[nBuffer];
	BYTE* pBuffer[nBuffer];
	for (int i = 0; i < nBuffer; i++){
		HANDLE hInitEvent = CreateEvent(0, FALSE, TRUE, NULL);
		pBuffer[i] = new BYTE[BufferSize];
		memset(&(oBuffer[i]), 0, sizeof(OVERLAPPED));
		oBuffer[i].hEvent = hInitEvent;
	}
	DWORD nNumberOfBytesRead;
	while(nFileSize > nCurrentWritten){
		for (int i = 0; i < nBuffer; i++){
			if (WaitForSingleObject(oBuffer[i].hEvent, 10) == WAIT_OBJECT_0)
				ReadFile(hInFile, pBuffer[i], BufferSize, &nNumberOfBytesRead, NULL);
			WriteFile(hOutFile, pBuffer[i], nNumberOfBytesRead, NULL, &(oBuffer[i]));
			nCurrentWritten += nNumberOfBytesRead / SECTOR_SIZE;
			ULONGLONG offset = (nCurrentWritten * (ULONGLONG)SECTOR_SIZE);
			oBuffer[(i + 1) % nBuffer].Offset = offset & 0x00000000FFFFFFFF;
			oBuffer[(i + 1) % nBuffer].OffsetHigh = offset >> 32;
		}
		// 输出进度条
		newTime = clock();
		if (newTime - oldTime >= 500)
		{
			oldTime = newTime;
			ProgressNew = 100 * ((double)nCurrentWritten / (double)nFileSize);
			ProgressOld = ProgressNew;
			ShowProgressBar(ProgressNew);
			cout << (nCurrentWritten - nLastWritten) / 1024 / 2 << "MB/S" << " " << nCurrentWritten << endl;
			nLastWritten = nCurrentWritten;
		}
	}
	// 释放缓冲区申请的内存
	for (int i = 0; i < nBuffer; i++){
		delete[] pBuffer[i];
	}
	CloseHandle(hInFile);
	CloseHandle(hOutFile);
}

// 双线程双缓冲区拷贝

// 读线程
DWORD WINAPI ReadThread(PVOID pvParam){
	while (true){
		if (nFileSize == nCurrentWritten)break;
		for (int i = 0; i < 2; i++){
			EnterCriticalSection(&(lockBuffer[i]));
			while (TO_BE_READ == BufferState[i]){
				SleepConditionVariableCS(&(BufferReadyToBeWrite[i]), &(lockBuffer[i]), INFINITE);
			}
			ReadFile(hInFile, multTBuffer[i], BufferSize, &(mnNumberOfBytesRead[i]), NULL);
			nCurrentRead += mnNumberOfBytesRead[i];
			BufferState[i] = TO_BE_READ;
			LeaveCriticalSection(&(lockBuffer[i]));
			WakeConditionVariable(&(BufferReadyToBeRead[i]));
		}
	}
	return 0;
}

// 写线程
DWORD WINAPI WriteThread(PVOID pvParam){
	while(true){
		if (nFileSize == nCurrentWritten)break;
		for (int i = 0; i < 2; i++){
			EnterCriticalSection(&(lockBuffer[i]));
			while (TO_BE_WRITE == BufferState[i]){
				SleepConditionVariableCS(&(BufferReadyToBeRead[i]), &(lockBuffer[i]),INFINITE);
			}
			WriteFile(hOutFile, multTBuffer[i], mnNumberOfBytesRead[i], &(mnNumberOfBytesWritten[i]), NULL);
			nCurrentWritten += mnNumberOfBytesWritten[i] / SECTOR_SIZE;
			BufferState[i] = TO_BE_WRITE;
			LeaveCriticalSection(&(lockBuffer[i]));

			newTime = clock();
			if (newTime - oldTime >= 500)
			{
				oldTime = newTime;
				ProgressNew = 100 * ((double)nCurrentWritten / (double)nFileSize);
				ProgressOld = ProgressNew;
				ShowProgressBar(ProgressNew);
				cout << (nCurrentWritten - nLastWritten) / 1024 / 2 << "MB/S" << " " << nCurrentWritten << endl;
				nLastWritten = nCurrentWritten;
			}
			WakeConditionVariable(&(BufferReadyToBeWrite[i]));
		}
	}
	return 0;
}

void Copy_2Thread2Buff(const wchar_t* inPath, const wchar_t* outPath){
	hInFile = CreateFile(inPath, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	hOutFile = CreateFile(outPath, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (hOutFile == INVALID_HANDLE_VALUE||hInFile == INVALID_HANDLE_VALUE)return;
	nFileSize = GetDiskSpace(hInFile);
	for (int i = 0; i < 2; i++){
		multTBuffer[i] = new BYTE[BufferSize];
		InitializeCriticalSectionAndSpinCount(&(lockBuffer[i]), 4000);
		InitializeConditionVariable(&(BufferReadyToBeRead[i]));
		InitializeConditionVariable(&(BufferReadyToBeWrite[i]));
		BufferState[i] = TO_BE_WRITE;
	}

	HANDLE hRead = CreateThread(NULL, 0, ReadThread, (PVOID)1, 0, NULL);
	HANDLE hWrite = CreateThread(NULL, 0, WriteThread, (PVOID)1, 0, NULL);

	WaitForSingleObject (hRead, INFINITE);
	WaitForSingleObject (hWrite, INFINITE);

	for (int i = 0; i < nBuffer; i++){
		delete[] multTBuffer[i];
	}
	CloseHandle(hInFile);
	CloseHandle(hOutFile);
}
int main(){
	Copy_2Thread2Buff(L"\\\\.\\PHYSICALDRIVE1", L"\\\\.\\PHYSICALDRIVE2");
	cout << sizeof(PVOID) <<endl;
	return 0;
}
