#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

// Windows Vista is the minimum - CancelIoEx (used by the sndStream fix) is only declared by the SDK headers
// when _WIN32_WINNT >= 0x0600, and it is imported from kernel32 unconditionally anyway
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <Xinput.h>
#include <Shlwapi.h>
#include <Shlobj.h>
#include <mmsystem.h>

#include "Utils/MemoryMgr.h"
#include "PoolsBully.h"
#include "DefaultControllerConfig.h"

#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

#ifndef NDEBUG

#define INCLUDE_MEMORY_CHECKS 1
#define COMPILE_TEST_VERSION 0 // Timestamp instead of build ID in menu

#else

#define INCLUDE_MEMORY_CHECKS 0
#define COMPILE_TEST_VERSION 0 

#endif

#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)


static HINSTANCE hDLLModule;
static HANDLE hShutdownEvent = NULL;

// Path to SilentPatchBully.ini next to the ASI, filled in by InjectHooks before anything reads it
static wchar_t g_iniPath[MAX_PATH];

// Immediate operand of the instruction patched by the FPSLimit INI option - the FPS cap value, INT_MAX = uncapped
static int32_t* const FPS_CAP_OPERAND = reinterpret_cast<int32_t*>(0x40618F + 1);

// ============= Optional diagnostics log (LogFile=1 in the INI) =============
namespace Log
{
	static HANDLE hLogFile = INVALID_HANDLE_VALUE;
	static CRITICAL_SECTION logLock;

	static void Init(const wchar_t* iniPath, bool enabled)
	{
		if (!enabled)
		{
			return;
		}
		InitializeCriticalSection(&logLock);

		wchar_t logPath[MAX_PATH];
		wcscpy_s(logPath, iniPath);
		PathRenameExtensionW(logPath, L".log");
		hLogFile = CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	static bool IsEnabled()
	{
		return hLogFile != INVALID_HANDLE_VALUE;
	}

	// printf-style with a narrow format string, use %ls for wide strings
	static void Write(const char* format, ...)
	{
		if (!IsEnabled())
		{
			return;
		}

		char line[1024];
		SYSTEMTIME time;
		GetLocalTime(&time);
		const int prefixLength = sprintf_s(line, "[%02u:%02u:%02u.%03u] ", static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond), static_cast<unsigned>(time.wMilliseconds));
		if (prefixLength < 0)
		{
			return;
		}
		size_t length = static_cast<size_t>(prefixLength);

		const size_t capacity = sizeof(line) - length - 2; // Leave room for the line break
		va_list args;
		va_start(args, format);
		const int written = vsnprintf(line + length, capacity, format, args);
		va_end(args);
		if (written < 0)
		{
			return;
		}
		length += static_cast<size_t>(written) >= capacity ? capacity - 1 : static_cast<size_t>(written); // Truncates overlong lines
		line[length++] = '\r';
		line[length++] = '\n';

		EnterCriticalSection(&logLock);
		DWORD bytesWritten = 0;
		WriteFile(hLogFile, line, static_cast<DWORD>(length), &bytesWritten, nullptr);
		LeaveCriticalSection(&logLock);
	}
}

namespace FixedAllocators
{
	void InitMemoryMgr()
	{
		// Do nothing
	}

	void ShutDownMemoryMgr()
	{
		// Do nothing
	}

#if INCLUDE_MEMORY_CHECKS
	static constexpr size_t MEMORY_PROLOGUE_SIZE = sizeof(size_t) + sizeof(uint32_t);
	static constexpr size_t MEMORY_EPILOGUE_SIZE = sizeof(uint32_t);
	static constexpr size_t MEMORY_CANARIES_TOTAL_SIZE = MEMORY_PROLOGUE_SIZE + MEMORY_EPILOGUE_SIZE;

	static constexpr uint32_t MEMORY_CANARY = 0xDFDFDFDF;
#endif

	void* MemoryMgrMalloc(size_t size)
	{
		if (size == 0)
		{
			return nullptr;
		}

		// Their malloc is actually calloc, as allocated memory gets zeroed
#if INCLUDE_MEMORY_CHECKS
		// Debug memory is structured as follows:
		// Allocated size
		// FDFDFDFD
		// Allocated space
		// FDFDFDFD

		void* memory = calloc(size + MEMORY_CANARIES_TOTAL_SIZE, 1);
		assert(memory != nullptr);

		uintptr_t memStart = uintptr_t(memory);
		*(size_t*)memStart = size;
		*(uint32_t*)(memStart + sizeof(size_t)) = MEMORY_CANARY;
		*(uint32_t*)(memStart + MEMORY_PROLOGUE_SIZE + size) = MEMORY_CANARY;

		return (void*)(memStart + MEMORY_PROLOGUE_SIZE);
#else
		return calloc(size, 1);
#endif
	}

	void MemoryMgrFree(void* data)
	{
		if (data != nullptr)
		{
#if INCLUDE_MEMORY_CHECKS
			uintptr_t mem = uintptr_t(data);
			uint32_t startCanary = *(uint32_t*)(mem - sizeof(uint32_t));
			assert(startCanary == MEMORY_CANARY);
			*(uint32_t*)(mem - sizeof(uint32_t)) = ~MEMORY_CANARY;

			// If start canary is valid, we can check the end canary (since size is probably valid too)
			size_t size = *(size_t*)(mem - MEMORY_PROLOGUE_SIZE);
			uint32_t endCanary = *(uint32_t*)(mem + size);
			assert(endCanary == MEMORY_CANARY);
			*(uint32_t*)(mem + size) = ~MEMORY_CANARY;

			free((void*)(mem - MEMORY_PROLOGUE_SIZE));
#else
			free(data);
#endif
		}
	}

	void* RwMallocAlign(size_t size, size_t align)
	{
		if (size == 0)
		{
			return nullptr;
		}
		void* memory = _aligned_malloc(size, align);
		assert(memory != nullptr);
		return memory;
	}

	void RwFreeAlign(void* data)
	{
		if (data != nullptr)
		{
			_aligned_free(data);
		}
	}

	void __stdcall MemoryHeap_Free(void* data)
	{
		MemoryMgrFree(data);
	}

	void* __stdcall MemoryHeap_MoveMemoryBully(void* data)
	{
		// Do NOT move
		return data;
	}

	size_t __stdcall MemoryHeap_GetMemoryUsed(int)
	{
		return 0;
	}

};

namespace PedTypeShutdown
{
	static void (*orgOperatorDelete)(void* data);
	void OperatorDelete_Safe(void** pData)
	{
		if (*pData != nullptr)
		{
			orgOperatorDelete(*pData);
			*pData = nullptr;
		}
	}
}

