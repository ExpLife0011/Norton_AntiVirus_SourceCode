// PROPRIETARY/CONFIDENTIAL. Use of this product is subject to license terms.
// Copyright (c) 2005 Symantec Corporation. All rights reserved.
/*===========================================================================*/

// Comm.cpp : Communication object specific routines.  Functions in this module are shared
// by both the MEC and SE personalities.

#include "ldvpmec.h"
#include "comm.h"
#include "advapi.h"

#include "ccEveryoneDacl.h"


HANDLE
	g_hMemFile,
	g_hMemMutex,
	g_hRTSemaphore,
	g_hPBEvent[MAX_PROCESS_BLOCKS];

PMEC_MEMFILE
	g_pMemFile;

TCHAR
	NAME_MemMutex[]		= _T("LANDesk VPMEC Mutex"),
	NAME_MemFile[]		= _T("LANDesk VPMEC MemFile"),
	NAME_MemSemaphore[]	= _T("LANDesk VPMEC Semaphore"),
	NAME_MemEvent[]		= _T("LANDesk VPMEC PBEvent ");	// Process block index appended to end of event name


// CloseCommunicationObjects() cleans up the communcation objects created by the CreateCommunicationObjects()
// function.  
//
// Inputs none
//
// Outputs
//	return	nothing -- this routine always does its best to close everything down 
//			even to the point of ignoring errors when objects are closed
// Globals
//	g_hMemFile		Handle to Memory mapped file closed and set to zero
//	g_pMemFile		Pointer to view of mapped file unmapped and set to zero
//	g_hMemMutex		Handle to memory file locking mutex closed and set to zero
//	g_hRTSemaphore	Handle to the real time semaphore closed and set to zero
//	g_hPBEvent[]	Handles to the real time process block events closed and set to zero
//
// Calling this routine does not necessarily cause the system objects to go away.  They can be held
// open by the other personality.
//
// Before calling CloseCommunicationObjects() real time processing (if any) should have been shutdown.

void CloseCommunicationObjects(void)
{
	static TCHAR s_verboseName[] = _T("CloseCommunicationObjects");

	DebugOut(SM_GENERAL, s_verboseName);

	if (g_hMemMutex)
		WaitForSingleObject(g_hMemMutex, WAIT_MemMutex);
		// If timeout or error occurs, where going proceed anyway

	// Close real time semaphore
	if (g_hRTSemaphore)
	{
		CloseHandle(g_hRTSemaphore);
		g_hRTSemaphore = 0;
	}

	// Close Process block handles
	for (int i = 0; i < MAX_PROCESS_BLOCKS; i++)
	{
		if (g_hPBEvent[i])
		{
			CloseHandle(g_hPBEvent[i]);
			g_hPBEvent[i] = 0;
		}
	}

	// Unmap view of file
	if (g_pMemFile)
	{
		UnmapViewOfFile(g_pMemFile);
		g_pMemFile = 0;
	}

	// Close MemFile
	if (g_hMemFile)
	{
		CloseHandle(g_hMemFile);
		g_hMemFile = 0;
	}

	// Close MemMutex
	if (g_hMemMutex)
	{
		ReleaseMutex(g_hMemMutex);
		CloseHandle(g_hMemMutex);
		g_hMemMutex = 0;
	}
}

// BuildProcessBlockEventName() creates the name of the event for a specific process block.
//
static void BuildProcessBlockEventName(LPTSTR buf, int cBuf, int index)
{
	TCHAR num[34];

	_tcsncpy(buf, NAME_MemEvent, cBuf);
	_tcsncat(buf, _itot(index, num, 10), (cBuf-_tcslen(NAME_MemEvent)));
}

