/* ======== SourceHook ========
* Copyright (C) 2004-2008 Metamod:Source Development Team
* No warranties of any kind
*
* License: zlib/libpng
*
* Author(s): Pavol "PM OnoTo" Marko, Scott "Damaged Soul" Ehlert
* Contributors: lancevorgin, XAD, theqizmo
* ============================
*/

#ifndef __SHINT_MEMORY_H__
#define __SHINT_MEMORY_H__

// Feb 17 / 2005:
//  Unprotect now sets to readwrite
//  The vtable doesn't need to be executable anyway

# if SH_XP == SH_XP_WINAPI
#		include <windows.h>
#		define SH_MEM_READ 1
#		define SH_MEM_WRITE 2
#		define SH_MEM_EXEC 4
# elif SH_XP == SH_XP_POSIX
#		include <sys/mman.h>
#		include <stdio.h>
#		include <signal.h>
#		include <setjmp.h>
// http://www.die.net/doc/linux/man/man2/mprotect.2.html
#		include <limits.h>
#		ifndef PAGESIZE
#			define PAGESIZE 4096
#		endif
#		define SH_MEM_READ PROT_READ
#		define SH_MEM_WRITE PROT_WRITE
#		define SH_MEM_EXEC PROT_EXEC

// We need to align addr down to pagesize on linux
// We assume PAGESIZE is a power of two
#		define SH_LALIGN(x) (void*)((intptr_t)(x) & ~(PAGESIZE-1))
#		define SH_LALDIF(x) ((intptr_t)(x) & (PAGESIZE-1))
# else
#		error Unsupported OS/Compiler
# endif


namespace SourceHook
{
	inline bool SetMemAccess(void *addr, size_t len, int access)
	{
# if SH_XP == SH_XP_POSIX
		return mprotect(SH_LALIGN(addr), len + SH_LALDIF(addr), access)==0 ? true : false;
# else
		DWORD tmp;
		DWORD prot;
		switch (access)
		{
		case SH_MEM_READ:
			prot = PAGE_READONLY; break;
		case SH_MEM_READ | SH_MEM_WRITE:
			prot = PAGE_READWRITE; break;
		case SH_MEM_READ | SH_MEM_EXEC:
			prot = PAGE_EXECUTE_READ; break;
		default:
		case SH_MEM_READ | SH_MEM_WRITE | SH_MEM_EXEC:
			prot = PAGE_EXECUTE_READWRITE; break;
		}
		return VirtualProtect(addr, len, prot, &tmp) ? true : false;
# endif
	}

#if SH_XP == SH_XP_POSIX
	namespace
	{
		bool g_BadReadCalled;
		jmp_buf g_BadReadJmpBuf;

# if SH_SYS == SH_SYS_LINUX
		static void BadReadHandler(int sig)
		{
			if (g_BadReadCalled)
				longjmp(g_BadReadJmpBuf, 1);
		}
# elif SH_SYS == SH_SYS_APPLE
		static void BadReadHandler(int signal, siginfo_t* my_siginfo, void* my_context)	
		{
			if (g_BadReadCalled)
				longjmp(g_BadReadJmpBuf, 1);
		}
# endif
	}
#endif

	/**
	*	@brief Checks whether the specified memory region is (still) accessible
	*
	*	@param addr The lower boundary
	*	@param len Length of the region to be checked
	*/
	namespace
	{
		bool ModuleInMemory(char *addr, size_t len)
		{
#if SH_SYS == SH_SYS_LINUX
			// On linux, first check /proc/self/maps
			long lower = reinterpret_cast<long>(addr);
			long upper = lower + len;

			FILE *pF = fopen("/proc/self/maps", "r");
			if (pF)
			{
				// Linux /proc/self/maps -> parse
				// Format:
				// lower    upper    prot     stuff                 path
				// 08048000-0804c000 r-xp 00000000 03:03 1010107    /bin/cat
				long rlower, rupper;
				while (fscanf(pF, "%lx-%lx", &rlower, &rupper) != EOF)
				{
					// Check whether we're IN THERE!
					if (lower >= rlower && upper <= rupper)
					{
						fclose(pF);
						return true;
					}
					// Read to end of line
					int c;
					while ((c = fgetc(pF)) != '\n')
					{
						if (c == EOF)
							break;
					}
					if (c == EOF)
						break;
				}
				fclose(pF);
				return false;
			}
			pF = fopen("/proc/curproc/map", "r");
			if (pF)
			{
				// FreeBSD /proc/curproc/map -> parse
				// 0x804800 0x805500 13 15 0xc6e18960 r-x 21 0x0 COW NC vnode
				long rlower, rupper;
				while (fscanf(pF, "0x%lx 0x%lx", &rlower, &rupper) != EOF)
				{
					// Check whether we're IN THERE!
					if (lower >= rlower && upper <= rupper)
					{
						fclose(pF);
						return true;
					}
					// Read to end of line
					int c;
					while ((c = fgetc(pF)) != '\n')
					{
						if (c == EOF)
							break;
					}
					if (c == EOF)
						break;
				}
				fclose(pF);
				return false;
			}

			// Both of the above failed, try to actually read and trap sigsegv (implemented by Damaged Soul)
			void(*prevHandler)(int sig);
			g_BadReadCalled = true;

			if (setjmp(g_BadReadJmpBuf))
				return true;

			prevHandler = signal(SIGSEGV, BadReadHandler);

			volatile const char *p = reinterpret_cast<const char*>(addr);
			char dummy;

			for (size_t i = 0; i < len; i++)
				dummy = p[i];

			g_BadReadCalled = false;

			signal(SIGSEGV, prevHandler);

			return false;
#elif SH_SYS == SH_SYS_APPLE
			struct sigaction sa, osa;
			sa.sa_sigaction = BadReadHandler;
			sa.sa_flags = SA_SIGINFO | SA_RESTART;

			g_BadReadCalled = true;

			if (setjmp(g_BadReadJmpBuf))
				return false;

			if (sigaction(SIGBUS, &sa, &osa) == -1)
				return false;

			volatile const char *p = reinterpret_cast<const char *>(addr);
			char dummy;

			for (size_t i = 0; i < len; i++)
				dummy = p[i];

			g_BadReadCalled = false;

            return true;
#elif SH_SYS == SH_SYS_WIN32 // greebo: Fixed that, was SH_SYS_WINAPI, which is wrong.
			// On Win32, simply use IsBadReadPtr
			return !IsBadReadPtr(addr, len);
#endif
		}
	}
}

#endif