namespace FrameTimingFix
{
	void (*orgUpdateTimer)(bool);
	void UpdateTimerAndSleep(bool captureInput)
	{
		orgUpdateTimer(captureInput);
		Sleep(100);
	}

	// ===== Hybrid frame limiter wait (FrameLimiterSleep=1 in the INI) =====
	// The game's limiter loops "while (frame is too short) Sleep(1);". By default SilentPatch removes the Sleep so the
	// loop spins, which is precise but keeps a CPU core fully busy. This replacement for the Sleep call instead sleeps
	// in short quanta while the estimated deadline is still far away and spins only for the last few milliseconds.
	// The game re-checks its own timer after every call, so a wrong estimate can only cost a single quantum of overshoot.

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

	static LONGLONG qpcFrequency = 0;
	static HANDLE hWaitTimer = nullptr;
	static LONGLONG waitQuantum100ns = 10000; // 1ms
	static LONGLONG spinWindowTicks = 0;
	static LONGLONG lastReturnTicks = 0;
	static LONGLONG deadlineEstimateTicks = 0;

	static LONGLONG Now()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		return counter.QuadPart;
	}

	static bool InitHybridWait()
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		qpcFrequency = frequency.QuadPart;

		// High resolution timers (Windows 10 1803 and newer) wake up within roughly half a millisecond,
		// older systems need timeBeginPeriod(1) and wake up within one to two milliseconds
		hWaitTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (hWaitTimer != nullptr)
		{
			waitQuantum100ns = 5000; // 0.5ms
			spinWindowTicks = (qpcFrequency * 2) / 1000; // Spin for the last 2ms
		}
		else
		{
			hWaitTimer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
			timeBeginPeriod(1);
			waitQuantum100ns = 10000; // 1ms
			spinWindowTicks = (qpcFrequency * 3) / 1000; // Spin for the last 3ms
		}
		return hWaitTimer != nullptr;
	}

	// Replaces the Sleep(1) call inside the game's frame limiter loop (cdecl, no arguments - the original push is removed too)
	void HybridWait()
	{
		const LONGLONG now = Now();

		// A gap since the last return means the game rendered a frame in between, so this is a new limiter session
		// whose deadline is one frame period after the previous session ended. Calls within a session are back to back.
		if (now - lastReturnTicks > qpcFrequency / 5000) // 0.2ms
		{
			const int32_t fpsCap = *FPS_CAP_OPERAND;
			deadlineEstimateTicks = fpsCap > 0 ? lastReturnTicks + qpcFrequency / fpsCap : now;
		}

		if (deadlineEstimateTicks - now > spinWindowTicks)
		{
			LARGE_INTEGER dueTime;
			dueTime.QuadPart = -waitQuantum100ns; // Negative = relative
			if (SetWaitableTimer(hWaitTimer, &dueTime, 0, nullptr, nullptr, FALSE) != FALSE)
			{
				WaitForSingleObject(hWaitTimer, INFINITE);
			}
		}
		else
		{
			YieldProcessor();
		}

		lastReturnTicks = Now();
	}
}

#ifdef _DEBUG
#include <intrin.h>
#endif

namespace DoubleFreeOnExitFix
{
	class UnkInnerStruct
	{
	public:
		virtual void Unknown() = 0;
		virtual void Release() = 0;

		LONG refCount;

	public:
		void ReleaseTexture()
		{
			if (_InterlockedDecrement(&refCount) == 0)
			{
				Release();
			}
		}
	};

	class UnkStruct
	{
	public:
		uint8_t		__pad[52];
		UnkInnerStruct* m_pInner;
	};

	const auto FreeStruct = (void(*)(UnkStruct * ptr))0x752380;
	uint32_t ReleaseStruct(UnkStruct* ptr)
	{
		if (ptr == nullptr) return 1;
		auto* inner = ptr->m_pInner;
		if (inner == nullptr)
		{
			FreeStruct(ptr);
			return 1;
		}

		ptr->m_pInner = nullptr;
		inner->ReleaseTexture();

		return 1;
	}

	void ReleaseTextureAndNull(UnkInnerStruct** pPtr)
	{
		if (*pPtr != nullptr)
		{
			(*pPtr)->ReleaseTexture();
			*pPtr = nullptr;
		}
	}

	void RwTextureAddRef(UnkInnerStruct* ptr)
	{
		_InterlockedIncrement(&ptr->refCount);
	}

	uint32_t ReleaseTextureDebug(UnkInnerStruct* ptr)
	{
#ifndef _DEBUG
		ptr->ReleaseTexture();
#else
		assert(ptr->refCount > 0);
		void** mem = reinterpret_cast<void**>(ptr);
		if (_InterlockedDecrement(&ptr->refCount) == 0)
		{
			*mem = _ReturnAddress();
		}
#endif
		return 1;
	}

	static void (*orgPopTimer)();
	void PopTimer_AddMissingReferences()
	{
		UnkInnerStruct** texture = (UnkInnerStruct**)0xC66E40;
		for (size_t i = 0; i < 8; i++)
		{
			RwTextureAddRef(texture[i]);
		}

		orgPopTimer();
	}
};

// ============= Fix use-after-free when sndStream gets released before overlapped read finishes =============
class sndStream // Documented by P3ti
{
private:
	DWORD state;
	BYTE _pad_1_[16];
	DWORD mode;
	BYTE _pad_2_[44];
	float position[3];
	DWORD _80;
	int waveBankId;
	int soundBankId;
	int soundCueId;
	DWORD size;
	DWORD soundBankBuffer;
	HANDLE file;
	OVERLAPPED overlapped;
	DWORD playListId;
	DWORD fileReadStatus;
	DWORD playListEntry;
	DWORD offset;
	BYTE _pad_3_[272];

public:
	static inline void (sndStream::* orgCleanupStreamInfo)();
	void CleanupStreamInfo()
	{
		std::invoke(orgCleanupStreamInfo, this);
	}

	void CleanupStreamInfo_SilentPatch()
	{
		if (fileReadStatus == 2 || fileReadStatus == 3)
		{
			BOOL result = CancelIoEx(file, &overlapped);
			if (result != FALSE)
			{
				DWORD numberOfBytesTransferred;
				result = GetOverlappedResult(file, &overlapped, &numberOfBytesTransferred, TRUE);
				assert(result != FALSE);
			}
		}

		if (overlapped.hEvent != nullptr)
		{
			CloseHandle(overlapped.hEvent);
			overlapped.hEvent = nullptr;
		}

		CleanupStreamInfo();
	}
};

static_assert(sizeof(sndStream) == 0x1A0, "Wrong size: sndStream");