// CreateCommunicationObjects() sets up all of the needed system objects for both SE and MEC communications
//	This includes: the Memory Mapped File, the mapping of the file, the Memory file Mutex, and the memory
//	in the in MemFile.  The real time events and semaphore are also created at this time even if real
//	time processing is not going to be used.  This simplifies the process of starting real time later.
//
// Note that either personality can "create" the communication objects.  Flags in MemFile indicate which
// personalities are active.  Shared access to the shared memory file (MemFile) is controlled by MemMutex.
// In general, MemMutex must be obtained before reading or writing MemFile.  There are some specific exceptions
// to this rule: statistical values can be accessed without obtaining the mutex.  Process blocks in state Aquired
// or Complete can be accessed without obtaining the mutex (but it must be obtained if the block is in Free, Scan,
// or Processing states).
//
// Inputs none
//
// Outputs
//	return	ERROR_SUCCESS on success, !0 on failure
//
// Globals set
//	g_hMemFile		Handle to Memory mapped file
//	g_pMemFile		Pointer to view of mapped file
//	g_hMemMutex		Handle to real time locking mutex
//	g_hRTSemaphore	Handle to the real time semaphore
//	g_hPBEvent[]	Handles to the real time process block events
//
// *g_pMemFile		If MemFile is actually created, it is initialized with zeros (and any known constant startup values).

