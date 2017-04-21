/*****************************************************************************
                    The Dark Mod GPL Source Code
 
 This file is part of the The Dark Mod Source Code, originally based 
 on the Doom 3 GPL Source Code as published in 2011.
 
 The Dark Mod Source Code is free software: you can redistribute it 
 and/or modify it under the terms of the GNU General Public License as 
 published by the Free Software Foundation, either version 3 of the License, 
 or (at your option) any later version. For details, see LICENSE.TXT.
 
 Project: The Dark Mod (http://www.thedarkmod.com/)
 
 $Revision$ (Revision of last commit) 
 $Date$ (Date of last commit)
 $Author$ (Author of last commit)
 
******************************************************************************/

#ifndef __SYS_LOCAL__
#define __SYS_LOCAL__

/*
==============================================================

	idSysLocal

==============================================================
*/

class idSysLocal : public idSys {
public:
	virtual void			DebugPrintf( const char *fmt, ... )id_attribute((format(printf,2,3)));
	virtual void			DebugVPrintf( const char *fmt, va_list arg );

	virtual double			GetClockTicks( void );
	virtual double			ClockTicksPerSecond( void );
	virtual cpuid_t			GetProcessorId( void );
	virtual const char *	GetProcessorString( void );
	virtual const char *	FPU_GetState( void );
	virtual bool			FPU_StackIsEmpty( void );
	virtual void			FPU_SetFTZ( bool enable );
	virtual void			FPU_SetDAZ( bool enable );

	virtual void			FPU_EnableExceptions( int exceptions );

	virtual void			GetCallStack( address_t *callStack, const int callStackSize );
	virtual const char *	GetCallStackStr( const address_t *callStack, const int callStackSize );
	virtual const char *	GetCallStackCurStr( int depth );
	virtual void			ShutdownSymbols( void );

	virtual bool			LockMemory( void *ptr, int bytes );
	virtual bool			UnlockMemory( void *ptr, int bytes );

	virtual int				DLL_Load( const char *dllName );
	virtual void *			DLL_GetProcAddress( int dllHandle, const char *procName );
	virtual void			DLL_Unload( int dllHandle );
	virtual void			DLL_GetFileName( const char *baseName, char *dllName, int maxLength );

	virtual sysEvent_t		GenerateMouseButtonEvent( int button, bool down );
	virtual sysEvent_t		GenerateMouseMoveEvent( int deltax, int deltay );
	virtual sysEvent_t		GenerateJoyAxisEvent( int axis, int value );
	virtual sysEvent_t		GenerateJoyInterfaceEvent( int joyposX, int joyposY );
	virtual sysEvent_t		GenerateJoyButtonEvent( int button, bool state );

	virtual void			OpenURL( const char *url, bool quit );
	virtual void			StartProcess( const char *exeName, bool quit );
};

#endif /* !__SYS_LOCAL__ */