// ============= Fix handle leaks in ScreamSoundBank =============
class ScreamSoundBank // Documented by P3ti
{
private:
	BYTE _pad_1_[12];
	OVERLAPPED overlapped;
	BYTE _pad_2_[288];

public:
	static inline void (ScreamSoundBank::* orgKill)();
	void Kill()
	{
		std::invoke(orgKill, this);
	}

	void Kill_SilentPatch()
	{
		// Fix ScreamSoundBank overlapped I/O event handle leak
		if (overlapped.hEvent != nullptr)
		{
			CloseHandle(overlapped.hEvent);
			overlapped.hEvent = nullptr;
		}

		Kill();
	}
};

static_assert(sizeof(ScreamSoundBank) == 0x140, "Wrong size: ScreamSoundBank");

// ============= Fix memory leaks in Scream sound setups =============
namespace SEALeaksFix
{
	LPVOID RedirectMallocToCoTaskMemAlloc(SIZE_T size)
	{
		return CoTaskMemAlloc(size);
	}

	void RedirectFreeToCoTaskMemFree(LPVOID ptr)
	{
		CoTaskMemFree(ptr);
	}

	static inline DWORD(*MDXactCreateSoundBankCommand)(DWORD, DWORD, DWORD, DWORD);
	DWORD MDXactCreateSoundBankWithManagedDataCommand(DWORD buffer, DWORD size, DWORD /*flags*/, DWORD alloc_attributes)
	{
		return MDXactCreateSoundBankCommand(buffer, size, 1 /*XACT_FLAG_ENGINE_CREATE_MANAGEDATA*/, alloc_attributes);
	}

	// When failing use CoTaskMemFree if it's managed
	void MDXact_Manager_Internal_CreateInMemoryWaveBank(int id, LPVOID buffer, DWORD size, DWORD flags, DWORD alloc_attributes)
	{
		DWORD pXactEngine = *(DWORD*)0xCFACE0;
		if (pXactEngine == NULL)
		{
			return;
		}

		DWORD pfnCreateInMemoryWaveBank = *(DWORD*)(*(DWORD*)pXactEngine + 0x28);
		DWORD pXactWaveBank = NULL;
		if (SUCCEEDED((*(HRESULT(__stdcall*)(DWORD, LPVOID, DWORD, DWORD, DWORD, PDWORD))pfnCreateInMemoryWaveBank)(
			pXactEngine, buffer, size, flags, alloc_attributes, &pXactWaveBank)))
		{
			(*(void(__cdecl*)(DWORD))0x5AD810)(pXactWaveBank); // register ref counted wave bank
			(*(void(__cdecl*)(int))0x5AD4D0)(id);
			DWORD pMDXactWaveBank = (*(DWORD(__cdecl*)(DWORD))0x5EE830)(12); // MemoryMgrMalloc
			if (pMDXactWaveBank)
			{
				// MDXact_WaveBank ctor
				(*(DWORD(__thiscall*)(DWORD, DWORD, bool, int))0x5B0660)(pMDXactWaveBank, pXactWaveBank, false, id);
			}
			// NiTMap<int,MDXact_WaveBank *>::SetAt
			(*(DWORD(__thiscall*)(DWORD, int, DWORD))0x8A5140)(*(DWORD*)0xCFACE4, id, pMDXactWaveBank);
		}
		else
		{
			if (buffer != NULL)
			{
				if (flags & 0x00000001) // XACT_FLAG_ENGINE_CREATE_MANAGEDATA
					CoTaskMemFree(buffer);
				else
					(*(void(__cdecl*)(LPVOID))0x5EE940)(buffer); // MemoryMgrFree
			}

			(*(void(__cdecl*)(int))0x5AD5F0)(id); // remove shared data of wave bank
		}
	}

	// Do not ignore XACT flags, when failing use CoTaskMemFree if it's managed
	void MDXact_Manager_Internal_CreateSoundBank(int id, LPVOID buffer, DWORD size, DWORD flags, DWORD alloc_attributes)
	{
		DWORD pXactEngine = *(DWORD*)0xCFACE0;
		if (pXactEngine == NULL)
		{
			return;
		}

		DWORD pfnCreateSoundBank = *(DWORD*)(*(DWORD*)pXactEngine + 0x24);
		DWORD pXactSoundBank = NULL;
		if (SUCCEEDED((*(HRESULT(__stdcall*)(DWORD, LPVOID, DWORD, DWORD, DWORD, PDWORD))pfnCreateSoundBank)(
			pXactEngine, buffer, size, flags, alloc_attributes, &pXactSoundBank)))
		{
			(*(void(__cdecl*)(DWORD))0x5AD8E0)(pXactSoundBank); // register ref counted sound bank
			(*(void(__cdecl*)(int))0x5AD530)(id);
			DWORD pMDXactSoundBank = (*(DWORD(__cdecl*)(DWORD))0x5EE830)(24); // MemoryMgrMalloc
			if (pMDXactSoundBank)
			{
				// MDXact_SoundBank ctor
				(*(DWORD(__thiscall*)(DWORD, DWORD, int))0x5B04E0)(pMDXactSoundBank, pXactSoundBank, id);
			}
			// NiTMap<int,MDXact_SoundBank *>::SetAt
			(*(DWORD(__thiscall*)(DWORD, int, DWORD))0x8A5140)(*(DWORD*)0xCFACE8, id, pMDXactSoundBank);
		}
		else
		{
			if (buffer != NULL)
			{
				if (flags & 0x00000001) // XACT_FLAG_ENGINE_CREATE_MANAGEDATA
					CoTaskMemFree(buffer);
				else
					(*(void(__cdecl*)(LPVOID))0x5EE940)(buffer); // MemoryMgrFree
			}

			(*(void(__cdecl*)(int))0x5AD660)(id); // remove shared data of sound bank
		}
	}
};


// RegSetKeyValueW only exists from Windows Vista onwards, this equivalent relies on APIs present since Windows 2000
static LSTATUS RegSetKeyValueWLegacy(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName, DWORD dwType, LPCVOID lpData, DWORD cbData)
{
	HKEY hTargetKey = hKey;
	const bool hasSubKey = lpSubKey != nullptr && *lpSubKey != 0;
	if (hasSubKey)
	{
		const LSTATUS status = RegCreateKeyExW(hKey, lpSubKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hTargetKey, nullptr);
		if (status != ERROR_SUCCESS)
		{
			return status;
		}
	}

	const LSTATUS status = RegSetValueExW(hTargetKey, lpValueName, 0, dwType, static_cast<const BYTE*>(lpData), cbData);

	if (hasSubKey)
	{
		RegCloseKey(hTargetKey);
	}
	return status;
}

