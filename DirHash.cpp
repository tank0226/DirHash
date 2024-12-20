/*
* An implementation of directory hashing that uses lexicographical order on name
* for sorting. Based on OpenSSL and Microsoft CNG for hash algorithms for maximum
* performance and compatibility.
*
* Copyright (c) 2010-2024 Mounir IDRASSI <mounir.idrassi@idrix.fr>. All rights reserved.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
* or FITNESS FOR A PARTICULAR PURPOSE.
*
*/

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 
#endif

/* We use UNICODE */
#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef ALG_SID_SHA_256
#define ALG_SID_SHA_256                 12
#endif

#ifndef ALG_SID_SHA_384
#define ALG_SID_SHA_384                 13
#endif

#ifndef ALG_SID_SHA_512
#define ALG_SID_SHA_512                 14
#endif

#ifndef CALG_SHA_256
#define CALG_SHA_256            (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_256)
#endif

#ifndef CALG_SHA_384
#define CALG_SHA_384            (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_384)
#endif

#ifndef CALG_SHA_512
#define CALG_SHA_512            (ALG_CLASS_HASH | ALG_TYPE_ANY | ALG_SID_SHA_512)
#endif

#define _CRT_SECURE_NO_WARNINGS
#pragma warning(disable : 4995)

#include <ntstatus.h>

#define WIN32_NO_STATUS
#include <windows.h>
#include <WinCrypt.h>
#include <bcrypt.h>
#include <Shlwapi.h>
#include <pathcch.h>
#include <stdio.h>
#include <stdarg.h>
#include <tchar.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include <strsafe.h>
#if !defined (_M_ARM64) && !defined (_M_ARM)
#include "BLAKE2/sse/blake2.h"
#else
#include "BLAKE2/neon/blake2.h"
#endif
#include "BLAKE3/blake3.h"
#include <memory>
#include <algorithm>
#include <string>
#include <list>
#include <map>
#include <vector>
#ifdef USE_STREEBOG
#include "Streebog.h"
#endif

#define DIRHASH_VERSION	"1.26.1"


using namespace std;

typedef vector<unsigned char> ByteArray;

class CFilePtr;

static BYTE g_pbBuffer[4096];
static TCHAR g_szCanonalizedName[MAX_PATH + 1];
static WORD  g_wAttributes = FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED;
static volatile WORD  g_wCurrentAttributes;
static HANDLE g_hConsole = NULL;
static CONSOLE_SCREEN_BUFFER_INFO g_originalConsoleInfo;
static vector<shared_ptr<CFilePtr>> outputFiles;
static bool g_bLowerCase = false;
static bool g_bUseMsCrypto = false;
static bool g_bMismatchFound = false;
static bool g_bSkipError = false;
static bool g_bNoLogo = false;
static bool g_bNoFollow = false;
static HANDLE g_hThreads[256];
static WORD ThreadProcessorGroups[256] = { 0 };
static DWORD g_threadsCount = 0;
static volatile bool g_bStopThreads = false;
static volatile bool g_bFatalError = false;
static volatile bool g_bStopOutputThread = false;
static HANDLE g_hReadyEvent = NULL;
static HANDLE g_hStopEvent = NULL;
static HANDLE g_hOutputReadyEvent = NULL;
static HANDLE g_hOutputStopEvent = NULL;
static HANDLE g_hOutputThread = NULL;
static std::wstring g_szLastErrorMsg;
static wstring g_currentDirectory;
static bool g_sumFileSkipped = false;
static bool g_bSumRelativePath = false;
static bool g_bIncludeLastDir = false;
static wstring g_inputDirPath;
static size_t g_inputDirPathLength = 0;
static bool g_bLongPathNamesEnabled = false;
static list<wstring> onlySpecList;
static list<wstring> excludeSpecList;


typedef BOOL(WINAPI* SetThreadGroupAffinityFn)(
	HANDLE               hThread,
	const GROUP_AFFINITY* GroupAffinity,
	PGROUP_AFFINITY      PreviousGroupAffinity
	);

typedef WORD(WINAPI* GetActiveProcessorGroupCountFn)();

typedef DWORD(WINAPI* GetActiveProcessorCountFn)(
	WORD GroupNumber
	);

typedef HRESULT(WINAPI* PathAllocCanonicalizeFn)(
	PCWSTR pszPathIn,
	ULONG  dwFlags,
	PWSTR* ppszPathOut
);

typedef HRESULT(WINAPI *PathCchSkipRootFn)(
	PCWSTR pszPath,
	PCWSTR* ppszRootEnd
);

typedef HRESULT(WINAPI *PathAllocCombineFn)(
	_In_opt_ PCWSTR pszPathIn,
	_In_opt_ PCWSTR pszMore,
	_In_ ULONG dwFlags,
	_Outptr_ PWSTR* ppszPathOut
);

PathAllocCanonicalizeFn PathAllocCanonicalizePtr = NULL;
PathAllocCombineFn PathAllocCombinePtr = NULL;
PathCchSkipRootFn PathCchSkipRootPtr = NULL;

// Used for sorting directory content
bool compare_nocase(LPCWSTR first, LPCWSTR second)
{
	return _wcsicmp(first, second) < 0;
}

TCHAR ToHex(unsigned char b)
{
	if (b >= 0 && b <= 9)
		return _T('0') + b;
	else if (b >= 10 && b <= 15)
		return (g_bLowerCase ? _T('a') : _T('A')) + b - 10;
	else
		return (g_bLowerCase ? _T('x') : _T('X'));
}

void ToHex(LPBYTE pbData, int iLen, LPTSTR szHex)
{
	unsigned char b;
	for (int i = 0; i < iLen; i++)
	{
		b = *pbData++;
		*szHex++ = ToHex(b >> 4);
		*szHex++ = ToHex(b & 0x0F);
	}
	*szHex = 0;
}

void ToHex (const ByteArray& data, LPTSTR szHex)
{
	unsigned char b;
	for (size_t i = 0; i < data.size(); i++)
	{
		b = data[i];
		*szHex++ = ToHex(b >> 4);
		*szHex++ = ToHex(b & 0x0F);
	}
	*szHex = 0;
}

bool FromHex(TCHAR c, unsigned char& b)
{
	if (c >= _T('0') && c <= _T('9'))
		b = c - _T('0');
	else if (c >= _T('a') && c <= _T('f'))
		b = 10 + (c - _T('a'));
	else if (c >= _T('A') && c <= _T('F'))
		b = 10 + (c - _T('A'));
	else
		return false;
	return true;
}

bool FromHex(const TCHAR* szHex, ByteArray& buffer)
{
	bool bRet = false;
	buffer.clear();
	if (szHex)
	{
		size_t l = _tcslen(szHex);
		if (l % 2 == 0)
		{
			size_t i;
			for (i = 0; i < l / 2; i++)
			{
				unsigned char b1, b2;
				if (FromHex(*szHex++, b1) && FromHex(*szHex++, b2))
				{
					buffer.push_back(b1 * 16 + b2);
				}
				else
					break;
			}

			if (i == (l / 2))
			{
				bRet = true;
			}
			else
				buffer.clear();
		}
	}

	return bRet;
}

std::wstring FormatString(LPCTSTR fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	int s = _vscwprintf(fmt, args);
	va_end(args);

	wchar_t* pStr = new wchar_t[s + 1];
	va_start(args, fmt);
	_vsnwprintf(pStr, (size_t) (s + 1), fmt, args);
	va_end(args);

	std::wstring ret = pStr;
	delete[] pStr;
	return ret;
}

void ShowMessage(WORD attributes, LPCTSTR szMsg, va_list args)
{
	SetConsoleTextAttribute(g_hConsole, attributes);
	_vtprintf(szMsg, args);
	SetConsoleTextAttribute(g_hConsole, g_wCurrentAttributes);
}

void ShowMessageDirect(WORD attributes, LPCTSTR szMsg)
{	
	SetConsoleTextAttribute(g_hConsole, attributes);
	_tprintf(L"%s", szMsg);
	SetConsoleTextAttribute(g_hConsole, g_wCurrentAttributes);
}

void ShowError(LPCTSTR szMsg, ...)
{
	va_list args;
	va_start(args, szMsg);
	ShowMessage (FOREGROUND_RED | FOREGROUND_INTENSITY, szMsg, args);
	va_end(args);
}

void ShowErrorDirect(LPCTSTR szMsg)
{
	ShowMessageDirect (FOREGROUND_RED | FOREGROUND_INTENSITY, szMsg);
}

void ShowWarning(LPCTSTR szMsg, ...)
{
	va_list args;
	va_start(args, szMsg);
	ShowMessage(FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY, szMsg, args);
	va_end(args);
}

void ShowWarningDirect(LPCTSTR szMsg)
{
	ShowMessageDirect(FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY, szMsg);
}

typedef  NTSTATUS(WINAPI* RtlGetVersionFn)(
	PRTL_OSVERSIONINFOW lpVersionInformation);

BOOL GetWindowsVersion(OSVERSIONINFOW* pOSversion)
{
	BOOL bRet = FALSE;
	HMODULE h = LoadLibrary(TEXT("ntdll.dll"));
	if (h != NULL)
	{
		RtlGetVersionFn pRtlGetVersion = (RtlGetVersionFn)GetProcAddress(h, "RtlGetVersion");
		if (pRtlGetVersion != NULL)
		{
			if (NO_ERROR == pRtlGetVersion((PRTL_OSVERSIONINFOW)pOSversion))
				bRet = TRUE;
		}

		FreeLibrary(h);
	}

	return bRet;
}

LPCWSTR GetFileName(LPCWSTR szPath)
{
	size_t len = wcslen(szPath);
	LPCWSTR ptr;
	if (len <= 1)
		return szPath;
	ptr = szPath + (len - 1);
	if (*ptr == L'\\' || *ptr == L'/')
		ptr--;

	while (ptr != szPath)
	{
		if (*ptr == L'\\' || *ptr == L'/')
			break;
		ptr--;
	}

	if (*ptr == L'\\' || *ptr == L'/')
		return ptr + 1;
	else
		return ptr;
}

bool IsDriveLetter(WCHAR c)
{
	return ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'));
}

bool IsAbsolutPath(LPCWSTR szPath)
{
	bool bRet = false;
	size_t pathLen = wcslen(szPath);
	if (pathLen > MAX_PATH)
	{
		if (PathCchSkipRootPtr)
		{
			LPCWSTR ptr = NULL;
			HRESULT hr = PathCchSkipRootPtr(szPath, &ptr);
			if (S_OK == hr)
				bRet = true;
		}
		else
		{
			// do the check manually by looking for drive letter or "\\serverName\" prefix
			if (pathLen >= 4 && szPath[1] == L':' && szPath[2] == L'\\' && IsDriveLetter(szPath[0]))
				bRet = true;
			else if (pathLen >= 5 && szPath[0] == L'\\' && szPath[1] == L'\\')
			{
				for (size_t i = 2; i < (pathLen - 1); i++)
				{
					if (szPath[i] == L'\\')
					{
						bRet = true;
						break;
					}
				}
			}
		}
	}
	else
	{
		if (!PathIsRelativeW(szPath))
			bRet = true;
	}

	return bRet;
}

wstring EnsureAbsolut(LPCWSTR szPath)
{
	wstring strVal = szPath;
	size_t pathLen = wcslen(szPath);
	if (pathLen)
	{
		WCHAR g_szCanonalizedName[MAX_PATH + 1];
		if (IsAbsolutPath(strVal.c_str()))
		{
			if (pathLen > MAX_PATH)
			{
				if (PathAllocCanonicalizePtr)
				{
					LPWSTR pCanonicalName = NULL;
					if (S_OK == PathAllocCanonicalizePtr(strVal.c_str(), PATHCCH_ALLOW_LONG_PATHS, &pCanonicalName))
						strVal = pCanonicalName;

					if (pCanonicalName)
						LocalFree(pCanonicalName);
				}
			}
			else
			{

				if (PathCanonicalizeW(g_szCanonalizedName, strVal.c_str()))
					strVal = g_szCanonalizedName;
			}
		}
		else
		{
			bool bDone = false;
			LPCWSTR szParent = g_currentDirectory.c_str();
			if (PathAllocCombinePtr)
			{
				LPWSTR szCombined = NULL;
				HRESULT hr = PathAllocCombinePtr(szParent, strVal.c_str(), PATHCCH_ALLOW_LONG_PATHS, &szCombined);
				if (S_OK == hr)
				{
					strVal = szCombined;
					bDone = true;
				}

				if (szCombined)
					LocalFree(szCombined);
			}

			if (!bDone && ((wcslen(szParent) + pathLen) < MAX_PATH))
			{
				if (PathCombineW(g_szCanonalizedName, szParent, strVal.c_str()))
				{
					strVal = g_szCanonalizedName;
					bDone = true;
				}
			}

			if (!bDone)
			{
				if ((pathLen >= 2) && strVal[0] == L'\\' && strVal[1] != L'\\')
				{
					// use drive letter of current directory
					WCHAR szDrv[3] = { szParent[0], szParent[1], 0 };
					strVal = szDrv + strVal;
				}
				else
					strVal = szParent + strVal;
			}
		}

		if (!g_bLongPathNamesEnabled)
		{
			// on old Windows versions, use \\?\ prefix to increase the path limit to 32767 characters
			std::wstring res = L"\\\\?\\";
			//detect UNC
			if (strVal[0] == L'\\' && strVal[1] == L'\\')
			{
				res += L"UNC\\";
				strVal = res + (strVal.c_str() + 2);
			}
			else
				strVal = res + strVal;
		}
	}

	return strVal;
}

/* code to detect Symbolic Links, Junction Points and Mount Points from 
 * http://blog.kalmbach-software.de/2008/02/
 * 
 * 
 */


typedef struct _REPARSE_DATA_BUFFER {
	ULONG  ReparseTag;
	USHORT  ReparseDataLength;
	USHORT  Reserved;
	union {
		struct {
			USHORT  SubstituteNameOffset;
			USHORT  SubstituteNameLength;
			USHORT  PrintNameOffset;
			USHORT  PrintNameLength;
			ULONG   Flags; // it seems that the docu is missing this entry (at least 2008-03-07)
			WCHAR  PathBuffer[1];
		} SymbolicLinkReparseBuffer;
		struct {
			USHORT  SubstituteNameOffset;
			USHORT  SubstituteNameLength;
			USHORT  PrintNameOffset;
			USHORT  PrintNameLength;
			WCHAR  PathBuffer[1];
		} MountPointReparseBuffer;
		struct {
			UCHAR  DataBuffer[1];
		} GenericReparseBuffer;
	};
} REPARSE_DATA_BUFFER, * PREPARSE_DATA_BUFFER;

