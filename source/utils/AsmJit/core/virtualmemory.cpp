// [AsmJit]
// Complete JIT Assembler for C++ Language.
//
// [License]
// Zlib - See COPYING file in this package.

#define ASMJIT_EXPORTS

// [Dependencies - AsmJit]
#include "../core/intutil.h"
#include "../core/virtualmemory.h"

// [Dependencies - Windows]
/*
#if defined(ASMJIT_WINDOWS)
# include <windows.h>
#endif // ASMJIT_WINDOWS
*/

// [Dependencies - Posix]
#if defined(ASMJIT_POSIX)
# include <sys/types.h>
# include <sys/mman.h>
# include <unistd.h>
#endif // ASMJIT_POSIX

// [Api-Begin]
#include "../core/apibegin.h"

//namespace AsmJit {

// ============================================================================
// [AsmJit::VirtualMemory - Windows]
// ============================================================================

#if defined(ASMJIT_WINDOWS)
struct VirtualMemoryLocal
{
  VirtualMemoryLocal()
   
  {
    //HCF
    /****
	SYSTEM_INFO info;
    GetSystemInfo(&info);
	****/

	//HCF Hardcoded!
	alignment = 64 * 1024 * 1024;
    pageSize = 4 * 1024 * 1024;
	/*
    alignment = info.dwAllocationGranularity;
    pageSize = IntUtil::roundUpToPowerOf2<uint32_t>(info.dwPageSize);
	*/
  }

  size_t alignment;
  size_t pageSize;
};

static VirtualMemoryLocal& vm()
 
{
  static VirtualMemoryLocal vm;
  return vm;
};

void* VirtualMemory::alloc(size_t length, size_t* allocated, bool canExecute)
 
{
	//HCF No handle
	return allocProcessMemory(length, allocated, canExecute);
  //return allocProcessMemory(GetCurrentProcess(), length, allocated, canExecute);
}

void VirtualMemory::free(void* addr, size_t length)
 
{
	//HCF No handle
	return freeProcessMemory(addr, length);
  //return freeProcessMemory(GetCurrentProcess(), addr, length);
}


//HCF No Handle
void* VirtualMemory::allocProcessMemory(size_t length, size_t* allocated, bool canExecute)
//void* VirtualMemory::allocProcessMemory(HANDLE hProcess, size_t length, size_t* allocated, bool canExecute)
 
{
  // VirtualAlloc rounds allocated size to page size automatically.
  size_t msize = IntUtil::roundUp(length, vm().pageSize);

  // Windows XP SP2 / Vista allow Data Excution Prevention (DEP).
  //HCF Hardcoded
  unsigned short protect = 0x40;
  //unsigned short protect = canExecute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  
  //HCF No handle and no VM
  void* mbase = malloc(msize);
  //void* mbase = VirtualAllocEx(NULL, msize, MEM_COMMIT | MEM_RESERVE, protect);
  //void* mbase = VirtualAllocEx(hProcess, NULL, msize, MEM_COMMIT | MEM_RESERVE, protect);
  
  if (mbase == NULL) return NULL;

  ASMJIT_ASSERT(IntUtil::isAligned<size_t>(reinterpret_cast<size_t>(mbase), vm().alignment));

  if (allocated != NULL)
    *allocated = msize;
  return mbase;
}

//HCF No Handle
void VirtualMemory::freeProcessMemory(void* addr, size_t /* length */)
//void VirtualMemory::freeProcessMemory(HANDLE hProcess, void* addr, size_t /* length */)
 
{
  
	//HCF No handle and no VM
	ASMJIT_FREE(addr);
	//free(addr);
	//free(addr);
	//VirtualFreeEx(addr, 0, MEM_RELEASE);
	//VirtualFreeEx(hProcess, addr, 0, MEM_RELEASE);
}

size_t VirtualMemory::getAlignment()
 
{
  return vm().alignment;
}

size_t VirtualMemory::getPageSize()
 
{
  return vm().pageSize;
}
#endif // ASMJIT_WINDOWS

// ============================================================================
// [AsmJit::VirtualMemory - Posix]
// ============================================================================

#if defined(ASMJIT_POSIX)

// MacOS uses MAP_ANON instead of MAP_ANONYMOUS.
#if !defined(MAP_ANONYMOUS)
# define MAP_ANONYMOUS MAP_ANON
#endif // MAP_ANONYMOUS

struct VirtualMemoryLocal
{
  VirtualMemoryLocal()
  {
    alignment = pageSize = ::getpagesize();
  }

  size_t alignment;
  size_t pageSize;
};

static VirtualMemoryLocal& vm()
 
{
  static VirtualMemoryLocal vm;
  return vm;
}

void* VirtualMemory::alloc(size_t length, size_t* allocated, bool canExecute)
 
{
  size_t msize = IntUtil::roundUp<size_t>(length, vm().pageSize);
  int protection = PROT_READ | PROT_WRITE | (canExecute ? PROT_EXEC : 0);

  void* mbase = ::mmap(NULL, msize, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mbase == MAP_FAILED)
    return NULL;
  
  if (allocated != NULL)
    *allocated = msize;
  return mbase;
}

void VirtualMemory::free(void* addr, size_t length)
 
{
  munmap(addr, length);
}

size_t VirtualMemory::getAlignment()
 
{
  return vm().alignment;
}

size_t VirtualMemory::getPageSize()
 
{
  return vm().pageSize;
}
#endif // ASMJIT_POSIX

//} // AsmJit namespace

// [Api-End]
#include "../core/apiend.h"