// ============= Automatic game settings from the INI =============
namespace GameSettings
{
	// Where the game keeps its own settings, and where SilentPatch remembers what it last wrote there
	static const wchar_t* const GAME_KEY_PATH = L"Software\\Rockstar Games\\Bully Scholarship Edition\\A0";
	static const wchar_t* const LAST_APPLIED_KEY_PATH = L"Software\\SilentPatch\\Bully Scholarship Edition\\LastApplied";

	struct Setting
	{
		const wchar_t* iniKey;
		const wchar_t* registryValue;
	};

	static const Setting SETTINGS[] =
	{
		{ L"AntiAliasing", L"AA" },
		{ L"ResolutionWidth", L"RESW" },
		{ L"ResolutionHeight", L"RESH" },
		{ L"ShadowLevel", L"SHA" },
		{ L"Vsync", L"VS" },
		{ L"Windowed", L"WIN" },
	};

	static bool ReadDword(LPCWSTR keyPath, LPCWSTR valueName, DWORD& value)
	{
		DWORD size = sizeof(value);
		return RegGetValueW(HKEY_CURRENT_USER, keyPath, valueName, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
	}

	// Writes each setting present in the INI to the game's registry settings, but only when the INI value changed
	// since SilentPatch last applied it - so changes made later in the game's own options menu are left alone.
	// AlwaysApplySettings=1 restores the old behaviour of writing them on every launch.
	static void Apply(const wchar_t* iniPath)
	{
		const bool alwaysApply = GetPrivateProfileIntW(L"SilentPatch", L"AlwaysApplySettings", 0, iniPath) != 0;

		for (const Setting& setting : SETTINGS)
		{
			const int iniValue = GetPrivateProfileIntW(L"SilentPatch", setting.iniKey, -1, iniPath);
			if (iniValue == -1)
			{
				continue;
			}
			const DWORD value = static_cast<DWORD>(iniValue);

			DWORD lastApplied = 0;
			if (!alwaysApply && ReadDword(LAST_APPLIED_KEY_PATH, setting.registryValue, lastApplied) && lastApplied == value)
			{
				Log::Write("%ls=%d unchanged since last applied, leaving the game's %ls setting alone", setting.iniKey, iniValue, setting.registryValue);
				continue;
			}

			const LSTATUS status = RegSetKeyValueWLegacy(HKEY_CURRENT_USER, GAME_KEY_PATH, setting.registryValue, REG_DWORD, &value, sizeof(value));
			if (status == ERROR_SUCCESS)
			{
				RegSetKeyValueWLegacy(HKEY_CURRENT_USER, LAST_APPLIED_KEY_PATH, setting.registryValue, REG_DWORD, &value, sizeof(value));
			}
			Log::Write("%ls=%d written to registry value %ls (status %ld)", setting.iniKey, iniValue, setting.registryValue, static_cast<long>(status));
		}
	}
}

// ============= In-game FPS cap toggle (SELECT + DPAD DOWN on a controller, F11 on the keyboard by default) =============
namespace FPSToggle
{
	static constexpr size_t MAX_CYCLE_VALUES = 8;
	static int32_t cycleValues[MAX_CYCLE_VALUES] = { 30, 60 };
	static size_t numCycleValues = 2;
	static int toggleKey = VK_F11;       // Virtual-key code, 0 disables the keyboard toggle
	static bool controllerToggle = true; // false disables the controller combo

	using XInputGetStateFn = DWORD (WINAPI*)(DWORD dwUserIndex, XINPUT_STATE* pState);
	static XInputGetStateFn pXInputGetState = nullptr;

	// XInput is resolved at runtime so the plugin does not hard-depend on one specific XInput DLL,
	// which would prevent the whole ASI from loading when that DLL is absent.
	// The game itself links against xinput1_3.dll, so that one is tried first.
	static void LoadXInput()
	{
		static const wchar_t* const XINPUT_DLLS[] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
		for (const wchar_t* dllName : XINPUT_DLLS)
		{
			HMODULE hXInput = LoadLibraryW(dllName);
			if (hXInput == nullptr)
			{
				continue;
			}
			pXInputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(hXInput, "XInputGetState"));
			if (pXInputGetState != nullptr)
			{
				Log::Write("XInput loaded from %ls", dllName);
				return;
			}
			FreeLibrary(hXInput);
		}
		Log::Write("No XInput DLL found, controller toggle unavailable");
	}

	// Reads FPSToggleValues, FPSToggleKey and FPSToggleController
	static void ReadSettings(const wchar_t* iniPath)
	{
		wchar_t buffer[128];

		// Comma separated list of caps to cycle through, 0 = uncapped
		GetPrivateProfileStringW(L"SilentPatch", L"FPSToggleValues", L"30,60", buffer, _countof(buffer), iniPath);
		numCycleValues = 0;
		const wchar_t* cursor = buffer;
		while (*cursor != 0 && numCycleValues < MAX_CYCLE_VALUES)
		{
			wchar_t* end = nullptr;
			const long value = wcstol(cursor, &end, 10);
			if (end == cursor)
			{
				break; // Not a number
			}
			if (value >= 0)
			{
				cycleValues[numCycleValues++] = static_cast<int32_t>(value);
			}
			cursor = end;
			while (*cursor == L',' || *cursor == L' ')
			{
				cursor++;
			}
		}
		if (numCycleValues == 0)
		{
			cycleValues[0] = 30;
			cycleValues[1] = 60;
			numCycleValues = 2;
		}

		// Virtual-key code, decimal or 0x hex
		GetPrivateProfileStringW(L"SilentPatch", L"FPSToggleKey", L"0x7A", buffer, _countof(buffer), iniPath);
		wchar_t* end = nullptr;
		const long key = wcstol(buffer, &end, 0);
		toggleKey = (end == buffer || key < 0 || key > 0xFF) ? VK_F11 : static_cast<int>(key);

		controllerToggle = GetPrivateProfileIntW(L"SilentPatch", L"FPSToggleController", 1, iniPath) != 0;
	}

	static bool IsEnabled()
	{
		return toggleKey != 0 || controllerToggle;
	}

	static bool IsGameInForeground()
	{
		HWND hForeground = GetForegroundWindow();
		if (hForeground == nullptr)
		{
			return false;
		}
		DWORD processId = 0;
		GetWindowThreadProcessId(hForeground, &processId);
		return processId == GetCurrentProcessId();
	}