bool IsReparsePoint(LPCTSTR szPath)
{
	bool bRet = false;
	HANDLE hFile;

	hFile = CreateFile(szPath, FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		// Allocate the reparse data structure
		DWORD dwBufSize = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;
		REPARSE_DATA_BUFFER* rdata = (REPARSE_DATA_BUFFER*) new BYTE[dwBufSize];

		// Query the reparse data
		DWORD dwRetLen;
		if (DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 0, rdata, dwBufSize, &dwRetLen, NULL))
		{
			if (IsReparseTagMicrosoft(rdata->ReparseTag))
			{
				if (rdata->ReparseTag == IO_REPARSE_TAG_SYMLINK)
				{
					bRet = true;
				}
				else if (rdata->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) // this applies also for Junction Point
				{
					bRet = true;
				}
			}
		}
		CloseHandle(hFile);


		delete[](BYTE*) rdata;
	}
	return bRet;
}

class CPath
{
protected:
	std::wstring m_path;
	std::wstring m_absolutPath;
public:
	CPath()
	{

	}

	explicit CPath(LPCWSTR szPath) : m_path(szPath)
	{
		std::replace(m_path.begin(), m_path.end(), L'/', L'\\');
		m_absolutPath = EnsureAbsolut(m_path.c_str());
	}

	CPath(LPCWSTR szPath, LPCWSTR szAbsolutPath) : m_path(szPath), m_absolutPath(szPath)
	{

	}

	CPath(const CPath& path) : m_path(path.m_path), m_absolutPath(path.m_absolutPath)
	{

	}

	CPath& operator = (const CPath& p)
	{
		m_path = p.m_path;
		m_absolutPath = p.m_absolutPath;
		return *this;
	}

	CPath& operator = (LPCWSTR p)
	{
		m_path = p;
		std::replace(m_path.begin(), m_path.end(), L'/', L'\\');
		m_absolutPath = EnsureAbsolut(m_path.c_str());
		return *this;
	}

	void AppendName(LPCWSTR szName)
	{
		m_path += L"\\";
		m_path += szName;
		m_absolutPath += L"\\";
		m_absolutPath += szName;
	}

	const std::wstring& GetPathValue() const { return m_path; }
	const std::wstring& GetAbsolutPathValue() const { return m_absolutPath; }
};

// ---------------------------------------------
// class to hold FILE pointers and close them automatically
// it provides a cast operator to FILE*
class CFilePtr
{
protected:
	FILE* m_pFile;
	FILE* m_pShadowFile;
	wstring m_fileName;
	wstring m_shadowFileName;
	// forbid copying
	CFilePtr(const CFilePtr&) : m_pFile(NULL), m_pShadowFile(NULL), m_fileName(L""), m_shadowFileName(L"")
	{

	}
	
	CFilePtr& operator = (const CFilePtr&)
	{
		return *this;
	}
public:
	CFilePtr() : m_pFile(NULL), m_pShadowFile(NULL)
	{

	}


	explicit CFilePtr(FILE* pFile, const wstring& name, FILE* pShadowFile, const wstring& shadowName) 
	: m_pFile(pFile), m_pShadowFile(pShadowFile), m_fileName(name), m_shadowFileName(shadowName)
	{

	}

	~CFilePtr()
	{
		Close();
	}

	const wstring& GetFileName() const { return m_fileName; }
	const wstring& GetShadowFileName() const { return m_shadowFileName; }

	FILE* GetShadowFile() const { return m_pShadowFile; }

	operator FILE* () const { return m_pFile; }

	FILE* operator -> () const { return m_pFile; }

	void CloseShadowFile()
	{
		if (m_pShadowFile)
		{
			fclose(m_pShadowFile);
			m_pShadowFile = NULL;
		}
	}

	void Close()
	{
		if (m_pFile)
		{
			fclose(m_pFile);
			m_pFile = NULL;
		}

		if (m_pShadowFile)
		{
			fclose(m_pShadowFile);
			m_pShadowFile = NULL;
		}
	}
};



// ---------------------------------------------
/*
 * This class is used to make Windows console where we are ruuning compatible with printing UNICODE characters
 * Note that in order to display UNICODE characters correctly, the console should be using a font that support
 * this UNICODE characters. "Courier New" font is a good choice.
 */
class CConsoleUnicodeOutputInitializer
{
protected:
	UINT m_originalCP;

public:
	CConsoleUnicodeOutputInitializer()
	{
		m_originalCP = GetConsoleOutputCP();
		
		SetConsoleOutputCP(CP_UTF8);
		_setmode(_fileno(stdout), _O_U8TEXT);
	}

	~CConsoleUnicodeOutputInitializer()
	{
		SetConsoleOutputCP(m_originalCP);
	}

};



// ---------------------------------------------

class Hash
{
public:
	virtual void Init() = 0;
	virtual void Update(LPCBYTE pbData, size_t dwLength) = 0;
	virtual void Final(LPBYTE pbDigest) = 0;
	virtual int GetHashSize() = 0;
	virtual LPCTSTR GetID() = 0;
	virtual bool IsValid() const { return true; }
	virtual bool UsesMSCrypto() const { return false; }
	virtual Hash* Clone() { return GetHash(GetID()); }
	static bool IsHashId(LPCTSTR szHashId);
	static bool IsHashSize(int size);
	static Hash* GetHash(LPCTSTR szHashId);
	static std::vector<std::wstring> GetSupportedHashIds();
	static bool IsHashIdCombination(LPCTSTR szHashId);
	static std::vector<std::shared_ptr<Hash>> GetHashes(LPCTSTR szHashId);

	void* operator new(size_t i)
	{
		return _mm_malloc(i, 16);
	}

	void operator delete(void* p)
	{
		_mm_free(p);
	}
};

class NeonBlake2s : public Hash
{
protected:
	blake2s_state m_ctx;
public:
	NeonBlake2s() : Hash()
	{
		Init();
	}

	void Init() { blake2s_init(&m_ctx, BLAKE2S_OUTBYTES); }
	void Update(LPCBYTE pbData, size_t dwLength) { blake2s_update(&m_ctx, pbData, dwLength); }
	void Final(LPBYTE pbDigest) { blake2s_final(&m_ctx, pbDigest, BLAKE2S_OUTBYTES); }
	LPCTSTR GetID() { return _T("Blake2s"); }
	int GetHashSize() { return BLAKE2S_OUTBYTES; }
};

class NeonBlake2b : public Hash
{
protected:
	blake2b_state m_ctx;
public:
	NeonBlake2b() : Hash()
	{
		Init();
	}

	void Init() { blake2b_init(&m_ctx, BLAKE2B_OUTBYTES); }
	void Update(LPCBYTE pbData, size_t dwLength) { blake2b_update(&m_ctx, pbData, dwLength); }
	void Final(LPBYTE pbDigest) { blake2b_final(&m_ctx, pbDigest, BLAKE2B_OUTBYTES); }
	LPCTSTR GetID() { return _T("Blake2b"); }
	int GetHashSize() { return BLAKE2B_OUTBYTES; }
};

#ifdef USE_STREEBOG
class Streebog : public Hash
{
protected:
	STREEBOG_CTX m_ctx;
public:
	Streebog() : Hash()
	{
		STREEBOG_init(&m_ctx);
	}

	void Init() { STREEBOG_init(&m_ctx); }
	void Update(LPCBYTE pbData, size_t dwLength) { STREEBOG_add(&m_ctx, pbData, dwLength); }
	void Final(LPBYTE pbDigest) { STREEBOG_finalize(&m_ctx, pbDigest); }
	LPCTSTR GetID() { return _T("Streebog"); }
	int GetHashSize() { return 64; }
};
#endif

class CngHash : public Hash
{
protected:
	BCRYPT_ALG_HANDLE m_hAlg;
	BCRYPT_HASH_HANDLE m_hash;
	LPWSTR m_wszAlg;
	ULONG m_cbHashObject;
	unsigned char* m_pbHashObject;
public:
	CngHash(LPCWSTR wszAlg) : Hash(), m_hAlg(NULL), m_hash(NULL), m_wszAlg(NULL), m_pbHashObject(NULL), m_cbHashObject(0)
	{
		m_wszAlg = _wcsdup(wszAlg);
		Init();
	}

	virtual ~CngHash()
	{
		Clear();
		if (m_wszAlg)
			free(m_wszAlg);
	}

	void Clear()
	{
		if (m_hash)
			BCryptDestroyHash(m_hash);
		if (m_hAlg)
			BCryptCloseAlgorithmProvider(m_hAlg, 0);
		if (m_pbHashObject)
			delete[] m_pbHashObject;
		m_pbHashObject = NULL;
		m_cbHashObject = 0;
		m_hash = NULL;
		m_hAlg = NULL;
	}

	virtual bool IsValid() const { return (m_hash != NULL); }
	virtual bool UsesMSCrypto() const { return true; }

	virtual void Init() {
		Clear();
		if (STATUS_SUCCESS == BCryptOpenAlgorithmProvider(&m_hAlg, m_wszAlg, MS_PRIMITIVE_PROVIDER, 0))
		{
			DWORD dwValue, count = sizeof(DWORD);
			if (STATUS_SUCCESS == BCryptGetProperty(m_hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&dwValue, count, &count, 0))
			{
				m_cbHashObject = dwValue;
				m_pbHashObject = new unsigned char[dwValue];
				if (STATUS_SUCCESS != BCryptCreateHash(m_hAlg, &m_hash, m_pbHashObject, m_cbHashObject, NULL, 0, 0))
				{
					m_cbHashObject = 0;
					delete[] m_pbHashObject;
					m_pbHashObject = NULL;
				}
			}
		}

		if (!m_pbHashObject)
		{
			Clear();
		}
	}

	virtual void Update(LPCBYTE pbData, size_t dwLength) {
		if (IsValid())
		{
			BCryptHashData(m_hash, (PUCHAR)pbData, (ULONG)dwLength, 0);
		}
	}

	virtual void Final(LPBYTE pbDigest) {
		if (IsValid())
		{
			ULONG dwHashLen = (ULONG)GetHashSize();
			BCryptFinishHash(m_hash, pbDigest, dwHashLen, 0);
		}
	}
};

class Md5Cng : public CngHash
{
public:
	Md5Cng() : CngHash(BCRYPT_MD5_ALGORITHM)
	{

	}

	~Md5Cng()
	{
	}

	LPCTSTR GetID() { return _T("MD5"); }
	int GetHashSize() { return 16; }
};

class Sha1Cng : public CngHash
{
public:
	Sha1Cng() : CngHash(BCRYPT_SHA1_ALGORITHM)
	{

	}

	~Sha1Cng()
	{
	}

	LPCTSTR GetID() { return _T("SHA1"); }
	int GetHashSize() { return 20; }
};

class Sha256Cng : public CngHash
{
public:
	Sha256Cng() : CngHash(BCRYPT_SHA256_ALGORITHM)
	{

	}

	~Sha256Cng()
	{
	}

	LPCTSTR GetID() { return _T("SHA256"); }
	int GetHashSize() { return 32; }
};

class Sha384Cng : public CngHash
{
public:
	Sha384Cng() : CngHash(BCRYPT_SHA384_ALGORITHM)
	{

	}

	~Sha384Cng()
	{
	}

	LPCTSTR GetID() { return _T("SHA384"); }
	int GetHashSize() { return 48; }
};

class Sha512Cng : public CngHash
{
public:
	Sha512Cng() : CngHash(BCRYPT_SHA512_ALGORITHM)
	{

	}

	~Sha512Cng()
	{
	}

	LPCTSTR GetID() { return _T("SHA512"); }
	int GetHashSize() { return 64; }
};

class Blake3Hash : public Hash
{
protected:
	blake3_hasher m_ctx;
public:
	Blake3Hash() : Hash()
	{
		Init();
	}

	void Init() { blake3_hasher_init(&m_ctx); }
	void Update(LPCBYTE pbData, size_t dwLength) { blake3_hasher_update(&m_ctx, pbData, dwLength); }
	void Final(LPBYTE pbDigest) { blake3_hasher_finalize(&m_ctx, pbDigest, BLAKE3_OUT_LEN); }
	LPCTSTR GetID() { return _T("Blake3"); }
	int GetHashSize() { return BLAKE3_OUT_LEN; }

};

bool Hash::IsHashId(LPCTSTR szHashId)
{
	if ((_tcsicmp(szHashId, _T("SHA1")) == 0)
		|| (_tcsicmp(szHashId, _T("SHA256")) == 0)
		|| (_tcsicmp(szHashId, _T("SHA384")) == 0)
		|| (_tcsicmp(szHashId, _T("SHA512")) == 0)
		|| (_tcsicmp(szHashId, _T("MD5")) == 0)
		|| (_tcsicmp(szHashId, _T("Streebog")) == 0)
		|| (_tcsicmp(szHashId, _T("Blake2s")) == 0)
		|| (_tcsicmp(szHashId, _T("Blake2b")) == 0)
		|| (_tcsicmp(szHashId, _T("Blake3")) == 0)
		)
	{
		return true;
	}
	else
		return false;
}

// Check if a given string is a single hash id or combination of several hash ids separated by a comma character
// for example: "SHA512" or "SHA1,MD5" or "SHA256,Blake2s,Blake3"
// it uses the IsHashId method to check if each part of the string is a valid hash id
// if the string is a combination of hash ids, it returns true, otherwise it returns false
bool Hash::IsHashIdCombination(LPCTSTR szHashId)
{
	if (!szHashId)
		return false;

	std::wstring strHashId(szHashId);
	std::wstring strHashIdPart;
	std::wstring::size_type pos = 0;
	std::wstring::size_type pos2 = 0;
	std::wstring::size_type posEnd = strHashId.length();

	while (pos != posEnd)
	{
		pos2 = strHashId.find_first_of(L",", pos);
		if (pos2 == std::wstring::npos)
			pos2 = posEnd;

		strHashIdPart = strHashId.substr(pos, pos2 - pos);

		if (!IsHashId(strHashIdPart.c_str()))
			return false;

		pos = pos2;
		if (pos != posEnd)
		{
			pos++;
			if (pos == posEnd)
			{
				// there is a comma at the end without anything after it. so this is error.
				return false;
			}
		}
	}

	return true;
}