DWORD CreateCommunicationObjects(void)
{
	CAdvApiWrap advApiDll;
	static TCHAR s_verboseName[] = _T("CreateCommunicationObjects");
	HRESULT returnVal = E_FAIL;

	bool bInitMemFile = false;

	PSECURITY_DESCRIPTOR    pSD;
	SECURITY_ATTRIBUTES     sa;
	SAV::CEveryoneDACL nullDacl;
	PACL pDacl= NULL;

	// get a empty security desriptor
	pSD = (PSECURITY_DESCRIPTOR) LocalAlloc(LPTR, sizeof(SECURITY_DESCRIPTOR));

	if (pSD == NULL)
		goto egress;

	if(g_osVersionInfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
	{
		returnVal = advApiDll.Initialize();
		if (returnVal == S_OK)
		{
			// Don't check error conditions for w9x
			if(!advApiDll.InitializeSecurityDescriptor(pSD, SECURITY_DESCRIPTOR_REVISION))
				goto egress;


			// Add a NULL disc. ACL to the security descriptor. This means ALL_ACCESS.
			OSVERSIONINFO VersionInfo;
			ZeroMemory(&VersionInfo, sizeof(VersionInfo));
			VersionInfo.dwOSVersionInfoSize = sizeof(VersionInfo);
			GetVersionEx(&VersionInfo);
			if (VersionInfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
			{
				if (nullDacl.Create() == TRUE)
					pDacl= nullDacl;
				else
					goto egress;
			}


			// add a NULL disc. ACL to the security descriptor.
			// Don't check error conditions for w9x
			if(!advApiDll.SetSecurityDescriptorDacl(pSD, TRUE, pDacl, FALSE))
				goto egress;
		}
		else
		{
			DebugOut(SM_ERROR, "ERROR:  Error occured loading ADVAPI DLL, code %x", returnVal);
			goto egress;
		}
	}

	sa.nLength = sizeof(sa);
	sa.lpSecurityDescriptor = pSD;
	sa.bInheritHandle = TRUE;       // why not...
		
	// All error exits beyond this point must exit via the egress code (to undo what has been done).

	g_hMemMutex = CreateMutex(&sa, FALSE, NAME_MemMutex);
	if (!g_hMemMutex)
	{
		// now that we're using a non-null security descriptor, CreateMutex will fail
		// if the mutex was already created by another process, so we need to try open also
		g_hMemMutex= OpenMutex( SYNCHRONIZE|MUTEX_MODIFY_STATE, true, NAME_MemMutex );

		if (!g_hMemMutex)
			goto egress;
	}

    // Acquire mutex guarding mem mapped file
    switch ( WaitForSingleObject(g_hMemMutex, WAIT_MemMutex) )
    {
    case WAIT_OBJECT_0:
    case WAIT_ABANDONED:
        // Okay
        break;
    default:
        goto egress;
    }

	g_hMemFile = CreateFileMapping((HANDLE) 0xFFFFFFFF, &sa, PAGE_READWRITE, 0, sizeof(MEC_MEMFILE), NAME_MemFile);

    if ( g_hMemFile )
    {
        bInitMemFile = GetLastError() != ERROR_ALREADY_EXISTS;
    }
    else
	{
		// now that we're using a non-null security descriptor, CreateFileMapping will fail
		// if the object was already created by another process, so we need to try open also
		g_hMemFile = OpenFileMapping(FILE_MAP_READ|FILE_MAP_WRITE, true, NAME_MemFile);

		if (!g_hMemFile)
			goto egress;
	}

	g_pMemFile = (MEC_MEMFILE *) MapViewOfFile(g_hMemFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (!g_pMemFile)
		goto egress;

	if (bInitMemFile)
	{
		memset(g_pMemFile, 0, sizeof(MEC_MEMFILE));
	
		// Initialize MemFile members which are initially non-zero

		g_pMemFile->signature = MEC_SIGNATURE;
		g_pMemFile->cInstanceID = 1;
	}

	// Initialize process block handles

	for (int i = 0; i < MAX_PROCESS_BLOCKS; i++)
	{
		TCHAR eventName[MAX_PATH];

		BuildProcessBlockEventName(eventName, MAX_PATH, i);
		g_hPBEvent[i] = CreateEvent(&sa, FALSE, FALSE, eventName);	// auto reset, not signaled
		if (!g_hPBEvent[i])
		{
			// now that we're using a non-null security descriptor, CreateEvent will fail
			// if the object was already created by another process, so we need to try open also
			g_hPBEvent[i] = OpenEvent( EVENT_MODIFY_STATE | SYNCHRONIZE, true, eventName);

			if (!g_hPBEvent[i])
				goto egress;
		}
	}

	// Create real time semaphore

	g_hRTSemaphore = CreateSemaphore(&sa, 0, MAX_PROCESS_BLOCKS, NAME_MemSemaphore);
	if (!g_hRTSemaphore)
	{
		// now that we're using a non-null security descriptor, CreateSemaphore will fail
		// if the object was already created by another process, so we need to try open also
		g_hRTSemaphore = OpenSemaphore( SYNCHRONIZE|SEMAPHORE_MODIFY_STATE, true, NAME_MemSemaphore);

		if (!g_hRTSemaphore)
			goto egress;
	}

	ReleaseMutex(g_hMemMutex);

	// get the debug property that the storage ext set...
	g_debug = g_pMemFile->bDebug;

	DebugOut(SM_GENERAL, _T("%s: successful"), s_verboseName);

	LocalFree((HLOCAL)pSD);
	
	return ERROR_SUCCESS;

egress :

	if (pSD)
		LocalFree((HLOCAL)pSD);
	
	if (g_hMemMutex)
		ReleaseMutex(g_hMemMutex);

	CloseCommunicationObjects();

	DebugOut(SM_ERROR, "ERROR: %s failure", s_verboseName);

	return ERROR_GENERAL;
}

// FindProcessBlock() searches the process block array for the lowest block with the
// given state.
//
// Inputs:
//	desiredState	Process block state to match
//	index			Pointer to index of process block
//
// Outputs:
//	returns	The first (lowest) process block with the specified state
//			or NULL if none found.
//	*index	if process block found, its index (unmodified otherwise)
//
// This function does NOT obtain MemMutex.
 
PPROCESSBLOCK FindProcessBlock(PROCESSBLOCK::ProcessState desiredState, int *pIndex)
{
	int
		i;

	for (i = 0; i < MAX_PROCESS_BLOCKS; i++)
	{
		if (g_pMemFile->processBlock[i].state == desiredState)
		{
			if (pIndex)
				*pIndex = i;
			return &g_pMemFile->processBlock[i];
		}
	}

	return 0;
}

// NextProcessBlockInstanceID() increments pMemFile->cInstanceID by two and returns the value.
// This value is then placed in pProcessBlock->instanceID when the proess block is aquired.
// It is then used by the threads that share the process block as a timeout check.
// pMemFile->cInstanceID should never be zero (hence, a pProcessBlock->instanceID should also
// never be zero in an aquired process block).  When a timeout occurs, pProcessBlock->instanceID
// can be set to zero.  The sanity check looks for a changed value (not a specific value).
// If a process block is re-assigned, the new value will be different.

inline DWORD NextProcessBlockInstanceID(void)
{
	return g_pMemFile->cInstanceID += 2;	// pMemFile->cInstanceID will always be odd and never zero.
}


// AquireProcessBlock() aquires a free process block.
//	This function obtains MemMutex and then searches for a free process block.
//	When one is found, the block is initialized, its state is set to Aquired, and its instanceID is updated.
//	The mutex is then released.
//
// Inputs:
//	*index			Pointer to index of process block
//
// Outputs:
//	returns		intialized process block or NULL if no block available
//	*index		index of block (unmodified if no block found)

PPROCESSBLOCK AquireProcessBlock(int *pIndex)
{
	PPROCESSBLOCK
		pProcessBlock = 0;

	if (WaitForSingleObject(g_hMemMutex, WAIT_MemMutex) == WAIT_OBJECT_0)
	{
		pProcessBlock = FindProcessBlock(PROCESSBLOCK::Free, pIndex);
		if (pProcessBlock)
		{
			pProcessBlock->Initialize();
			pProcessBlock->instanceID = NextProcessBlockInstanceID();
			pProcessBlock->state = PROCESSBLOCK::Aquired;
		}
		ReleaseMutex(g_hMemMutex);
	}

	return pProcessBlock;
}

// ScanProcessBlock() locates a process block to be scanned.
//	This function obtains MemMutex and then searches for a process block in state 'Scan'.
//	When one is found, its state is set to Processing, the process block is copied and
//	the mutex is then released.
//
//	Only the INPUTDATA, state and instanceID are copied (ACTIONDATA is not copied).
//
// Inputs:
//
// Outputs:
//	returns			pointer to real process block (0 if not found)
//	*pIndex			index of real process block
//	*pCopy			Copy of real process block

PPROCESSBLOCK ScanProcessBlock(int *pIndex, PPROCESSBLOCK pCopy)
{
	PPROCESSBLOCK
		pProcessBlock = 0;

	if (WaitForSingleObject(g_hMemMutex, WAIT_MemMutex) == WAIT_OBJECT_0)
	{
		pProcessBlock = FindProcessBlock(PROCESSBLOCK::Scan, pIndex);
		if (pProcessBlock)
		{
			pCopy->state = pProcessBlock->state = PROCESSBLOCK::Processing;
			pCopy->instanceID = pProcessBlock->instanceID;
			pCopy->input = pProcessBlock->input;
		}
		ReleaseMutex(g_hMemMutex);
	}

	return pProcessBlock;
}

// RequestProcessBlock() locates a process block that contains a request.
//	This function obtains MemMutex and then searches for a process block in state 'Request'.
//	When one is found, its state is set to Processing, the process block is copied and
//	the mutex is then released.
//
//	Only the INPUTDATA, state and instanceID are copied (ACTIONDATA is not copied).
//
// Inputs:
//
// Outputs:
//	returns			pointer to real process block (0 if not found)
//	*pIndex			index of real process block
//	*pCopy			Copy of real process block

PPROCESSBLOCK RequestProcessBlock(int *pIndex, PPROCESSBLOCK pCopy)
{
	PPROCESSBLOCK
		pProcessBlock = 0;

	if (WaitForSingleObject(g_hMemMutex, WAIT_MemMutex) == WAIT_OBJECT_0)
	{
		pProcessBlock = FindProcessBlock(PROCESSBLOCK::Request, pIndex);
		if (pProcessBlock)
		{
			pProcessBlock->state = PROCESSBLOCK::Processing;
			
			if(pCopy)
			{
				pCopy->state = pProcessBlock->state;
				pCopy->instanceID = pProcessBlock->instanceID;
				pCopy->input = pProcessBlock->input;
			}
		}
		ReleaseMutex(g_hMemMutex);
	}

	return pProcessBlock;
}

/* end source file */