	static bool IsControllerComboHeld()
	{
		if (!controllerToggle || pXInputGetState == nullptr)
		{
			return false;
		}
		XINPUT_STATE state = {};
		if (pXInputGetState(0, &state) != ERROR_SUCCESS)
		{
			return false;
		}
		constexpr WORD COMBO = XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_DPAD_DOWN;
		return (state.Gamepad.wButtons & COMBO) == COMBO;
	}

	static bool IsKeyboardToggleHeld()
	{
		return toggleKey != 0 && (GetAsyncKeyState(toggleKey) & 0x8000) != 0;
	}

	// The cap operand stores INT_MAX for "uncapped", the INI and the cycle list use 0 for it
	static int32_t CapToOperand(int32_t cap)
	{
		return cap > 0 ? cap : INT_MAX;
	}

	static int32_t OperandToCap(int32_t operand)
	{
		return operand == INT_MAX ? 0 : operand;
	}

	static void CycleFPSCap(const wchar_t* iniPath)
	{
		const int32_t currentCap = OperandToCap(*FPS_CAP_OPERAND);

		// Move to the value after the current one, or to the first one if the current cap is not in the list
		size_t nextIndex = 0;
		for (size_t i = 0; i < numCycleValues; i++)
		{
			if (cycleValues[i] == currentCap)
			{
				nextIndex = (i + 1) % numCycleValues;
				break;
			}
		}
		const int32_t newCap = cycleValues[nextIndex];
		if (newCap == currentCap)
		{
			return;
		}

		// Apply first - persisting to the INI can legitimately fail (e.g. read-only game directory)
		// and that must not stop the in-game toggle from working
		Memory::VP::Patch<int32_t>(FPS_CAP_OPERAND, CapToOperand(newCap));

		wchar_t value[16];
		swprintf_s(value, L"%d", newCap);
		const BOOL persisted = WritePrivateProfileStringW(L"SilentPatch", L"FPSLimit", value, iniPath);
		Log::Write("FPS cap changed from %d to %d (0 = uncapped), INI %s", currentCap, newCap, persisted != FALSE ? "updated" : "not writable");
	}

	DWORD WINAPI InputThread(LPVOID /*lpParam*/)
	{
		LoadXInput();

		bool toggleWasHeld = false;
		while (WaitForSingleObject(hShutdownEvent, 50) == WAIT_TIMEOUT)
		{
			// Track the held state regardless of focus so a press started elsewhere does not fire on alt-tab,
			// but only act on it while the game is the foreground window
			const bool toggleHeld = IsControllerComboHeld() || IsKeyboardToggleHeld();
			if (toggleHeld && !toggleWasHeld && IsGameInForeground())
			{
				CycleFPSCap(g_iniPath);
			}
			toggleWasHeld = toggleHeld;
		}
		return 0;
	}
}

// ============= ControllerSettings file handling for the EnableControllers INI option =============
namespace ControllerSettings
{
	static constexpr DWORD SETTINGS_FILE_SIZE = sizeof(EnabledControllerSettings);
	static constexpr size_t CONTROLLERS_ENABLED_OFFSET = 0xA8;

	static_assert(sizeof(DisabledControllerSettings) == SETTINGS_FILE_SIZE, "Controller settings templates must match in size");

	static bool GetSettingsPath(wchar_t (&path)[MAX_PATH])
	{
		// The game keeps this file under the Documents folder of the current user. Ask the shell for it so that
		// folder redirection (e.g. OneDrive) is respected, unlike a hardcoded %USERPROFILE%\Documents
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, path)))
		{
			return false;
		}
		if (!PathAppendW(path, L"Bully Scholarship Edition"))
		{
			return false;
		}
		CreateDirectoryW(path, nullptr); // Fails harmlessly if it already exists
		return PathAppendW(path, L"ControllerSettings") != FALSE;
	}

	static bool ReadSettings(const wchar_t* path, unsigned char (&buffer)[SETTINGS_FILE_SIZE])
	{
		HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		DWORD bytesRead = 0;
		const bool result = GetFileSize(hFile, nullptr) == SETTINGS_FILE_SIZE
			&& ReadFile(hFile, buffer, SETTINGS_FILE_SIZE, &bytesRead, nullptr) != FALSE
			&& bytesRead == SETTINGS_FILE_SIZE;
		CloseHandle(hFile);
		return result;
	}

	static bool WriteSettings(const wchar_t* path, const unsigned char (&buffer)[SETTINGS_FILE_SIZE])
	{
		HANDLE hFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		DWORD bytesWritten = 0;
		const bool result = WriteFile(hFile, buffer, SETTINGS_FILE_SIZE, &bytesWritten, nullptr) != FALSE
			&& bytesWritten == SETTINGS_FILE_SIZE;
		CloseHandle(hFile);
		return result;
	}

	static void Apply(bool enableControllers)
	{
		wchar_t path[MAX_PATH];
		if (!GetSettingsPath(path))
		{
			Log::Write("EnableControllers: could not resolve the ControllerSettings path");
			return;
		}

		unsigned char settings[SETTINGS_FILE_SIZE];
		const bool hasValidFile = ReadSettings(path, settings);
		if (!hasValidFile)
		{
			// Missing or malformed file - start from the matching defaults
			memcpy(settings, enableControllers ? EnabledControllerSettings : DisabledControllerSettings, SETTINGS_FILE_SIZE);
		}

		const unsigned char value = static_cast<unsigned char>(enableControllers ? 1 : 0);
		if (hasValidFile && settings[CONTROLLERS_ENABLED_OFFSET] == value)
		{
			Log::Write("EnableControllers=%d already set in %ls", value, path);
			return; // Already up to date, leave the file untouched
		}
		settings[CONTROLLERS_ENABLED_OFFSET] = value;
		const bool written = WriteSettings(path, settings);
		Log::Write("EnableControllers=%d %s to %ls%s", value, written ? "written" : "could not be written", path, hasValidFile ? "" : " (file was missing or malformed, defaults used)");
	}
}