// Rreturn a vector of Hash instance smart pointers for a given hash id combination
// if the string is a single hash id, it returns a vector with a single Hash instance smart pointer
// if the string is a combination of hash ids, it returns a vector with a Hash instance smart pointer for each hash id
// if the string is not a valid hash id or combination of hash ids, it returns an empty vector
std::vector<std::shared_ptr<Hash>> Hash::GetHashes(LPCTSTR szHashId)
{
	std::vector<std::shared_ptr<Hash>> res;

	if (!szHashId)
		return res;

	std::wstring strHashId(szHashId);
	std::wstring strHashIdPart;
	std::wstring::size_type pos = 0;
	std::wstring::size_type pos2 = 0;
	std::wstring::size_type posEnd = strHashId.length();

	while (pos != posEnd)
	{
		pos2 = strHashId.find_first_of(L",", pos);
		if (pos2 == std::wstring::npos)
			pos2 = posEnd;

		strHashIdPart = strHashId.substr(pos, pos2 - pos);

		if (IsHashId(strHashIdPart.c_str()))
		{
			std::shared_ptr<Hash> hash(GetHash(strHashIdPart.c_str()));
			if (hash)
				res.push_back(hash);
		}
		else
		{
			res.clear();
			break;
		}

		pos = pos2;
		if (pos != posEnd)
		{
			pos++;
			if (pos == posEnd)
			{
				// there is a comma at the end without anything after it. so this is error.
				res.clear();
				break;
			}
		}
	}

	return res;
}

std::vector<std::wstring> Hash::GetSupportedHashIds()
{
	std::vector<std::wstring> res;

	res.push_back(L"MD5");
	res.push_back(L"SHA1");
	res.push_back(L"SHA256");
	res.push_back(L"SHA384");
	res.push_back(L"SHA512");
	res.push_back(L"Streebog");
	res.push_back(L"Blake2s");
	res.push_back(L"Blake2b");
	res.push_back(L"Blake3");
	return res;
}

bool Hash::IsHashSize(int size)
{
	switch (size)
	{
	case 16:
	case 20:
	case 32:
	case 48:
	case 64:
		return true;
	}

	return false;
}

Hash* Hash::GetHash(LPCTSTR szHashId)
{
	if (!szHashId || (_tcsicmp(szHashId, _T("SHA1")) == 0))
	{
		return new Sha1Cng();
	}
	if (_tcsicmp(szHashId, _T("SHA256")) == 0)
	{
		return new Sha256Cng();
	}
	if (_tcsicmp(szHashId, _T("SHA384")) == 0)
	{
		return new Sha384Cng();
	}
	if (_tcsicmp(szHashId, _T("SHA512")) == 0)
	{
		return new Sha512Cng();
	}
	if (_tcsicmp(szHashId, _T("MD5")) == 0)
	{
		return new Md5Cng();
	}
	if (_tcsicmp(szHashId, _T("Blake2s")) == 0)
	{
		return new NeonBlake2s();
	}
	if (_tcsicmp(szHashId, _T("Blake2b")) == 0)
	{
		return new NeonBlake2b();
	}
	if (_tcsicmp(szHashId, _T("Blake3")) == 0)
	{
		return new Blake3Hash();
	}
#ifdef USE_STREEBOG
	if (_tcsicmp(szHashId, _T("Streebog")) == 0)
	{
		return new Streebog();
	}
#endif
	return NULL;
}

// This function clones the Hash instances present on the vector and stores them in the output vector
void CloneHashes(vector<shared_ptr<Hash>>& pHashes, vector<shared_ptr<Hash>>& pOutputHashes)
{
	for (size_t i = 0; i < pHashes.size(); i++)
	{
		pOutputHashes.push_back(shared_ptr<Hash>(pHashes[i]->Clone()));
	}
}

// This function calls Update method on each instance of Hash stored in the vector
void UpdateHashes(vector<shared_ptr<Hash>>& pHashes, LPCBYTE pbBuffer, size_t dwBufferSize)
{
	for (size_t i = 0; i < pHashes.size(); i++)
	{
		pHashes[i]->Update(pbBuffer, dwBufferSize);
	}
}

// ----------------------------------------------------------

class HashResultEntry
{
public:
	wstring m_hashName;
	ByteArray m_digest;
	mutable bool m_processed;

	HashResultEntry() : m_hashName(L""), m_processed (false){}
	HashResultEntry(const HashResultEntry& hre) : m_hashName(hre.m_hashName), m_digest(hre.m_digest), m_processed (hre.m_processed) {}
	~HashResultEntry() {}

	HashResultEntry& operator = (const HashResultEntry& hre) { m_hashName = hre.m_hashName; m_digest = hre.m_digest; m_processed = hre.m_processed; return *this; }
};

class CDirContent
{
protected:
	CPath m_szPath;
	bool m_bIsDir;
public:
	CDirContent(CPath szPath, LPCWSTR szName, bool bIsDir) : m_bIsDir(bIsDir), m_szPath(szPath)
	{
		m_szPath.AppendName(szName);
	}

	CDirContent(const CDirContent& content) : m_bIsDir(content.m_bIsDir), m_szPath(content.m_szPath) {}

	CDirContent& operator = (const CDirContent& content)
	{
		m_bIsDir = content.m_bIsDir;
		m_szPath = content.m_szPath;
		return *this;
	}

	bool IsDir() const { return m_bIsDir; }
	const CPath& GetPath() const { return m_szPath; }
	operator LPCWSTR () { return m_szPath.GetPathValue().c_str(); }
};

bool IsExcludedName(LPCTSTR szName, bool bIsFile)
{
    // Include check
    if (bIsFile && !onlySpecList.empty()) {// -only applied only to files
        bool matched = false;
        for (list<wstring>::iterator It = onlySpecList.begin(); It != onlySpecList.end(); It++) {
            if (PathMatchSpec(szName, It->c_str())) {
                matched = true;
                break;
            }
        }
        
		return !matched;
    }

    // Exclude check
	for (list<wstring>::iterator It = excludeSpecList.begin(); It != excludeSpecList.end(); It++)
	{
		if (PathMatchSpec(szName, It->c_str()))
			return true;
	}

	return false;
}

// return the file name. If it is too long, it is shortness so that the progress line 
LPCTSTR GetShortFileName(LPCTSTR szFilePath, unsigned long long fileSize)
{
	static TCHAR szShortName[256];
	size_t l, bufferSize = ARRAYSIZE(szShortName);
	int maxPrintLen = _scprintf(" [==========] 100.00 %% (%llu/%llu)", fileSize, fileSize); // 10 steps for progress bar
	LPCTSTR ptr = &szFilePath[_tcslen(szFilePath) - 1];

	// Get file name part from the path
	while ((ptr != szFilePath) && (*ptr != _T('\\')) && (*ptr != _T('/')))
	{
		ptr--;
	}
	ptr++;

	// calculate maximum length for file name	
	bufferSize = (g_originalConsoleInfo.dwSize.X > (maxPrintLen + 1)) ? min(bufferSize, (g_originalConsoleInfo.dwSize.X - 1 - maxPrintLen)) : 9;

	l = _tcslen(ptr);
	if (l < bufferSize)
		_tcscpy(szShortName, ptr);
	else
	{
		size_t prefixLen = (bufferSize / 2 - 2);
		size_t suffixLen = bufferSize - prefixLen - 4;

		memcpy(szShortName, ptr, prefixLen * sizeof(TCHAR));
		memcpy(((unsigned char*)szShortName) + prefixLen * sizeof(TCHAR), _T("..."), 3 * sizeof(TCHAR));
		memcpy(((unsigned char*)szShortName) + (prefixLen + 3) * sizeof(TCHAR), ptr + (l - suffixLen), suffixLen * sizeof(TCHAR));
		szShortName[bufferSize - 1] = 0;
	}
	return szShortName;
}

void DisplayProgress(LPCTSTR szFileName, unsigned long long currentSize, unsigned long long fileSize, clock_t startTime, clock_t& lastBlockTime)
{
	clock_t t = clock();
	if (lastBlockTime == 0 || currentSize == fileSize || ((t - lastBlockTime) >= CLOCKS_PER_SEC))
	{
		unsigned long long maxPos = 10ull;
		unsigned long long pos = (currentSize * maxPos) / fileSize;
		double pourcentage = ((double)currentSize / (double)fileSize) * 100.0;

		lastBlockTime = t;

		_tprintf(_T("\r%s ["), szFileName);
		for (unsigned long long i = 0; i < maxPos; i++)
		{
			if (i < pos)
				_tprintf(_T("="));
			else
				_tprintf(_T(" "));
		}
		_tprintf(_T("] %.2f %% (%llu/%llu)"), pourcentage, currentSize, fileSize);

		_tprintf(_T("\r"));
	}
}

void ClearProgress()
{
	_tprintf(_T("\r"));
	for (int i = 0; i < g_originalConsoleInfo.dwSize.X - 1; i++)
	{
		_tprintf(_T(" "));
	}

	_tprintf(_T("\r"));
}

typedef struct _threadParam
{
	CPath filePath;
	ULONGLONG fileSize;
	bool bQuiet;
	bool bShowProgress;
	bool bSumMode;
	bool bSumVerificationMode;
	ByteArray pbExpectedDigest;
	vector<shared_ptr<Hash>> pHashes;

	_threadParam(const CPath& fp) : filePath(fp), fileSize(0), bQuiet(false), bShowProgress(false), bSumMode(false), bSumVerificationMode(false) {}
} threadParam;

typedef struct _JOB_ITEM {
	SLIST_ENTRY ItemEntry;
	threadParam* pParam;
} JOB_ITEM, * PJOB_ITEM;

typedef struct _OUTPUT_ITEM {
	SLIST_ENTRY ItemEntry;
	std::wstring* pParam;
	std::wstring* pConsoleParam;
	bool bQuiet;
	bool bError;
	bool bSkipOutputFile;
	size_t nOutputFile;
} OUTPUT_ITEM, * POUTPUT_ITEM;

PSLIST_HEADER g_jobsList = NULL;
PSLIST_HEADER g_outputsList = NULL;

void FreejobList()
{
	JOB_ITEM* pJob;
	
	while (pJob = (JOB_ITEM*)InterlockedPopEntrySList(g_jobsList))
	{
		threadParam* p = pJob->pParam;
		delete p;
		_aligned_free(pJob);
	}

	InterlockedFlushSList(g_jobsList);
	_aligned_free(g_jobsList);

}

void FreeOutputList()
{
	OUTPUT_ITEM* pOutput;

	while ((pOutput = (OUTPUT_ITEM*)InterlockedPopEntrySList(g_outputsList)))
	{
		std::wstring* p = pOutput->pParam;
		delete p;
		_aligned_free(pOutput);
	}

	InterlockedFlushSList(g_outputsList);
	_aligned_free(g_outputsList);

}

void AddHashJobEntry(threadParam* pParam)
{
	JOB_ITEM* pJobItem = (JOB_ITEM*)_aligned_malloc(sizeof(JOB_ITEM), MEMORY_ALLOCATION_ALIGNMENT);
	if (NULL == pJobItem)
		return;

	pJobItem->pParam = pParam;
	InterlockedPushEntrySList(g_jobsList, &(pJobItem->ItemEntry));
}

void AddOutputEntry(std::wstring* pParam, std::wstring* pConsoleParam, bool bQuiet, bool bError, bool bSkipOutputFile, size_t nOutputFile)
{
	OUTPUT_ITEM* pOutputItem = (OUTPUT_ITEM*)_aligned_malloc(sizeof(OUTPUT_ITEM), MEMORY_ALLOCATION_ALIGNMENT);
	if (NULL == pOutputItem)
		return;

	pOutputItem->pParam = pParam;
	pOutputItem->pConsoleParam = pConsoleParam;
	pOutputItem->bQuiet = bQuiet;
	pOutputItem->bError = bError;
	pOutputItem->bSkipOutputFile = bSkipOutputFile;
	pOutputItem->nOutputFile = nOutputFile;
	InterlockedPushEntrySList(g_outputsList, &(pOutputItem->ItemEntry));

	SetEvent(g_hOutputReadyEvent);
}

void AddHashJob(const CPath& filePath, ULONGLONG fileSize, bool bQuiet, bool bShowProgress, bool bSumMode, bool bSumVerificationMode, LPCBYTE pbExpectedDigest, vector<shared_ptr<Hash>>& pHashes)
{
	threadParam* p = new threadParam(filePath);
	p->fileSize = fileSize;
	p->bQuiet = bQuiet;
	p->bShowProgress = bShowProgress;
	p->bSumMode = bSumMode;
	p->bSumVerificationMode = bSumVerificationMode;
	if (bSumVerificationMode && pbExpectedDigest)
	{
		// In verification mode, we always have a single hash
		p->pbExpectedDigest.resize(pHashes[0]->GetHashSize());
		memcpy(p->pbExpectedDigest.data(), pbExpectedDigest, pHashes[0]->GetHashSize());
	}
	p->pHashes = pHashes;

	AddHashJobEntry(p);

	SetEvent(g_hReadyEvent);
}