void InjectHooks()
{
	using namespace Memory;

	// Obtain a path to the INI next to the ASI
	GetModuleFileNameW(hDLLModule, g_iniPath, _countof(g_iniPath) - 3); // Minus max required space for extension
	PathRenameExtensionW(g_iniPath, L".ini");

	Log::Init(g_iniPath, GetPrivateProfileIntW(L"SilentPatch", L"LogFile", 0, g_iniPath) != 0);
	Log::Write("SilentPatch for Bully build %d.%d, INI: %ls", SILENTPATCH_REVISION_ID, SILENTPATCH_BUILD_ID, g_iniPath);

	// If it's not 1.200, bail out
	if (!MemEquals(0x860C6B, { 0xC7, 0x45, 0xFC, 0xFE, 0xFF, 0xFF, 0xFF }))
	{
		Log::Write("Unsupported executable (not 1.200), no fixes applied");
#ifndef _DEBUG
		MessageBoxW(nullptr, L"You are using an executable version not supported by SilentPatch (most likely 1.154)!\n\n"
			L"I strongly recommend obtaining a 1.200 executable - if you are using a retail version, just download an official 1.200 patch; "
			L"if you are using a Steam version, verify your game's files (since by default Steam uses 1.200).",
			L"SilentPatch", MB_OK | MB_ICONWARNING);
#endif
		return;
	}
	Log::Write("Executable 1.200 detected, applying fixes");

	std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule(GetModuleHandle(nullptr), ".text");

	// Replaced custom CMemoryHeap with regular CRT functions (like in GTA)
	if (const int INIoption = GetPrivateProfileIntW(L"SilentPatch", L"CustomMemoryMgr", 0, g_iniPath); INIoption != 0)
	{
		using namespace FixedAllocators;
		Log::Write("CustomMemoryMgr enabled");

		InjectHook(0x5EE630, InitMemoryMgr, PATCH_JUMP);
		InjectHook(0x5EE5A0, ShutDownMemoryMgr, PATCH_JUMP);
		InjectHook(0x5EE830, MemoryMgrMalloc, PATCH_JUMP);
		InjectHook(0x5EE940, MemoryMgrFree, PATCH_JUMP);
		InjectHook(0x5EE9C0, RwMallocAlign, PATCH_JUMP);
		// 0x5EEA50 - RwMemoryMgrMalloc - jumps to MemoryMgrMalloc
		// 0x5EEA60 - RwMemoryMgrFree - jumps to MemoryMgrFree
		InjectHook(0x5EEA70, RwFreeAlign, PATCH_JUMP);

		InjectHook(0x5EEEF0, MemoryHeap_Free, PATCH_JUMP);
		InjectHook(0x5EF4D0, MemoryHeap_MoveMemoryBully, PATCH_JUMP);

		InjectHook(0x5EEDD0, MemoryHeap_GetMemoryUsed, PATCH_JUMP);

		// Write a pointer to fake 'upper memory bound' so CStreaming::MakeSpaceFor is pleased
		static const uintptr_t FAKE_MAX_MEMORY = 0x7FFFFFFF;
		Patch(0xD141A8, &FAKE_MAX_MEMORY);
	}

	// Fixed CPedType::Shutdown (zero pointers to prevent a double free)
	{
		using namespace PedTypeShutdown;

		Patch<uint8_t>(0x499CD8, 0x56);
		ReadCall(0x499CD9, orgOperatorDelete);
		InjectHook(0x499CD9, OperatorDelete_Safe);
	}

	// Don't call cSCREAMAudioManager::CleanupAfterMission from cSCREAMAudioManager::Terminate (used already freed memory)
	Nop(0x5963C3, 5);


	// Fixed a crash in CFileLoader::LoadCollisionModel occuring with a replaced allocator
	// Fixed COLL vertex loading
	Patch<uint8_t>(0x42BE80 + 2, 16);

	{
		using namespace FrameTimingFix;

		// The limiter loop calls Sleep(1) between timer checks. Either replace that call with the hybrid sleep/spin wait
		// (FrameLimiterSleep=1), or remove it so the loop spins (default, most precise but keeps a CPU core busy)
		bool hybridWait = false;
		if (GetPrivateProfileIntW(L"SilentPatch", L"FrameLimiterSleep", 0, g_iniPath) != 0)
		{
			// push imm8 / call dword ptr [Sleep] - verify before touching it
			if (MemEquals(0x4061C4, { 0x6A }) && MemEquals(0x4061C6, { 0xFF, 0x15 }) && InitHybridWait())
			{
				InjectHook(0x4061C4, HybridWait, PATCH_CALL);
				Nop(0x4061C9, 3);
				hybridWait = true;
			}
			else
			{
				Log::Write("FrameLimiterSleep requested but the limiter code did not match or the timer could not be created, spinning instead");
			}
		}
		if (!hybridWait)
		{
			// DO NOT sleep when limiting FPS in game...
			Nop(0x4061C4, 2 + 6);
		}
		Log::Write("Frame limiter wait: %s", hybridWait ? "hybrid sleep/spin" : "spin");

		// ...sleep for 100ms periodically when minimized
		ReadCall(0x43D660, orgUpdateTimer);
		InjectHook(0x43D660, UpdateTimerAndSleep);

		// Because we're doing a busy loop now, 31FPS cap can now become a 30FPS cap.
		// Defaults to 30 when the option is missing so the in-game toggle always has a known starting point.
		const int fpsLimit = GetPrivateProfileIntW(L"SilentPatch", L"FPSLimit", 30, g_iniPath);
		Patch<int32_t>(FPS_CAP_OPERAND, fpsLimit > 0 ? fpsLimit : INT_MAX);
		Log::Write("FPSLimit=%d (0 = uncapped)", fpsLimit);

		// Revert code changes 60FPS EXE does, we don't need them anymore
		Patch<int8_t>(0x4061BE + 1, 0x4);
		Patch<uint8_t>(0x4061C2, 0x73);
	}

	// Game settings from the INI, written to the registry only when they changed since they were last applied
	GameSettings::Apply(g_iniPath);

	if (const int INIoption = GetPrivateProfileIntW(L"SilentPatch", L"EnableControllers", -1, g_iniPath); INIoption == 0 || INIoption == 1)
	{
		ControllerSettings::Apply(INIoption == 1);
	}

	// Remove FILE_FLAG_NO_BUFFERING from CdStreams
	Patch<uint32_t>(0x73ABEA + 6, FILE_FLAG_OVERLAPPED);


	// Fixed crash in Nutcracking
	// Consistently treat playercount as ID, not actual size
	Nop(0x6FB302, 6);
	Nop(0x6FB3EB, 6);
	Nop(0x6FC920, 2);
	Nop(0x6FC945, 2);
	Nop(0x6FC94F, 2);
	Nop(0x6FC97C, 2);
	Nop(0x6FCE91, 2);


	// Fixes for CBasePool misuse (object/projectile pools)
	{
		// Poor man's jitasm ;)
		auto pushEax = [](uintptr_t& addr)
			{
				Patch<uint8_t>(addr, 0x50);
				addr += 1;
			};

		auto pushEbx = [](uintptr_t& addr)
			{
				Patch<uint8_t>(addr, 0x53);
				addr += 1;
			};

		auto pushEsi = [](uintptr_t& addr)
			{
				Patch<uint8_t>(addr, 0x56);
				addr += 1;
			};

		auto pushEdi = [](uintptr_t& addr)
			{
				Patch<uint8_t>(addr, 0x57);
				addr += 1;
			};

		auto movEcxEbx = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xCB });
				addr += 2;
			};

		auto movEcxEbp = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xCD });
				addr += 2;
			};

		auto movEcxEsi = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xCE });
				addr += 2;
			};

		auto movEcxEdi = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xCF });
				addr += 2;
			};

		auto movEsiEax = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xF0 });
				addr += 2;
			};

		auto movEdiEax = [](uintptr_t& addr)
			{
				Patch(addr, { 0x8B, 0xF8 });
				addr += 2;
			};

		auto call = [](uintptr_t& addr, auto dest)
			{
				InjectHook(addr, dest, PATCH_CALL);
				addr += 5;
			};

		auto jmp = [](uintptr_t& addr, uintptr_t dest)
			{
				const ptrdiff_t offset = dest - (addr + 2);
				if (offset >= INT8_MIN && offset <= INT8_MAX)
				{
					Patch(addr, { 0xEB, static_cast<uint8_t>(offset) });
					addr += 2;
				}
				else
				{
					InjectHook(addr, dest, PATCH_JUMP);
					addr += 5;
				}
			};

		InjectHook(0x435BD0, &CBasePool::GetSlotWithLinked, PATCH_JUMP);
		InjectHook(0x436B90, &CBasePool::GetSlotWithLinkedWrapper, PATCH_JUMP);

		uintptr_t address;

		address = 0x4378D0;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x437901);

		address = 0x437A18;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x437A59);

		address = 0x437AA4;
		pushEsi(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x437AD3);

		address = 0x44A4F6;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x44A527);

		address = 0x450A99;
		pushEsi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x450AC5);

		address = 0x45E72B;
		pushEsi(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x45E757);

		address = 0x49E0D4;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x49E109);

		address = 0x49E489;
		pushEbx(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x49E4B8);

		address = 0x4CFE11;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4CFE43);

		address = 0x4CFE91;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4CFEC3);

		address = 0x4CFF24;
		pushEbx(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4CFF59);

		address = 0x4D0138;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4D0169);

		address = 0x4D01A0;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4D01D1);

		address = 0x4D0211;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4D0243);

		address = 0x4D02B0;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x4D02E2);

		address = 0x4D0350;
		pushEsi(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x4D037F);

		address = 0x5309A6;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x5309D5);

		address = 0x532446;
		pushEsi(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x532472);

		address = 0x5332DA;
		pushEbx(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x53330C);

		address = 0x5BCE62;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x5BCE97);

		address = 0x5C2642;
		pushEbx(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEdiEax(address);
		jmp(address, 0x5C2679);

		address = 0x5C2770;
		pushEbx(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEdiEax(address);
		jmp(address, 0x5C27A7);

		address = 0x5C2FC6;
		pushEdi(address);
		movEcxEsi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x5C2FF3);

		address = 0x5C31F0;
		pushEdi(address);
		movEcxEsi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x5C3220);

		address = 0x5C3280;
		pushEsi(address);
		movEcxEdi(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x5C32AF);

		address = 0x5C33D6;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x5C3407);

		address = 0x5C4D91;
		pushEdi(address);
		movEcxEbp(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x5C4DC3);

		address = 0x677206;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		jmp(address, 0x67722E);

		address = 0x67C1AD;
		pushEdi(address);
		movEcxEbx(address);
		call(address, &CBasePool::GetSlotWithLinkedWrapper);
		movEsiEax(address);
		jmp(address, 0x67C1F2);
	}

	// Don't fail if call to CoInitializeEx didn't return S_TRUE (maybe something else called CoInitializeEx on us already)
	Nop(0x5AE2B4, 6);


	// Version number with SP build in main menu
#if COMPILE_TEST_VERSION
	Patch<const char*>(0x6A69EA + 1, "SP Test " __DATE__ " " __TIME__);
#else
	Patch<const char*>(0x6A69EA + 1, "%1.3f SP Build " STRINGIZE(SILENTPATCH_REVISION_ID));
#endif


	// Fix heap corruptions on exit
	{
		using namespace DoubleFreeOnExitFix;

		InjectHook(0x519090, ReleaseStruct, PATCH_JUMP);

		Patch<uint8_t>(0x55E0E2, 0x56); // push esi
		InjectHook(0x55E0E3, ReleaseTextureAndNull);

		// Series of identical calls to patch...
		auto fixEax = [](uintptr_t& addr, uintptr_t extra = 0)
			{
				Patch(addr, { 0xB8 });
				addr += 5 + 1 + extra;
				InjectHook(addr, DoubleFreeOnExitFix::ReleaseTextureAndNull);
				addr += 5;
			};

		auto fixEcx = [](uintptr_t& addr, uintptr_t extra = 0)
			{
				Patch(addr, { 0x90, 0xB9 });
				addr += 6 + 1 + extra;
				InjectHook(addr, DoubleFreeOnExitFix::ReleaseTextureAndNull);
				addr += 5;
			};

		auto fixEdx = [](uintptr_t& addr, uintptr_t extra = 0)
			{
				Patch(addr, { 0x90, 0xBA });
				addr += 6 + 1 + extra;
				InjectHook(addr, DoubleFreeOnExitFix::ReleaseTextureAndNull);
				addr += 5;
			};

		uintptr_t address = 0x528622;
		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		// add esp in code...
		fixEdx(address);
		fixEax(address, 3);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);
		fixEcx(address);

		fixEdx(address);
		fixEax(address);


		// Missing reference adding...
		ReadCall(0x514D98, orgPopTimer);
		InjectHook(0x514D98, PopTimer_AddMissingReferences);

		// TEMPORARY DEBUG
		//InjectHook( 0x5F0F90, ReleaseTextureDebug, PATCH_JUMP );
	}

	// Don't call RwFrameDestroy from CameraDestroy (crashes for some reason)
	Patch<uint8_t>(0x5EF954, 0xEB);


	// Fix use-after-free when sndStream gets released before overlapped read finishes
	{
		ReadCall(0x5A6EA9, sndStream::orgCleanupStreamInfo);
		InjectHook(0x5A6EA9, &sndStream::CleanupStreamInfo_SilentPatch);
		InjectHook(0x5A71D0, &sndStream::CleanupStreamInfo_SilentPatch);
		InjectHook(0x5A72D2, &sndStream::CleanupStreamInfo_SilentPatch);
		InjectHook(0x5A753F, &sndStream::CleanupStreamInfo_SilentPatch);
		InjectHook(0x5A79BF, &sndStream::CleanupStreamInfo_SilentPatch);
		InjectHook(0x5A7B21, &sndStream::CleanupStreamInfo_SilentPatch);
	}

	// Fix handle leaks in ScreamSoundBank
	{
		ReadCall(0x5A98D5, ScreamSoundBank::orgKill);
		InjectHook(0x5A98D5, &ScreamSoundBank::Kill_SilentPatch);
		InjectHook(0x5AA13F, &ScreamSoundBank::Kill_SilentPatch);
		InjectHook(0x5AA462, &ScreamSoundBank::Kill_SilentPatch);
	}


	// Fix memory leaks in Scream sound setups by allowing XACT to take ownership of allocations,
	// as it seemingly was intended to
	{
		using namespace SEALeaksFix;

		// Do not ignore XACT flags, when failing use CoTaskMemFree if it's managed
		InjectHook(0x5AE690, MDXact_Manager_Internal_CreateInMemoryWaveBank, PATCH_JUMP);
		InjectHook(0x5AE590, MDXact_Manager_Internal_CreateSoundBank, PATCH_JUMP);

		// In ScreamSoundBank::SetupSEA use CoTaskMemAlloc & CoTaskMemFree for XACT buffer
		InjectHook(0x5A990D, RedirectMallocToCoTaskMemAlloc);
		InjectHook(0x5A99FB, RedirectMallocToCoTaskMemAlloc);
		InjectHook(0x5A9B32, RedirectFreeToCoTaskMemFree);

		// Let XACT manage memory of SEA wave and sound banks
		Patch<int8_t>(0x5A99AF, 1);
		ReadCall(0x5A9A1F, MDXactCreateSoundBankCommand);
		InjectHook(0x5A9A1F, MDXactCreateSoundBankWithManagedDataCommand);
	}

	// In-game FPS cap toggle
	FPSToggle::ReadSettings(g_iniPath);
	if (FPSToggle::IsEnabled())
	{
		HANDLE hThread = CreateThread(nullptr, 0, FPSToggle::InputThread, nullptr, 0, nullptr);
		if (hThread != nullptr)
		{
			CloseHandle(hThread);
		}
		Log::Write("FPS toggle enabled: key 0x%02X (0 = disabled), controller combo %s", FPSToggle::toggleKey, FPSToggle::controllerToggle ? "on" : "off");
	}
	else
	{
		Log::Write("FPS toggle disabled");
	}

	Log::Write("All fixes applied");
}