void ProcessFile(HANDLE f, ULONGLONG fileSize, LPCTSTR szFilePath, bool bQuiet, bool bShowProgress, bool bSumMode, bool bSumVerificationMode, LPCBYTE pbExpectedDigest, vector<shared_ptr<Hash>>& pHashes, LPBYTE pbBuffer, size_t cbBuffer)
{
	bShowProgress = !bQuiet && bShowProgress && !g_threadsCount; // no progress shown in case of multitheaded computation
	unsigned long long currentSize = 0;
	clock_t startTime = bShowProgress ? clock() : 0;
	clock_t lastBlockTime = 0;
	LPCTSTR szFileName = bShowProgress ? GetShortFileName(szFilePath, fileSize) : NULL;
	DWORD cbCount = 0;

	while (ReadFile(f, pbBuffer, (DWORD) cbBuffer, &cbCount, NULL) && cbCount)
	{
		currentSize += (unsigned long long) cbCount;
		UpdateHashes(pHashes,pbBuffer, cbCount);
		if (bShowProgress)
			DisplayProgress(szFileName, currentSize, fileSize, startTime, lastBlockTime);
		if (currentSize == fileSize)
			break;
	}

	CloseHandle(f);

	if (bShowProgress)
		ClearProgress();

	if (bSumMode)
	{
		if (bSumVerificationMode)
		{
			// in verification mode we only have one hash
			BYTE pbSumDigest[128];
			pHashes[0]->Final(pbSumDigest);
			if (memcmp(pbSumDigest, pbExpectedDigest, pHashes[0]->GetHashSize()))
			{
				g_bMismatchFound = true;

				std::wstring szMsg = FormatString(L"Hash value mismatch for \"%s\"\n", szFilePath);

				if (g_threadsCount)
				{
					if (!bQuiet || outputFiles[0])
					{
						AddOutputEntry(new std::wstring(szMsg), NULL, bQuiet, false, false, 0);
					}
				}
				else
				{
					if (!bQuiet) ShowWarningDirect(szMsg.c_str());
					if (outputFiles[0]) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
				}				
			}
		}
		else
		{
			BYTE pbSumDigest[128];
			WCHAR szDigestHex[129]; // enough for 64 bytes digest
			bool bMultiHash = pHashes.size() > 1;
			for (size_t i = 0; i < pHashes.size(); i++)
			{
				
				pHashes[i]->Final(pbSumDigest);

				ToHex(pbSumDigest, pHashes[i]->GetHashSize(), szDigestHex);

				std::wstring szMsg = szDigestHex;
				szMsg += L"  ";
				if (g_bSumRelativePath)
				{
					// remove the input directory from the path written to the SUM file
					szMsg += (szFilePath + g_inputDirPathLength);
				}
				else
					szMsg += szFilePath;
				szMsg += L"\n";

				wstring szConsoleMsg;
				if (!bQuiet && bMultiHash)
				{
					szConsoleMsg = FormatString(L"%s: %s", pHashes[i]->GetID(), szMsg.c_str());
				}
				else
				{
					szConsoleMsg = szMsg;
				}

				if (g_threadsCount)
				{
					if (!bQuiet || outputFiles[i])
					{
						AddOutputEntry(new std::wstring(szMsg), new std::wstring(szConsoleMsg), bQuiet, false, false, i);
					}
				}
				else
				{
					if (!bQuiet) ShowWarningDirect(szConsoleMsg.c_str());
					if (outputFiles[i]) _ftprintf(*outputFiles[i], L"%s", szMsg.c_str());
				}
			}
		}
	}
}

DWORD WINAPI OutputThreadCode(LPVOID pArg)
{
	HANDLE syncObjs[2] = { g_hOutputReadyEvent, g_hOutputStopEvent };

	SetConsoleTextAttribute(g_hConsole, FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY);

	while (!g_bFatalError)
	{
		std::wstring* p = NULL;
		std::wstring* pConsole = NULL;
		OUTPUT_ITEM* pOutput;
		
		while (!g_bFatalError && (pOutput = (OUTPUT_ITEM*)InterlockedPopEntrySList(g_outputsList)))
		{
			bool bDeleteConsole = true;
			p = pOutput->pParam;
			pConsole = pOutput->pConsoleParam;
			if (!pConsole)
			{
				bDeleteConsole = false;
				pConsole = p;
			}
			if (!pOutput->bQuiet)
			{
				if (pOutput->bError)
					ShowErrorDirect(pConsole->c_str());
				else
					ShowWarningDirect(pConsole->c_str());
			}
			if (!pOutput->bSkipOutputFile && outputFiles[pOutput->nOutputFile]) {
				FILE* fTarget = *outputFiles[pOutput->nOutputFile];
				// write to shadow file if it is enabled
				FILE* fShadow = outputFiles[pOutput->nOutputFile]->GetShadowFile();
				if (fShadow) fTarget = fShadow;

				_ftprintf(fTarget, L"%s", p->c_str());
			}
			delete p;
			if (bDeleteConsole) delete pConsole;
			_aligned_free(pOutput);
		}

		if (g_bStopOutputThread || g_bFatalError)
			break;
		else
		{
			WaitForMultipleObjects(2, syncObjs, FALSE, INFINITE);
		}
	}

	return 0;
}

DWORD WINAPI ThreadCode(LPVOID pArg)
{
	BYTE pbBuffer[4096];
	HANDLE syncObjs[2] = { g_hReadyEvent, g_hStopEvent };

	SetThreadGroupAffinityFn SetThreadGroupAffinityPtr = (SetThreadGroupAffinityFn)GetProcAddress(GetModuleHandle(L"kernel32.dll"), "SetThreadGroupAffinity");
	if (SetThreadGroupAffinityPtr && pArg)
	{
		GROUP_AFFINITY groupAffinity = { 0 };
		groupAffinity.Mask = ~0ULL;
		groupAffinity.Group = *(WORD*)(pArg);
		SetThreadGroupAffinityPtr(GetCurrentThread(), &groupAffinity, NULL);
	}

	while (!g_bFatalError)
	{
		threadParam* p = NULL;

		JOB_ITEM* pJob = (JOB_ITEM*) InterlockedPopEntrySList(g_jobsList);

		if (pJob)
		{
			p = pJob->pParam;
			// open the file handle
			const wstring& szFilePath = p->filePath.GetPathValue();
			const wstring& szAbsolutePath = p->filePath.GetAbsolutPathValue();
			HANDLE f = CreateFileW(szAbsolutePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			if (f == INVALID_HANDLE_VALUE)
			{
				std::wstring szMsg = FormatString (_T("Failed to open file \"%s\" for reading (error 0x%.8X)\n"), szFilePath.c_str(), GetLastError());
				if (outputFiles[0] && (!p->bSumMode || p->bSumVerificationMode)) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
				if (g_bSkipError)
				{
					if (!p->bQuiet)
					{
						AddOutputEntry(new std::wstring(szMsg), NULL, p->bQuiet, true, true, 0);
					}
					
					if (p->bSumMode) g_bMismatchFound = true;
				}
				else
				{		
					g_szLastErrorMsg = szMsg;
				}
			}
			else
			{
				// ProcessFile will  close the file handle
				ProcessFile(f, p->fileSize, szFilePath.c_str(), p->bQuiet, p->bShowProgress, p->bSumMode, p->bSumVerificationMode, p->pbExpectedDigest.data(), p->pHashes, pbBuffer, sizeof (pbBuffer));
			}
			delete p;
			_aligned_free(pJob);
		}
		else if (g_bStopThreads || g_bFatalError)
			break;
		else
		{			
			WaitForMultipleObjects(2, syncObjs, FALSE, INFINITE);
		}
	}
	SecureZeroMemory(pbBuffer, sizeof(pbBuffer));
	return 0;
}

size_t GetCpuCount(WORD* pGroupCount)
{
	size_t cpuCount = 0;
	SYSTEM_INFO sysInfo;
	GetActiveProcessorGroupCountFn GetActiveProcessorGroupCountPtr = (GetActiveProcessorGroupCountFn)GetProcAddress(GetModuleHandle(L"Kernel32.dll"), "GetActiveProcessorGroupCount");
	GetActiveProcessorCountFn GetActiveProcessorCountPtr = (GetActiveProcessorCountFn)GetProcAddress(GetModuleHandle(L"Kernel32.dll"), "GetActiveProcessorCount");
	if (GetActiveProcessorGroupCountPtr && GetActiveProcessorCountPtr)
	{
		WORD j, groupCount = GetActiveProcessorGroupCountPtr();
		size_t totalProcessors = 0;
		for (j = 0; j < groupCount; ++j)
		{
			totalProcessors += (size_t)GetActiveProcessorCountPtr(j);
		}
		cpuCount = totalProcessors;
		if (pGroupCount)
			*pGroupCount = groupCount;
	}
	else
	{
		GetSystemInfo(&sysInfo);
		cpuCount = (size_t)sysInfo.dwNumberOfProcessors;
		if (pGroupCount)
			*pGroupCount = 1;
	}

	return cpuCount;
}

void StartThreads(bool bOutputThread)
{
	size_t cpuCount = 0, i = 0;
	WORD groupCount = 1;
	uint32 ThreadCount = 0;

	cpuCount = GetCpuCount(&groupCount);

	if (cpuCount > ARRAYSIZE(g_hThreads))
		cpuCount = ARRAYSIZE(g_hThreads);

	if (cpuCount <= 1)
		return;

	g_jobsList = (PSLIST_HEADER)_aligned_malloc(sizeof(SLIST_HEADER), MEMORY_ALLOCATION_ALIGNMENT);
	g_outputsList = (PSLIST_HEADER)_aligned_malloc(sizeof(SLIST_HEADER), MEMORY_ALLOCATION_ALIGNMENT);
	InitializeSListHead(g_jobsList);
	InitializeSListHead(g_outputsList);

	g_hReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	g_hOutputReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	g_hOutputStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	for (ThreadCount = 0; ThreadCount < (uint32) cpuCount; ++ThreadCount)
	{
		WORD* pThreadArg = NULL;
		if (groupCount > 1)
		{

			GetActiveProcessorCountFn GetActiveProcessorCountPtr = (GetActiveProcessorCountFn)GetProcAddress(GetModuleHandle(L"Kernel32.dll"), "GetActiveProcessorCount");
			// Determine which processor group to bind the thread to.
			if (GetActiveProcessorCountPtr)
			{
				WORD j;
				uint32 totalProcessors = 0U;
				for (j = 0U; j < groupCount; j++)
				{
					totalProcessors += (uint32)GetActiveProcessorCountPtr(j);
					if (totalProcessors >= ThreadCount)
					{
						ThreadProcessorGroups[ThreadCount] = j;
						break;
					}
				}
			}
			else
				ThreadProcessorGroups[ThreadCount] = 0;

			pThreadArg = &ThreadProcessorGroups[ThreadCount];
		}

		g_hThreads[ThreadCount] = CreateThread(NULL, 0, ThreadCode, (void*)pThreadArg, 0, NULL);
	}

	g_threadsCount = ThreadCount;

	if (bOutputThread)
		g_hOutputThread = CreateThread(NULL, 0, OutputThreadCode, NULL, 0, NULL);
}

void StopThreads(bool bError)
{
	if (g_threadsCount)
	{
		g_bFatalError = bError;
		g_bStopThreads = true;
		SetEvent(g_hStopEvent);
		
		WaitForMultipleObjects(g_threadsCount, g_hThreads, TRUE, INFINITE);

		for (DWORD i = 0; i < g_threadsCount; i++)
		{
			CloseHandle(g_hThreads[i]);
		}
		CloseHandle(g_hReadyEvent);
		CloseHandle(g_hStopEvent);

		g_bStopOutputThread = true;
		SetEvent(g_hOutputStopEvent);

		WaitForSingleObject(g_hOutputThread, INFINITE);
		CloseHandle(g_hOutputThread);

		CloseHandle(g_hOutputReadyEvent);
		CloseHandle(g_hOutputStopEvent);

		FreejobList();
		FreeOutputList();
	}
}


static CPath g_outputFileName;
static CPath g_verificationFileName;

DWORD HashFile(const CPath& filePath, vector<shared_ptr<Hash>>& pHashes, bool bIncludeNames, bool bStripNames, bool bQuiet, bool bShowProgress, bool bSumMode, const map<wstring, HashResultEntry>& digestList)
{
	DWORD dwError = 0;
	HANDLE f;
	LARGE_INTEGER fileSize;
	LPCWSTR szFilePath = filePath.GetPathValue().c_str();
	int pathLen = lstrlen(szFilePath);
	map<wstring, HashResultEntry>::const_iterator It;
	bool bSumVerificationMode = false;
	LPCBYTE pbExpectedDigest = NULL;
	vector<shared_ptr<Hash>> pClonedHashes;
	vector<shared_ptr<Hash>>& pHashesToUse = pHashes;
	wstring fileAbsolutPath = filePath.GetAbsolutPathValue();

	if (IsExcludedName(szFilePath, true))
		return 0;

	if (bSumMode)
	{
		if (!digestList.empty())
		{
			// check that the current file is specified in the checksum file
				It = digestList.find(szFilePath);
			if (It == digestList.end())
			{
				std::wstring szMsg = FormatString(_T("Error: file \"%s\" not found in checksum file.\n"), szFilePath);
				
				if (outputFiles[0]) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
				if (g_bSkipError)
				{					
					if (!bQuiet)
					{
						if (g_threadsCount)
							AddOutputEntry(new std::wstring(szMsg), NULL, bQuiet, true, true, 0);
						else
							ShowErrorDirect(szMsg.c_str());
					}
					g_bMismatchFound = true;
					return 0;					
				}
				else
				{		
					g_szLastErrorMsg = szMsg;
					return -5;
				}
			}
			else
			{
				It->second.m_processed = true;
				pbExpectedDigest = It->second.m_digest.data();
				bSumVerificationMode = true;
			}
		}
		
		CloneHashes(pHashes, pClonedHashes);
		pHashesToUse = pClonedHashes;
	}

	if (bIncludeNames)
	{
		LPCTSTR pNameToHash = NULL;
		LPWSTR pCanonicalName = NULL;
		if (pathLen > MAX_PATH)
		{
			if (PathAllocCanonicalizePtr)
			{
				if (S_OK == PathAllocCanonicalizePtr(szFilePath, PATHCCH_ALLOW_LONG_PATHS, &pCanonicalName))
					pNameToHash = pCanonicalName;
				else
					pNameToHash = szFilePath;
			}
			else
			{
				pNameToHash = szFilePath;
			}
			if (bStripNames)
				pNameToHash = GetFileName(pNameToHash);
		}
		else
		{
			g_szCanonalizedName[MAX_PATH] = 0;
			if (!PathCanonicalize(g_szCanonalizedName, szFilePath))
				lstrcpy(g_szCanonalizedName, szFilePath);

			if (bStripNames)
				pNameToHash = GetFileName(g_szCanonalizedName);
			else
				pNameToHash = g_szCanonalizedName;
		}

		UpdateHashes(pHashesToUse ,(LPCBYTE)pNameToHash, _tcslen(pNameToHash) * sizeof(TCHAR));

		if (pCanonicalName)
			LocalFree(pCanonicalName);
	}

	f = CreateFileW(fileAbsolutPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f != INVALID_HANDLE_VALUE)
	{
		if (!GetFileSizeEx(f, &fileSize))
		{
			DWORD dwErr = GetLastError();
			CloseHandle(f);
			f = INVALID_HANDLE_VALUE;
			SetLastError(dwErr);
		}
		else if (bSumMode && g_threadsCount)
		{
			// close handle in case of multithreaded sum computation/verification.
			// worker threads will open the file again when processing the file
			CloseHandle(f);
		}
	}

	if (f != INVALID_HANDLE_VALUE)
	{
		if (bSumMode && g_threadsCount)
		{
			AddHashJob(filePath, fileSize.QuadPart, bQuiet, bShowProgress, bSumMode, bSumVerificationMode, pbExpectedDigest, pHashesToUse);
		}
		else
			ProcessFile(f, fileSize.QuadPart, szFilePath, bQuiet, bShowProgress, bSumMode, bSumVerificationMode, pbExpectedDigest , pHashesToUse, g_pbBuffer, sizeof (g_pbBuffer));
	}
	else
	{
		std::wstring szMsg = FormatString (_T("Failed to open file \"%s\" for reading (error 0x%.8X)\n"), szFilePath, GetLastError());
		if (outputFiles[0] && (!bSumMode || bSumVerificationMode)) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
		if (g_bSkipError)
		{
			if (!bQuiet)
			{
				if (g_threadsCount)
				{
					AddOutputEntry(new std::wstring(szMsg), NULL, bQuiet, true, true, 0);
				}
				else
					ShowErrorDirect(szMsg.c_str());
			}
			
			if (bSumMode) g_bMismatchFound = true;
			dwError = 0;
		}
		else
		{		
			g_szLastErrorMsg = szMsg;
			dwError = -1;
		}
	}

	return dwError;
}

DWORD HashDirectory(const CPath& dirPath, vector<shared_ptr<Hash>>& pHashes, bool bIncludeNames, bool bStripNames, bool bQuiet, bool bShowProgress, bool bSumMode, const map<wstring, HashResultEntry>& digestList)
{
	wstring szDir = dirPath.GetAbsolutPathValue();
	WIN32_FIND_DATA ffd;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError = 0;
	list<CDirContent> dirContent;
	LPCWSTR szDirPath = dirPath.GetPathValue().c_str();
	bool bSumVerificationMode = (bSumMode && !digestList.empty());

	if (IsExcludedName(szDirPath, false))
		return 0;


	szDir += _T("\\*");

	// Find the first file in the directory.

	hFind = FindFirstFile(szDir.c_str(), &ffd);

	if (INVALID_HANDLE_VALUE == hFind)
	{
		dwError = GetLastError();
		std::wstring szMsg = FormatString (_T("FindFirstFile failed on \"%s\" with error 0x%.8X.\n"), szDirPath, dwError);	
		if (outputFiles[0] && (!bSumMode || bSumVerificationMode)) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
		if (g_bSkipError)
		{
			if (!bQuiet)
			{
				if (g_threadsCount)
					AddOutputEntry(new std::wstring(szMsg), NULL, bQuiet, true, true, 0);
				else
					ShowErrorDirect(szMsg.c_str());
			}
			return 0;
		}
		else
		{
			g_szLastErrorMsg = szMsg;
			return dwError;
		}
	}

	// List all the files in the directory with some info about them.

	do
	{
		if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			// Skip "." and ".." directories
			if ((_tcscmp(ffd.cFileName, _T(".")) != 0) && (_tcscmp(ffd.cFileName, _T("..")) != 0))
			{
				CDirContent entry(dirPath, ffd.cFileName, true);
				if (!g_bNoFollow || !IsReparsePoint(entry.GetPath().GetAbsolutPathValue().c_str()))
					dirContent.push_back(entry);
			}
		}
		else
		{
			CDirContent entry(dirPath, ffd.cFileName, false);
			if (!g_bNoFollow || !IsReparsePoint(entry.GetPath().GetAbsolutPathValue().c_str()))
			{
				// skip file holding checksum
				if (bSumMode && !g_sumFileSkipped)
				{
					if (!digestList.empty())
					{
						// verification
						if (0 == _wcsicmp(g_verificationFileName.GetAbsolutPathValue().c_str(), entry.GetPath().GetAbsolutPathValue().c_str()))
						{
							g_sumFileSkipped = true;
							continue;
						}
					}
					else
					{
						if (g_outputFileName.GetAbsolutPathValue().empty())
							g_sumFileSkipped = true;
						else if (0 == _wcsicmp(g_outputFileName.GetAbsolutPathValue().c_str(), entry.GetPath().GetAbsolutPathValue().c_str()))
						{
							g_sumFileSkipped = true;
							continue;
						}
					}
				}
				dirContent.push_back(entry);
			}
		}
	}
	while (FindNextFile(hFind, &ffd) != 0);

	dwError = GetLastError();
	if (dwError != ERROR_NO_MORE_FILES)
	{
		std::wstring szMsg = FormatString (TEXT("FindNextFile failed while listing \"%s\". \n Error 0x%.8X.\n"), szDirPath, dwError);
		FindClose(hFind);
		
		if (outputFiles[0] && (!bSumMode || bSumVerificationMode)) _ftprintf(*outputFiles[0], L"%s", szMsg.c_str());
		if (g_bSkipError)
		{
			if (!bQuiet)
			{
				if (g_threadsCount)
					AddOutputEntry(new std::wstring(szMsg), NULL, bQuiet, true, true, 0);
				else
					ShowErrorDirect(szMsg.c_str());
			}
			return 0;
		}
		else
		{			
			g_szLastErrorMsg = szMsg;
			return dwError;
		}
	}

	// Clear the error
	dwError = 0;

	FindClose(hFind);

	// Sort all entries
	dirContent.sort(compare_nocase);

	if (bIncludeNames)
	{
		LPCTSTR pNameToHash = NULL;
		LPWSTR pCanonicalName = NULL;
		if (wcslen(szDirPath) > MAX_PATH)
		{
			if (PathAllocCanonicalizePtr)
			{
				if (S_OK == PathAllocCanonicalizePtr(szDirPath, PATHCCH_ALLOW_LONG_PATHS, &pCanonicalName))
					pNameToHash = pCanonicalName;
				else
					pNameToHash = szDirPath;
			}
			else
			{
				pNameToHash = szDirPath;
			}
			if (bStripNames)
				pNameToHash = GetFileName(pNameToHash);
		}
		else
		{
			g_szCanonalizedName[MAX_PATH] = 0;
			if (!PathCanonicalize(g_szCanonalizedName, szDirPath))
				lstrcpy(g_szCanonalizedName, szDirPath);

			if (bStripNames)
				pNameToHash = GetFileName(g_szCanonalizedName);
			else
				pNameToHash = g_szCanonalizedName;
		}

		UpdateHashes(pHashes,(LPCBYTE)pNameToHash, _tcslen(pNameToHash) * sizeof(TCHAR));
		if (pCanonicalName)
			LocalFree(pCanonicalName);
	}

	for (list<CDirContent>::iterator it = dirContent.begin(); it != dirContent.end(); it++)
	{
		if (it->IsDir())
		{
			dwError = HashDirectory(it->GetPath(), pHashes, bIncludeNames, bStripNames, bQuiet, bShowProgress, bSumMode, digestList);
			if (dwError)
				break;
		}
		else
		{
			dwError = HashFile(it->GetPath(), pHashes, bIncludeNames, bStripNames, bQuiet, bShowProgress, bSumMode, digestList);
			if (dwError)
				break;
		}
	}

	return dwError;
}

void ShowLogo()
{
	if (g_bNoLogo)
		return;

	SetConsoleTextAttribute(g_hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
	_tprintf(_T("\nDirHash ") _T(DIRHASH_VERSION) _T(" by Mounir IDRASSI (mounir@idrix.fr) Copyright 2010-2024\n\n"));
	_tprintf(_T("Recursively compute hash of a given directory content in lexicographical order.\nIt can also compute the hash of a single file.\n\n"));
	_tprintf(_T("Supported Algorithms :\n"));
	std::vector<std::wstring> algos = Hash::GetSupportedHashIds();
	for (size_t i = 0; i < algos.size(); i++)
		_tprintf(_T(" %s"), algos[i].c_str());
	_tprintf(_T("\n\n"));
	SetConsoleTextAttribute(g_hConsole, g_wCurrentAttributes);
}


void ShowUsage()
{
	ShowLogo();
	_tprintf(TEXT("Usage: \n")
		TEXT("  DirHash.exe DirectoryOrFilePath [HashAlgo] [-t ResultFileName] [-sum] [-sumRelativePath] [-includeLastDir] [-verify FileName] [-threads] [-clip] [-lowercase] [-overwrite]  [-quiet] [-nowait] [-hashnames] [-stripnames] [-skipError] [-nologo] [-nofollow] [-exclude pattern1] [-exclude pattern2]  [-only pattern1] [-only pattern2]\n")
		TEXT("  DirHash.exe -benchmark [HashAlgo | All] [-t ResultFileName] [-clip] [-overwrite]  [-quiet] [-nowait] [-nologo]\n")
		TEXT("\n")
		TEXT("  Possible values for HashAlgo (not case sensitive, default is Blake3):\n"));
	   _tprintf(_T(" "));
		std::vector<std::wstring> algos = Hash::GetSupportedHashIds();
		for (size_t i = 0; i < algos.size(); i++)
			_tprintf(_T(" %s"), algos[i].c_str());
		_tprintf(_T("\n")
		TEXT("Or any combinarion of the above values separated by comma, except when -verify is used\n")
		TEXT("\n\n")
		TEXT("  ResultFileName: text file where the result will be appended\n")
		TEXT("  -benchmark: perform speed benchmark of the selected algorithm. If \"All\" is specified, then all algorithms are benchmarked.\n")
		TEXT("  -sum: output hash of every file processed in a format similar to shasum.\n")
		TEXT("  -sumRelativePath (only when -sum is specified): the file paths are stored in the output file as relative to the input directory.\n")
		TEXT("  -verify: verify hash against value(s) present on the specified file.\n")
		TEXT("           argument must be either a checksum file or a result file.\n")
		TEXT("  -includeLastDir (only when -sum or -verify is specified): the last directory name of the input directory is included in the SUM file entries and used in the verification process. This switch implies -sumRelativePath.\n")
		TEXT("  -threads (only when -sum or -verify specified): multithreading will be used to accelerate hashing of files.\n")
		TEXT("  -clip: copy the result to Windows clipboard (ignored when -sum specified)\n")
		TEXT("  -lowercase: output hash value(s) in lower case instead of upper case\n")
		TEXT("  -progress: Display information about the progress of hash operation\n")
		TEXT("  -overwrite (only when -t present): output text file will be overwritten\n")
		TEXT("  -quiet: No text is displayed or written except the hash value\n")
		TEXT("  -nowait: avoid displaying the waiting prompt before exiting\n")
		TEXT("  -hashnames: case sensitive path of the files/directories will be included in the hash computation\n")
		TEXT("  -stripnames (only when -hashnames present): only last path portion of files/directories is used for hash computation\n")
		TEXT("  -exclude (cannot be combined with -only): specifies a name pattern for files to exclude from hash computation.\n")
		TEXT("  -only (cannot be combined with -exclude): only files matching the pattern are included in hash computation.\n")
		TEXT("  -skipError: ignore any encountered errors and continue processing.\n")
		TEXT("  -nologo: don't display the copyright message and version number on startup.\n")
		TEXT("  -nofollow: don't follow symbolic links, Junction points and mount points, excluding them from hash computation.\n")
	);
	_tprintf(_T("\n"));
}

void WaitForExit(bool bDontWait = false)
{
	if (!bDontWait)
	{
		_tprintf(_T("\n\nPress ENTER to exit the program ..."));
		getchar();
	}
}

void CopyToClipboard(LPCTSTR szDigestHex)
{
	if (OpenClipboard(NULL))
	{
		size_t cch = _tcslen(szDigestHex);

		HGLOBAL hglbCopy = GlobalAlloc(GMEM_MOVEABLE,
			(cch + 1) * sizeof(TCHAR));
		if (hglbCopy)
		{
			EmptyClipboard();

			// Lock the handle and copy the text to the buffer. 
			LPVOID lptstrCopy = GlobalLock(hglbCopy);
			if (lptstrCopy)
			{
				memcpy(lptstrCopy, (const TCHAR*)szDigestHex,
					(cch * sizeof(TCHAR)) + 1);
				GlobalUnlock(hglbCopy);

				// Place the handle on the clipboard. 
#ifdef _UNICODE
				SetClipboardData(CF_UNICODETEXT, hglbCopy);
#else
				SetClipboardData(CF_TEXT, hglbCopy);
#endif
			}
		}

		CloseClipboard();
	}
}

void BenchmarkAlgo(LPCTSTR hashAlgo, bool bQuiet, bool bCopyToClipboard, std::wstring* outputText)
{
#define BENCH_BUFFER_SIZE 50 * 1024 * 1024
#define BENCH_LOOPS 50
	unsigned char* pbData = new unsigned char[BENCH_BUFFER_SIZE];
	unsigned char pbDigest[64];

	if (pbData)
	{
		size_t i;
		clock_t t1, t2;
		Hash* pHash = Hash::GetHash(hashAlgo);

		t1 = clock();
		for (i = 0; i < BENCH_LOOPS; i++)
		{
			pHash->Update(pbData, BENCH_BUFFER_SIZE);
			pHash->Final(pbDigest);
			pHash->Init();
		}
		t2 = clock();

		double speed = ((double)BENCH_BUFFER_SIZE * (double)BENCH_LOOPS) / (((double)t2 - (double)t1) / (double)CLOCKS_PER_SEC);
		if (speed >= (double)(1024 * 1024 * 1024))
			StringCbPrintf((TCHAR*)pbData, BENCH_BUFFER_SIZE, _T("%s speed = %f GiB/s"), hashAlgo, (speed / (double)(1024 * 1024 * 1024)));
		else if (speed >= (double)(1024 * 1024))
			StringCbPrintf((TCHAR*)pbData, BENCH_BUFFER_SIZE, _T("%s speed = %f MiB/s"), hashAlgo, (speed / (double)(1024 * 1024)));
		else if (speed >= (double)(1024))
			StringCbPrintf((TCHAR*)pbData, BENCH_BUFFER_SIZE, _T("%s speed = %f KiB/s"), hashAlgo, (speed / (double)(1024)));
		else
			StringCbPrintf((TCHAR*)pbData, BENCH_BUFFER_SIZE, _T("%s speed = %f B/s"), hashAlgo, speed);

		// display hash in yellow
		SetConsoleTextAttribute(g_hConsole, FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY);

		if (!bQuiet) _tprintf(_T("%s\n"), (TCHAR*)pbData);
		if (outputFiles[0]) _ftprintf(*outputFiles[0], _T("%s\n"), (TCHAR*)pbData);

		if (bCopyToClipboard)
			CopyToClipboard((TCHAR*)pbData);

		if (outputText)
		{
			*outputText += (TCHAR*)pbData;
			*outputText += _T("\n");
		}

		// restore normal text color
		SetConsoleTextAttribute(g_hConsole, g_wCurrentAttributes);

		delete pHash;
		delete[] pbData;
	}
	else
	{
		if (!bQuiet) ShowError(_T("Failed to allocate memory for %s benchmark.\n"), hashAlgo);
		if (outputFiles[0]) _ftprintf(*outputFiles[0], _T("%s\n"), (TCHAR*)pbData);
	}
}

void PerformBenchmark(const vector<shared_ptr<Hash>>& pHashes, bool bQuiet, bool bCopyToClipboard)
{
	std::wstring outputText = L"";
	std::wstring* pOutputText = bCopyToClipboard ? &outputText : NULL;
	std::vector<std::wstring> hashList;
	if (pHashes.empty())
		hashList = Hash::GetSupportedHashIds();
	else
	{
		for (std::vector<shared_ptr<Hash>>::const_iterator It = pHashes.begin(); It != pHashes.end(); It++)
			hashList.push_back((*It)->GetID());
	}
	for (std::vector<std::wstring>::const_iterator It = hashList.begin(); It != hashList.end(); It++)
	{
		BenchmarkAlgo(It->c_str(), bQuiet, false, pOutputText);
	}

	if (bCopyToClipboard)
		CopyToClipboard(outputText.c_str());
}

// structure used to hold value from DirHash.ini
typedef struct
{
	wstring hashAlgoToUse;
	bool bQuiet;
	bool bDontWait;
	bool bShowProgress;
	bool bCopyToClipboard;
	bool bIncludeNames;
	bool bStripNames;
	bool bLowerCase;
	bool bSkipError;
	bool bNoLogo;
	bool bNoFollow;
	bool bForceSumMode;
	bool bUseThreads;
	bool bSumRelativePath;
	bool bIncludeLastDir;
} ConfigParams;

void LoadDefaults(ConfigParams& iniParams)
{
	iniParams.hashAlgoToUse = L"Blake3";
	iniParams.bDontWait = false;
	iniParams.bIncludeNames = false;
	iniParams.bStripNames = false;
	iniParams.bQuiet = false;
	iniParams.bCopyToClipboard = false;
	iniParams.bShowProgress = false;
	iniParams.bLowerCase = false;
	iniParams.bSkipError = false;
	iniParams.bNoLogo = false;
	iniParams.bNoFollow = false;
	iniParams.bForceSumMode = false;
	iniParams.bUseThreads = false;
	iniParams.bSumRelativePath = false;
	iniParams.bIncludeLastDir = false;

	// get values from DirHash.ini fille if it exists
	WCHAR szInitPath[1024];
	szInitPath[0] = 0;
	if (GetModuleFileName(NULL, szInitPath, ARRAYSIZE(szInitPath)))
	{
		wchar_t* ptr = wcsrchr (szInitPath, L'\\');
		if (ptr)
		{
			ptr += 1;
			*ptr = 0;
			StringCbCatW(szInitPath, sizeof(szInitPath), L"DirHash.ini");

			WCHAR szValue[128];
			if (GetPrivateProfileStringW(L"Defaults", L"Hash", L"Blake3", szValue, ARRAYSIZE(szValue), szInitPath) && Hash::IsHashId (szValue))
			{
				iniParams.hashAlgoToUse = szValue;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"Quiet", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bQuiet = true;
				else
					iniParams.bQuiet = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"NoWait", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bDontWait = true;
				else
					iniParams.bDontWait = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"ShowProgress", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bShowProgress = true;
				else
					iniParams.bShowProgress = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"hashnames", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bIncludeNames = true;
				else
					iniParams.bIncludeNames = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"stripnames", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bStripNames = true;
				else
					iniParams.bStripNames = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"clip", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bCopyToClipboard = true;
				else
					iniParams.bCopyToClipboard = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"lowercase", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bLowerCase = true;
				else
					iniParams.bLowerCase = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"SkipError", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bSkipError = true;
				else
					iniParams.bSkipError = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"NoLogo", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bNoLogo = true;
				else
					iniParams.bNoLogo = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"NoFollow", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bNoFollow = true;
				else
					iniParams.bNoFollow = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"Sum", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bForceSumMode = true;
				else
					iniParams.bForceSumMode = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"Threads", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bUseThreads = true;
				else
					iniParams.bUseThreads = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"SumRelativePath", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") == 0)
					iniParams.bSumRelativePath = true;
				else
					iniParams.bSumRelativePath = false;
			}

			if (GetPrivateProfileStringW(L"Defaults", L"IncludeLastDir", L"False", szValue, ARRAYSIZE(szValue), szInitPath))
			{
				if (_wcsicmp(szValue, L"True") != 0)
					iniParams.bIncludeLastDir = false;
				else
					iniParams.bIncludeLastDir = true;
			}
		}
	}
}

bool ParseResultLine(wchar_t* szLine, wstring& targetName, wstring& hashName, ByteArray& digestValue)
{
	bool bRet = false;

	if (szLine && wcslen (szLine) >= 32 ) // minimum length of line is 32 characters
	{
		// try first to decode as raw hash value
		if (FromHex(szLine, digestValue))
		{
			bRet = Hash::IsHashSize((int)digestValue.size());
		}
		else
		{
			// parse hash name, folder/file name and digest value
			// format: hashName hash of "XXXX" (DD bytes) = XXXXXX...XX
			wchar_t* ptr = wcschr(szLine, L' ');
			if (ptr)
			{
				*ptr = 0;
				// check that first part is a valid hash name
				shared_ptr<Hash> pHash(Hash::GetHash(szLine));
				if (pHash)
				{
					hashName = pHash->GetID();
					ptr++;
					if ((wcslen(ptr) > 32) && wcsstr(ptr, L"hash of \"") == ptr)
					{
						ptr += 9; // 9 is length of "hash of \""
						wchar_t* delim = wcschr(ptr, L'\"');
						if (delim)
						{
							*delim = 0;
							targetName = ptr;

							if (delim[1] == L' ' && delim[2] == L'(')
							{
								ptr = delim + 3;
								if (wcslen(ptr) > 32 && (delim = wcschr(ptr, L' ')))
								{
									// get encoded digest size (always 2 characters)
									int hashLen = 0;
									*delim = 0;
									if (wcslen(ptr) == 2 && (1 == swscanf(ptr, L"%2d", &hashLen)) && (hashLen == pHash->GetHashSize()))
									{
										ptr = delim + 1;
										if ((wcslen(ptr) > 32) && wcsstr(ptr, L"bytes) = ") == ptr)
										{
											// look for digest
											ptr += 9; // 9 is length of "bytes) = "

											if (wcslen(ptr) == ((size_t) 2 * (size_t) hashLen)) // hexadecimal encoding is double the size in bytes
											{
												if (FromHex(ptr, digestValue))
												{
													bRet = true;
												}
											}
										}
									}
								}
							}						
						}
					}
				}
			}

		}
	}

	if (!bRet)
	{
		targetName = L"";
		hashName = L"";
		digestValue.clear();
	}

	return bRet;
}

bool ParseResultFile(const CPath& resultFile, map<wstring, HashResultEntry>& pathDigestList, map<int, ByteArray>& rawDigestList)
{
	bool bRet = false;
	// Result files are created using UTF-8 encoding in order to support UNICODE file names
	FILE* f = _wfopen(resultFile.GetAbsolutPathValue().c_str(), L"rt,ccs=UTF-8");
	if (f)
	{
		bool bFailed = false;
		ByteArray buffer(4096 * 2);
		wchar_t* szLine = (wchar_t*)buffer.data();
		size_t digestLen = 0;
		wstring targetName, hashName;
		ByteArray digestValue;

		pathDigestList.clear();
		rawDigestList.clear();

		while (fgetws(szLine, (int)(buffer.size() / 2), f))
		{
			size_t l = wcslen(szLine);
			if (szLine[l - 1] == L'\n')
			{
				szLine[l - 1] = 0;
				l--;
			}

			if (l == 0)
				continue;

			if (ParseResultLine(szLine, targetName, hashName, digestValue))
			{
				if (targetName.length() && hashName.length())
				{
					pathDigestList[targetName].m_hashName = hashName;
					pathDigestList[targetName].m_digest = digestValue;
				}
				else
					rawDigestList[(int)digestValue.size()] = digestValue;
			}
			else
			{
				bFailed = true;
				break;
			}
		}
		fclose(f);

		if (bFailed)
		{
			pathDigestList.clear();
			rawDigestList.clear();
		}
		else if (pathDigestList.size() == 0 && rawDigestList.size() == 0)
		{
		}
		else
			bRet = true;
	}
	else
	{
		_tprintf(TEXT("Failed to open file \"%s\" for reading\n"), resultFile.GetPathValue().c_str());
	}

	return bRet;
}

bool ParseSumFile(const CPath& sumFile, map<wstring, HashResultEntry>& digestList, vector<int>& skippedLines, bool normalizePath = true)
{
	bool bRet = false;	
	// SUM files are created using UTF-8 encoding in order to support UNICODE file names
	FILE* f = _wfopen(sumFile.GetAbsolutPathValue().c_str(), L"rt,ccs=UTF-8");
	if (f)
	{
		bool bFailed = false;
		ByteArray buffer(4096 * 2);
		wchar_t* szLine = (wchar_t*)buffer.data();
		size_t digestLen = 0;
		int lineNumber = 0;
		
		digestList.clear();
		skippedLines.clear();

		while (fgetws(szLine, (int)(buffer.size() / 2), f))
		{
			size_t l = wcslen(szLine);
			if (szLine[l - 1] == L'\n')
			{
				szLine[l - 1] = 0;
				l--;
			}

			lineNumber++;

			if (l == 0)
				continue;

			bFailed = true;

			// extract hash which is followed by two or one space characters
			wchar_t* ptr = wcschr(szLine, L' ');
			if (ptr)
			{
				*ptr = 0;
				ptr++;
				// look for begining of file path
				while (ptr != &szLine[l - 1] && *ptr == L' ')
					ptr++;
				// remove '*' if present (this is for unix checksum compatibility)
				if (ptr != &szLine[l - 1] && *ptr == L'*')
					ptr++;
				if (ptr != &szLine[l - 1])
				{
					// hash length must be one of the supported ones (16, 20, 32, 48, 64)
					ByteArray digest;
					if (FromHex(szLine, digest))
					{
						if ((digestLen != 0 && digestLen == digest.size())
							|| (digestLen == 0 && Hash::IsHashSize ((int) digest.size()))
							)
						{
							wstring entryName = ptr;
							// replace '/' by '\' for compatibility with checksum format on *nix platforms
							std::replace(entryName.begin(), entryName.end(), L'/', L'\\');

							// check that entreName starts by the input directory value. Otherwise add it.
							if ( normalizePath && g_inputDirPathLength && ((entryName.length() < g_inputDirPathLength)
								||	(_wcsicmp(g_inputDirPath.c_str(), entryName.substr(0, g_inputDirPathLength).c_str())))
								)
							{
								entryName = g_inputDirPath + entryName;
							}

							digestLen = digest.size();
							digestList[entryName].m_digest = digest;
							bFailed = false;
						}
					}
				}
			}

			if (bFailed)
			{
				if (lineNumber > 1)
				{
					skippedLines.push_back(lineNumber);
					bFailed = false;
				}
				else
					break;
			}
		}
		fclose(f);

		if (bFailed)
		{
			digestList.clear();
		}
		else if (digestList.size() == 0)
		{
		}
		else
			bRet = true;
	}
	else
	{
	_tprintf(TEXT("Failed to open file \"%s\" for reading\n"), sumFile.GetPathValue().c_str());
	}

	return bRet;
}

// Helper function to count the depth of directories in a path
long long countPathDepth(const wstring& path) {
	return std::count(path.begin(), path.end(), L'\\');
}

bool SortSumFile(const CPath& sumFile, FILE* fTarget)
{
	map<wstring, HashResultEntry> digestList;
	vector<int> skippedLines;
	bool bRet = false;

	// Parse the existing SHASUM file
	if (ParseSumFile(sumFile, digestList, skippedLines, false))
	{
		// Convert map to vector for sorting
		vector<pair<wstring, HashResultEntry>> sortedEntries(digestList.begin(), digestList.end());

		// Sort the entries by directory depth first, then lexicographically
		sort(sortedEntries.begin(), sortedEntries.end(),
			[](const auto& a, const auto& b) {
				long long depthA = countPathDepth(a.first);
				long long depthB = countPathDepth(b.first);
				if (depthA != depthB) {
					return depthA > depthB; // // Entries with more directories come first
				}
				return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; // Lexicographical order as secondary criterion
			});

		// Write the sorted entries to the file
		bool openedNewFile = false;
		if (!fTarget)
		{
			fTarget = _wfopen(sumFile.GetAbsolutPathValue().c_str(), L"wt,ccs=UTF-8");
			openedNewFile = true;
		}

		if (fTarget)
		{
			WCHAR szDigestHex[129]; // Enough for 64 bytes digest
			for (const auto& entry : sortedEntries)
			{
				wstring szLine;
				ToHex(entry.second.m_digest, szDigestHex);
				szLine = szDigestHex;
				szLine += L"  ";
				szLine += entry.first;
				szLine += L"\n";
				_ftprintf(fTarget, L"%s", szLine.c_str());
			}

			if (openedNewFile)
				fclose(fTarget);

			bRet = true;
		}
	}

	return bRet;
}


BOOL WINAPI CtrlHandler(DWORD fdwCtrlType)
{
	switch (fdwCtrlType)
	{
		// Handle the CTRL-C signal.
	case CTRL_C_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_BREAK_EVENT:
		// notify thread to stop but don't wait for them
		if (g_threadsCount)
		{
			g_bFatalError = true;
			g_bStopThreads = true;
			g_bStopOutputThread = true;
			SetEvent(g_hStopEvent);
			SetEvent(g_hOutputStopEvent);

			FreejobList(); // we close all already opened handles to avoid leaving too many opened handle
			FreeOutputList();
		}
		// restore orginal console attributes
		g_wCurrentAttributes = g_wAttributes;
		SetConsoleTextAttribute(g_hConsole, g_wAttributes);
		return FALSE;

	default:
		return FALSE;
	}
}

bool GetPathType(LPCWSTR szPath, bool& bIsFile)
{
	struct _stat64 buf;
	std::wstring drivePath;
	bIsFile = false;

	if ((0 == _wstat64(szPath, &buf)) && (buf.st_mode & (_S_IFDIR | _S_IFREG)))
	{
		bIsFile = (buf.st_mode & _S_IFREG) ? true : false;
		return true;
	}
	else
		return false;
}

wstring GetCurDir()
{
	wstring ret;
	DWORD cchCurDir;
	LPWSTR wszCurDir = NULL;
	cchCurDir = GetCurrentDirectoryW(0, NULL);
	wszCurDir = new WCHAR[cchCurDir];
	GetCurrentDirectoryW(cchCurDir, wszCurDir);
	ret = wszCurDir;
	ret += L"\\";
	delete[] wszCurDir;
	return ret;
}

bool IsWindowsLongPathNamesEnabled()
{
	// Registry key HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled (Type: REG_DWORD) 
	// must exist and be set to 1.
	bool bRet = false;
	HKEY hKey;
	LSTATUS lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", 0, KEY_READ, &hKey);
	if (ERROR_SUCCESS == lRet)
	{
		DWORD dwType, dwValue, cbLen = sizeof(DWORD);
		lRet = RegQueryValueExW(hKey, L"LongPathsEnabled", NULL, &dwType, (LPBYTE) &dwValue, &cbLen);
		if (ERROR_SUCCESS == lRet)
		{
			bRet = (dwValue == 1);
		}
		RegCloseKey(hKey);
	}
	return bRet;

}

bool ValidateHashesVector(vector<shared_ptr<Hash>>& pHashes)
{
	bool bRet = true;
	for (auto it = pHashes.begin(); it != pHashes.end(); it++)
	{
		if (!(*it)->IsValid())
		{
			bRet = false;
			break;
		}
	}
	return bRet;
}

int _tmain(int argc, _TCHAR* argv[])
{
	HANDLE hFind = INVALID_HANDLE_VALUE;
	DWORD dwError = 0;
	vector<shared_ptr<Hash>> pHashes;
	bool bDontWait = false;
	bool bIncludeNames = false;
	bool bStripNames = false;
	bool bQuiet = false;
	bool bOverwrite = false;
	bool bCopyToClipboard = false;
	bool bShowProgress = false;
	bool bSumMode = false;
	bool bVerifyMode = false;
	wstring hashAlgoToUse = L"Blake3";
	bool bBenchmarkOp = false;
	map < wstring, HashResultEntry> digestsList;
	map < int, ByteArray> rawDigestsList;
	vector < int > skippedLines;
	ByteArray verifyDigest;
	bool bBenchmarkAllAlgos = false;
	CConsoleUnicodeOutputInitializer conUnicode;
	bool bUseThreads = false;
	bool bIsFile = false;
	bool bForceSumMode = false;
	bool onlySpecified = false;
	bool excludeSpecified = false;
	OSVERSIONINFOW versionInfo;
	wstring inputArg;
	ConfigParams iniParams;
	CPath inputPath;

	if (GetWindowsVersion(&versionInfo) && (versionInfo.dwMajorVersion >= 10))
	{
		PathAllocCanonicalizePtr = (PathAllocCanonicalizeFn)GetProcAddress(GetModuleHandle(L"KernelBase.dll"), "PathAllocCanonicalize");
		PathAllocCombinePtr = (PathAllocCombineFn)GetProcAddress(GetModuleHandle(L"KernelBase.dll"), "PathAllocCombine");
		PathCchSkipRootPtr = (PathCchSkipRootFn)GetProcAddress(GetModuleHandle(L"KernelBase.dll"), "PathCchSkipRoot");

		// Long path names support is available starting from Windows 10 version 1607 (Build 14393)
		if (versionInfo.dwBuildNumber >= 14393)
		{
			g_bLongPathNamesEnabled = IsWindowsLongPathNamesEnabled();
		}
	}

	g_hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

	// get original console attributes
	if (GetConsoleScreenBufferInfo(g_hConsole, &g_originalConsoleInfo))
		g_wAttributes = g_originalConsoleInfo.wAttributes;

	g_wCurrentAttributes = g_wAttributes;

	setbuf(stdout, NULL);

	SetConsoleTitle(_T("DirHash by Mounir IDRASSI (mounir@idrix.fr) Copyright 2010-2024"));

	SetConsoleCtrlHandler(CtrlHandler, TRUE);

	// store the current directory
	g_currentDirectory = GetCurDir();

	if (argc < 2)
	{
		ShowUsage();
		WaitForExit();
		return 1;
	}

	LoadDefaults(iniParams);
	hashAlgoToUse = iniParams.hashAlgoToUse;
	bQuiet = iniParams.bQuiet;
	bDontWait = iniParams.bDontWait;
	bShowProgress = iniParams.bShowProgress;
	bCopyToClipboard = iniParams.bCopyToClipboard;
	bIncludeNames = iniParams.bIncludeNames;
	bStripNames = iniParams.bStripNames;
	g_bLowerCase = iniParams.bLowerCase;
	g_bSkipError = iniParams.bSkipError;
	g_bNoLogo = iniParams.bNoLogo;
	g_bNoFollow = iniParams.bNoFollow;
	bForceSumMode =  iniParams.bForceSumMode;
	bUseThreads = iniParams.bUseThreads;
	g_bSumRelativePath = iniParams.bSumRelativePath;
	g_bIncludeLastDir = iniParams.bIncludeLastDir;

	if (_tcscmp(argv[1], _T("-benchmark")) == 0)
		bBenchmarkOp = true;

	if (argc >= 3)
	{
		for (int i = 2; i < argc; i++)
		{
			if (_tcscmp(argv[i], _T("-t")) == 0)
			{
				if ((i + 1) >= argc)
				{
					// missing file argument               
					ShowUsage();
					ShowError(_T("Error: Missing argument for switch -t\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				g_outputFileName = argv[i + 1];

				i++;
			}
			else if (_tcscmp(argv[i], _T("-overwrite")) == 0)
			{
				bOverwrite = true;
			}
			else if (_tcscmp(argv[i], _T("-nowait")) == 0)
			{
				bDontWait = true;
			}
			else if (_tcscmp(argv[i], _T("-quiet")) == 0)
			{
				bQuiet = true;
			}
			else if (_tcscmp(argv[i], _T("-hashnames")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -hashnames can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				bIncludeNames = true;
			}
			else if (_tcscmp(argv[i], _T("-stripnames")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -stripnames can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				bStripNames = true;
			}
			else if (_tcscmp(argv[i], _T("-sum")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -sum can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				if (bVerifyMode)
				{
					ShowUsage();
					ShowError(_T("Error: -sum can not be combined with -verify\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				bSumMode = true;
			}
			else if (_tcscmp(argv[i], _T("-verify")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -verify can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				
				if (bSumMode)
				{
					ShowUsage();
					ShowError(_T("Error: -verify can not be combined with -sum\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				if ((i + 1) >= argc)
				{
					// missing file argument               
					ShowUsage();
					ShowError(_T("Error: Missing argument for switch -verify\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				bVerifyMode = true;

				g_verificationFileName = argv[i + 1];
				i++;
			}
			else if (_tcscmp(argv[i], _T("-exclude")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -exclude can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				if (onlySpecified) {
					ShowUsage();
					ShowError(_T("Error: -only and -exclude cannot be specified at the same time\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				if ((i + 1) >= argc)
				{
					// missing file argument               
					ShowUsage();
					ShowError(_T("Error: Missing argument for switch -exclude\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				excludeSpecified = true;
				excludeSpecList.push_back(argv[i + 1]);

				i++;
			}
			else if (_tcscmp(argv[i], _T("-only")) == 0) {
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -only can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				if (excludeSpecified) {
					ShowUsage();
					ShowError(_T("Error: -only and -exclude cannot be specified at the same time\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				if ((i + 1) >= argc) {
					// missing file argument
					ShowUsage();
					ShowError(_T("Error: Missing argument for switch -only\n"));
					WaitForExit(bDontWait);
					return 1;
				}

				onlySpecified = true;
				onlySpecList.push_back(argv[i + 1]);
				i++;
			}
			else if (_tcscmp(argv[i], _T("-clip")) == 0)
			{
				bCopyToClipboard = true;
			}
			else if (_tcscmp(argv[i], _T("-progress")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -progress can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				bShowProgress = true;
			}
			else if (_tcscmp(argv[i], _T("-lowercase")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -lowercase can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				g_bLowerCase = true;
			}
			else if (_tcscmp(argv[i], _T("-skipError")) == 0)
			{
				if (bBenchmarkOp)
				{
					ShowUsage();
					ShowError(_T("Error: -skipError can not be combined with -benchmark\n"));
					WaitForExit(bDontWait);
					return 1;
				}
				g_bSkipError = true;
			}
			else if (_tcsicmp(argv[i], _T("-nologo")) == 0)
			{
				g_bNoLogo = true;
			}
			else if (_tcsicmp(argv[i], _T("-nofollow")) == 0)
			{
				g_bNoFollow = true;
			}
			else if (Hash::IsHashIdCombination(argv[i]))
			{
				hashAlgoToUse = argv[i];
			}
			else if (bBenchmarkOp && (0 == _tcsicmp(argv[i], _T("All"))))
			{
				bBenchmarkAllAlgos = true;
			}
			else if (_tcsicmp(argv[i], _T("-threads")) == 0)
			{
				bUseThreads = true;
			}
			else if (_tcsicmp(argv[i], _T("-sumRelativePath")) == 0)
			{
				g_bSumRelativePath = true;
			}
			else if (_tcsicmp(argv[i], _T("-includeLastDir")) == 0)
			{
				g_bIncludeLastDir = true;
				g_bSumRelativePath = true;
			}
			else
			{
				ShowUsage();
				ShowError(_T("Error: Argument \"%s\" not recognized\n"), argv[i]);
				WaitForExit(bDontWait);
				return 1;
			}
		}
	}

	if (!bBenchmarkAllAlgos)
	{
		pHashes = Hash::GetHashes(hashAlgoToUse.c_str());
		if (pHashes.empty()|| !ValidateHashesVector(pHashes))
		{
			ShowError(_T("Error: Failed to initialize the hash algorithm(s) \"%s\"\n"), hashAlgoToUse.c_str());
			WaitForExit(bDontWait);
			return 1;
		}
	}

	if (!bQuiet)
		ShowLogo();

	// in case "-verify" was not specified, set SUM mode if it was specied in DirHash.ini
	if (!bVerifyMode && bForceSumMode)
		bSumMode = true;

	// we don't support multiple hash algorithms in verify mode
	if (bVerifyMode && pHashes.size() > 1)
	{
		if (!bQuiet)
			ShowError(TEXT("Error: -verify can not be combined with multiple hash algorithms\n"));
		WaitForExit(bDontWait);
		return (-10);
	}

	if (!g_outputFileName.GetPathValue().empty())
	{
		// in case of sum mode and if there are multiple hash algorithms specified, we need to create a separate file for each hash algorithm
		// the file name will be the same as the output file name, but with the hash algorithm appended		
		bool bMultiHashMode = bSumMode && pHashes.size() > 1;
		bool bSumComputation = bSumMode && !bVerifyMode;
		for (size_t i = 0; i < pHashes.size(); i++)
		{
			// create a new file name by appending the hash algorithm name
			std::wstring newFileName = g_outputFileName.GetAbsolutPathValue();
			std::wstring shadowFileName;
			if (bMultiHashMode)
			{
				newFileName += _T(".");
				newFileName += pHashes[i]->GetID();
			}
			// open the file
			FILE* newFile = _tfopen(newFileName.c_str(), bOverwrite ? _T("wt,ccs=UTF-8") : _T("a+t,ccs=UTF-8"));
			if (!newFile)
			{
				if (!bQuiet)
				{
					ShowError(_T("!!!Failed to open the %s SUM file for writing!!!\n"), pHashes[i]->GetID());
				}
			}
			else if (!bOverwrite)
			{
				// add a new Line to the file to avoid issues with existing content
				__int64 fileLength = _filelengthi64(_fileno(newFile));
				if (fileLength > 3) // ignore UTF-8 BOM bytes which are always written by fopen when "ccs=UTF-8" specified
					_ftprintf(newFile, L"\n");
			}

			FILE* shadowFile = NULL;
			if (bSumComputation && bUseThreads && !bOverwrite)
			{
				// create a shadow file for the current sum file. This file will be used to store the hash values.
				// when all computations are done, we will first sort the hash values and then write the sorted values to the target sum file
				// this is done to avoid issues with the order of hash values in the sum file when using threads
				shadowFileName = newFileName + L".dirhash_shadow";
				shadowFile = _tfopen(shadowFileName.c_str(), _T("wt,ccs=UTF-8"));
			}

			if (newFile)
				// add the file to the list of output files
				outputFiles.push_back(shared_ptr<CFilePtr>(new CFilePtr(newFile, newFileName, shadowFile, shadowFileName)));
			else
				outputFiles.push_back(NULL);
			
			if (!bSumMode)
				break;
		}
	}
	else
	{
		// no output file specified, add NULL to the list of output files
		outputFiles.push_back(NULL);
	}

	if (bBenchmarkOp)
	{
		PerformBenchmark(pHashes, bQuiet, bCopyToClipboard);

		WaitForExit(bDontWait);
		return dwError;
	}

	inputArg = argv[1];
	std::replace(inputArg.begin(), inputArg.end(), L'/', L'\\');
	// remove any trailing backslash to harmonize directory names in case they are included
	// in hash computations
	size_t inputArgLen = wcslen(inputArg.c_str());
	if (inputArg[inputArgLen - 1] == L'\\')
		inputArg.erase(inputArgLen - 1, 1);
	inputPath = inputArg.c_str();

	if (!GetPathType(inputPath.GetAbsolutPathValue().c_str(), bIsFile))
	{
		if (!bQuiet)
			ShowError(TEXT("Error: The given input file doesn't exist\n"));
		WaitForExit(bDontWait);
		return (-2);
	}

	if (g_bNoFollow && IsReparsePoint(argv[1]))
	{
		if (!bQuiet)
			ShowError(TEXT("Error: -nofollow specified but the given input file or directoty is Symbolic Link, Junction Point or Mount Point.\n"));
		WaitForExit(bDontWait);
		return (-9);
	}

	if (!bQuiet)
	{
		_tprintf(_T("Using %s to %s %s of \"%s\" ...\n"),
			hashAlgoToUse.c_str(),
			bVerifyMode? _T("verify") : _T("compute"),
			bSumMode ? _T("checksum") : _T("hash"),
			bStripNames ? GetFileName(argv[1]) : argv[1]);
		fflush(stdout);
	}

	if (!bIsFile)
	{

		if (bSumMode || bVerifyMode)
		{
			// we store the input directory when -sum or -verify are specified
			g_inputDirPath = inputArg;
			if (g_bIncludeLastDir)
			{
				// remove the last directory name so that it is present in the output
				size_t pos = g_inputDirPath.find_last_of(L"\\");
				if (pos != std::wstring::npos)
					g_inputDirPath.erase(pos + 1);
				else
					g_inputDirPath.clear();
			}
			else
				g_inputDirPath += L"\\";
			g_inputDirPathLength = wcslen(g_inputDirPath.c_str());
		}
	}

	if (bVerifyMode)
	{

		if (ParseSumFile(g_verificationFileName, digestsList, skippedLines))
		{
			// check that hash length used in the checksum file is the same as the one specified by the user
			int sumFileHashLen = (int)digestsList.begin()->second.m_digest.size();
			if (sumFileHashLen != pHashes[0]->GetHashSize())
			{
				if (!bQuiet)
					ShowError(TEXT("Error: hash length parsed from checksum file (%d bytes) is different from used hash length (%d bytes).\n"), sumFileHashLen, pHashes[0]->GetHashSize());
				WaitForExit(bDontWait);
				return (-4);
			}
			if (bIsFile)
			{
				// if input is a file, we need to remove all entries from digestsList except the one corresponding to the input file
				// this is because we only want to verify the hash of the input file, not all files in the sum file
				// keep only the entry corresponding to the input file
				wstring inputFileName = inputPath.GetPathValue();
				auto it = digestsList.find(inputFileName);
				if (it != digestsList.end())
				{
					// found the entry, keep it and remove all other entries
					HashResultEntry entry = it->second;
					digestsList.clear();
					digestsList[inputFileName] = entry;
				}
				else
				{
					// entry not found, this is an error
					if (!bQuiet)
						ShowError(L"Error: file \"%s\" not found in checksum file.\n", inputFileName.c_str());
					WaitForExit(bDontWait);
					return -5;
				}
			}
			bSumMode = true;
		}
		else if (ParseResultFile(g_verificationFileName, digestsList, rawDigestsList))
		{
			// 
			std::wstring entryName = GetFileName(argv[1]);
			map < wstring, HashResultEntry>::iterator It = digestsList.find(entryName);
			if (It == digestsList.end())
			{
				map<int, ByteArray>::iterator ItRaw = rawDigestsList.find(pHashes[0]->GetHashSize());
				if (ItRaw == rawDigestsList.end())
				{
					if (!bQuiet)
						ShowError(TEXT("Error: Failed to find a valid entry for \"%s\" in the result file\n"), entryName.c_str());
					WaitForExit(bDontWait);
					return (-8);
				}
				else
					verifyDigest = ItRaw->second;
			}
			else
				verifyDigest = It->second.m_digest;

			if (verifyDigest.size() != (size_t) pHashes[0]->GetHashSize())
			{
				if (!bQuiet)
					ShowError(TEXT("Error: hash length parsed from result file (%d bytes) is different from used hash length (%d bytes).\n"), (int) verifyDigest.size(), pHashes[0]->GetHashSize());
				WaitForExit(bDontWait);
				return (-4);
			}
		}
		else
		{
			if (!bQuiet)
				ShowError(TEXT("Error: Failed to parse file \"%s\". Please check that it exists and that its content is valid (either checksum file or result file).\n"), g_verificationFileName.GetPathValue().c_str());
			WaitForExit(bDontWait);
			return (-3);
		}
	}

	if (bSumMode)
	{
		// set default text color to yellow
		g_wCurrentAttributes = FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY;
		SetConsoleTextAttribute(g_hConsole, g_wCurrentAttributes);		

		if (bUseThreads)
		{
			bool bOutfileValid = false;
			// check that at least one of the output files is valid
			for (size_t i = 0; i < outputFiles.size(); i++)
			{
				if (outputFiles[i])
				{
					bOutfileValid = true;
					break;
				}
			}
			StartThreads(!bQuiet || bOutfileValid);
		}
	}

	if (!bIsFile)
	{
		CPath dirPath(inputArg.c_str());
		dwError = HashDirectory(dirPath, pHashes, bIncludeNames, bStripNames, bQuiet, bShowProgress, bSumMode, digestsList);
	}
	else
	{
		CPath filePath(inputArg.c_str());
		dwError = NO_ERROR;
		if (bSumMode)
		{
			// if input is a file, -sumRelativePath is irrelevant
			g_bSumRelativePath = false;
			if (!digestsList.empty())
			{
				// verification
				if (0 == _wcsicmp(g_verificationFileName.GetAbsolutPathValue().c_str(), filePath.GetAbsolutPathValue().c_str()))
				{
					ShowError(L"Input file is the same as SUM verification file. Aborting!");
					dwError = ERROR_INVALID_PARAMETER;
				}
			}
			else
			{
				if (!g_outputFileName.GetAbsolutPathValue().empty() && (0 == _wcsicmp(g_outputFileName.GetAbsolutPathValue().c_str(), filePath.GetAbsolutPathValue().c_str())))
				{
					ShowError(L"Input file is the same as SUM result file. Aborting!");
					dwError = ERROR_INVALID_PARAMETER;
				}
			}
		};

		if (dwError == NO_ERROR)
			dwError = HashFile(filePath, pHashes, bIncludeNames, bStripNames, bQuiet, bShowProgress, bSumMode, digestsList);
	}

	if (bSumMode)
	{
		if (bUseThreads)
			StopThreads(dwError != NO_ERROR);
		g_wCurrentAttributes = g_wAttributes;
		SetConsoleTextAttribute(g_hConsole, g_wAttributes);
	}

	if (dwError == NO_ERROR)
	{
		if (bSumMode)
		{
			if (bVerifyMode)
			{
				// check if some entries in SUM files where not processed
				size_t skippedEntries = 0;
				for (std::map<wstring, HashResultEntry>::iterator It = digestsList.begin(); It != digestsList.end(); It++)
				{
					if (!It->second.m_processed)
						skippedEntries++;
				}

				if (skippedEntries)
				{
					if (!bQuiet)
					{
						if (skippedEntries == 1)
							ShowWarning(_T("1 entry in \"%s\" was not found:\n"), g_verificationFileName.GetPathValue().c_str());
						else
							ShowWarning(_T("%lu entries in \"%s\" where not found:\n"), (unsigned long)skippedEntries, g_verificationFileName.GetPathValue().c_str());
					}
					if (outputFiles[0])
					{
						if (skippedEntries == 1)
							_ftprintf(*outputFiles[0], _T("1 entry in \"%s\" was not found:\n"), g_verificationFileName.GetPathValue().c_str());
						else
							_ftprintf(*outputFiles[0], _T("%lu entries in \"%s\" where not found:\n"), (unsigned long)skippedEntries, g_verificationFileName.GetPathValue().c_str());
					}

					unsigned long counter = 1;
					for (std::map<wstring, HashResultEntry>::iterator It = digestsList.begin(); It != digestsList.end(); It++)
					{
						if (!It->second.m_processed)
						{
							if (!bQuiet)
								ShowWarning(_T(" %lu - %s\n"), counter, It->first.c_str());
							if (outputFiles[0])
								_ftprintf(*outputFiles[0], _T(" %lu - %s\n"), counter, It->first.c_str());
							counter++;
						}
					}

					if (!bQuiet)
						_tprintf(_T("\n"));
					if (outputFiles[0])
						_ftprintf(*outputFiles[0], _T("\n"));

					// report error
					g_bMismatchFound = true;
						
				}

				if (g_bMismatchFound)
				{
					if (!bQuiet)
					{
						ShowError(_T("Verification of \"%s\" against \"%s\" failed!\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					if (outputFiles[0])
					{
						_ftprintf(*outputFiles[0], _T("Verification of \"%s\" against \"%s\" failed!\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					dwError = -7;
				}
				else
				{
					if (!bQuiet)
					{
						ShowWarning(_T("Verification of \"%s\" against \"%s\" succeeded.\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					if (outputFiles[0])
					{
						_ftprintf(*outputFiles[0], _T("Verification of \"%s\" against \"%s\" succeeded.\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
				}

				if (!skippedLines.empty())
				{
					if (!bQuiet)
						ShowWarning(_T("\n%d line(s) were skipped in \"%s\" because they are corrupted.\nSkipped lines numbers are: "), (int)skippedLines.size(), g_verificationFileName.GetPathValue().c_str());						
					if (*outputFiles[0])
						_tprintf(_T("\n%d line(s) were skipped in \"%s\" because they are corrupted.\nSkipped lines numbers are: "), (int)skippedLines.size(), g_verificationFileName.GetPathValue().c_str());
						
					for (size_t i = 0; i < min(skippedLines.size(), 9); i++)
					{
						if (!bQuiet)
							ShowWarning(_T("%d "), skippedLines[i]);							
						if (*outputFiles[0])
							_tprintf(_T("%d "), skippedLines[i]);							
					}

					if (skippedLines.size() > 9)
					{
						if (!bQuiet)
							ShowWarning(_T("... %d\n"), skippedLines[skippedLines.size() - 1]);							
						if (*outputFiles[0])
							_tprintf(_T("... %d\n"), skippedLines[skippedLines.size() - 1]);
					}
				}

			}
			else
			{
				// Sort the entries of each sum file in case of multithreaded mode
				// for this, we loop over outputFiles elements and for each non NULL element, we sort it by calling SortSumFile
				if (bUseThreads)
				{
					for (size_t i = 0; i < outputFiles.size(); i++)
					{
						if (outputFiles[i])
						{
							FILE* pShadowFile = outputFiles[i]->GetShadowFile();
							if (pShadowFile)
							{
								// close the shadow file
								outputFiles[i]->CloseShadowFile();
								// sort its content and write it to the target file
								CPath shadowFilePath(outputFiles[i]->GetShadowFileName().c_str());
								FILE* pFile = *outputFiles[i];
								if (!SortSumFile(shadowFilePath, pFile))
								{
									if (!bQuiet)
									{
										ShowError(_T("Failed to parse and write entries from the shadow file \"%s\".\n"), shadowFilePath.GetPathValue().c_str());
									}
								}
								else
								{
									// delete shadow file
									DeleteFile(shadowFilePath.GetAbsolutPathValue().c_str());
								}
							}
							else
							{
								// close the file
								outputFiles[i]->Close();
								// sort its content and overwrite it with the sorted content
								CPath filePath(outputFiles[i]->GetFileName().c_str());
								if (!SortSumFile(filePath, NULL))
								{
									if (!bQuiet)
									{
										ShowError(_T("Failed to parse and write entries from the file \"%s\".\n"), filePath.GetPathValue().c_str());
									}
								}
							}
						}
					}
				}
			}
		}
		else
		{
			BYTE pbDigest[64];
			if (bVerifyMode)
			{
				pHashes[0]->Final(pbDigest);
				if (memcmp(pbDigest, verifyDigest.data(), verifyDigest.size()))
				{
					if (!bQuiet)
					{
						ShowError(_T("Verification of \"%s\" against \"%s\" failed!\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					if (outputFiles[0])
					{
						_ftprintf(*outputFiles[0], _T("Verification of \"%s\" against \"%s\" failed!\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					dwError = -7;
				}
				else
				{
					if (!bQuiet)
					{
						ShowWarning(_T("Verification of \"%s\" against \"%s\" succeeded.\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
					if (outputFiles[0])
					{
						_ftprintf(*outputFiles[0], _T("Verification of \"%s\" against \"%s\" succeeded.\n"),
							argv[1],
							g_verificationFileName.GetPathValue().c_str());
					}
				}
			}
			else
			{
				TCHAR szDigestHex[129]; 
				// call Final method for each hash in  the pHashes vector and display the result
				for (size_t i = 0; i < pHashes.size(); i++)
				{
					pHashes[i]->Final(pbDigest);
					if (!bQuiet)
					{
						if (outputFiles[0])
						{
							_ftprintf(*outputFiles[0], __T("%s hash of \"%s\" (%d bytes) = "),
								pHashes[i]->GetID(),
								GetFileName(argv[1]),
								pHashes[i]->GetHashSize());
						}
						_tprintf(_T("%s (%d bytes) = "), pHashes[i]->GetID(), pHashes[i]->GetHashSize());
					}

					// display hash in yellow
					SetConsoleTextAttribute(g_hConsole, FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY);

					ToHex(pbDigest, pHashes[i]->GetHashSize(), szDigestHex);

					_tprintf(szDigestHex);
					if (outputFiles[0]) _ftprintf(*outputFiles[0], szDigestHex);

					if (bCopyToClipboard)
						CopyToClipboard(szDigestHex);

					// restore normal text color
					SetConsoleTextAttribute(g_hConsole, g_wAttributes);

					if (i < (pHashes.size() - 1))
					{
						_tprintf(_T("\n"));
						if (outputFiles[0]) _ftprintf(*outputFiles[0], _T("\n"));
					}
				}

				SecureZeroMemory(szDigestHex, sizeof(szDigestHex));
			}

			_tprintf(_T("\n"));
			if (outputFiles[0]) _ftprintf(*outputFiles[0], _T("\n"));

			SecureZeroMemory(pbDigest, sizeof(pbDigest));
		}

	}
	else
	{
		if (wcslen(g_szLastErrorMsg.c_str()))
			ShowErrorDirect(g_szLastErrorMsg.c_str());
	}

	SecureZeroMemory(g_pbBuffer, sizeof(g_pbBuffer));


	WaitForExit(bDontWait);
	return dwError;
}