static void ProcHook()
{
	static bool		bPatched = false;
	if (!bPatched)
	{
		bPatched = true;

		InjectHooks();
	}
}

static uint8_t orgCode[5];
static decltype(SystemParametersInfoA)* pOrgSystemParametersInfoA;
BOOL WINAPI SystemParametersInfoA_Hook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
	ProcHook();
	return pOrgSystemParametersInfoA(uiAction, uiParam, pvParam, fWinIni);
}

BOOL WINAPI SystemParametersInfoA_OverwritingHook(UINT uiAction, UINT uiParam, PVOID pvParam, UINT fWinIni)
{
	Memory::VP::Patch(pOrgSystemParametersInfoA, { orgCode[0], orgCode[1], orgCode[2], orgCode[3], orgCode[4] });
	return SystemParametersInfoA_Hook(uiAction, uiParam, pvParam, fWinIni);
}

static bool PatchIAT()
{
	HINSTANCE					hInstance = GetModuleHandle(nullptr);
	PIMAGE_NT_HEADERS			ntHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)hInstance + ((PIMAGE_DOS_HEADER)hInstance)->e_lfanew);

	// Find IAT	
	PIMAGE_IMPORT_DESCRIPTOR	pImports = (PIMAGE_IMPORT_DESCRIPTOR)((DWORD_PTR)hInstance + ntHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

	// Find user32.dll
	for (; pImports->Name != 0; pImports++)
	{
		if (!_stricmp((const char*)((DWORD_PTR)hInstance + pImports->Name), "USER32.DLL"))
		{
			if (pImports->OriginalFirstThunk != 0)
			{
				PIMAGE_IMPORT_BY_NAME* pFunctions = (PIMAGE_IMPORT_BY_NAME*)((DWORD_PTR)hInstance + pImports->OriginalFirstThunk);

				// user32.dll found, find SystemParametersInfoA
				for (ptrdiff_t j = 0; pFunctions[j] != nullptr; j++)
				{
					if (!strcmp((const char*)((DWORD_PTR)hInstance + pFunctions[j]->Name), "SystemParametersInfoA"))
					{
						// Overwrite the address with the address to a custom SystemParametersInfoA
						DWORD			dwProtect[2];
						DWORD_PTR* pAddress = &((DWORD_PTR*)((DWORD_PTR)hInstance + pImports->FirstThunk))[j];

						VirtualProtect(pAddress, sizeof(DWORD_PTR), PAGE_EXECUTE_READWRITE, &dwProtect[0]);
						pOrgSystemParametersInfoA = **(decltype(pOrgSystemParametersInfoA)*)pAddress;
						*pAddress = (DWORD_PTR)SystemParametersInfoA_Hook;
						VirtualProtect(pAddress, sizeof(DWORD_PTR), dwProtect[0], &dwProtect[1]);

						return true;
					}
				}
			}
		}
	}
	return false;
}

static bool PatchIAT_ByPointers()
{
	pOrgSystemParametersInfoA = SystemParametersInfoA;
	memcpy(orgCode, pOrgSystemParametersInfoA, sizeof(orgCode));
	Memory::VP::InjectHook(pOrgSystemParametersInfoA, SystemParametersInfoA_OverwritingHook, PATCH_JUMP);
	return true;
}

static void InstallHooks()
{
	bool getStartupInfoHooked = PatchIAT();
	if (!getStartupInfoHooked)
	{
		PatchIAT_ByPointers();
	}
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	UNREFERENCED_PARAMETER(lpvReserved);

	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		if (hShutdownEvent == NULL)
		{
			hShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		}
		hDLLModule = hinstDLL;
	}
	if (fdwReason == DLL_PROCESS_DETACH)
	{
		SetEvent(hShutdownEvent); // Signal the event
	}
	return TRUE;
}

extern "C" __declspec(dllexport)
uint32_t GetBuildNumber()
{
	return (SILENTPATCH_REVISION_ID << 8) | SILENTPATCH_BUILD_ID;
}

extern "C"
{
	static LONG InitCount = 0;
	__declspec(dllexport) void InitializeASI()
	{
		if (_InterlockedCompareExchange(&InitCount, 1, 0) != 0) return;
		InstallHooks();
	}
}
