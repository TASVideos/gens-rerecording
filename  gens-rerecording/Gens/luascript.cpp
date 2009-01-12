#include "luascript.h"
#include "gens.h"
#include "save.h"
#include "g_main.h"
#include "guidraw.h"
#include "movie.h"
#include "vdp_io.h"
#include "drawutil.h"
#include "unzip.h"
#include "Cpu_68k.h"
#include <assert.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

// the emulator must provide these so that we can implement
// the various functions the user can call from their lua script
// (this interface with the emulator needs cleanup, I know)
extern int (*Update_Frame)();
extern int (*Update_Frame_Fast)();
extern unsigned int ReadValueAtHardwareAddress(unsigned int address, unsigned int size);
extern bool WriteValueAtHardwareAddress(unsigned int address, unsigned int value, unsigned int size);
extern bool WriteValueAtHardwareRAMAddress(unsigned int address, unsigned int value, unsigned int size);
extern bool WriteValueAtHardwareROMAddress(unsigned int address, unsigned int value, unsigned int size);
extern bool IsHardwareAddressValid(unsigned int address);
extern bool IsHardwareRAMAddressValid(unsigned int address);
extern bool IsHardwareROMAddressValid(unsigned int address);
extern "C" int disableSound2, disableRamSearchUpdate;
extern "C" int Clear_Sound_Buffer(void);
extern long long GetCurrentInputCondensed();
extern long long PeekInputCondensed();
extern void SetNextInputCondensed(long long input, long long mask);
extern int Set_Current_State(int Num, bool showOccupiedMessage, bool showEmptyMessage);
extern int Update_Emulation_One(HWND hWnd);
extern void Update_Emulation_One_Before(HWND hWnd);
extern void Update_Emulation_After_Fast(HWND hWnd);
extern void Update_Emulation_One_Before_Minimal();
extern int Update_Frame_Adjusted();
extern int Update_Frame_Hook();
extern int Update_Frame_Fast_Hook();
extern void Update_Emulation_After_Controlled(HWND hWnd, bool flip);
extern void Prevent_Next_Frame_Skipping();
extern void UpdateLagCount();
extern bool BackgroundInput;
extern bool g_disableStatestateWarnings;
extern bool g_onlyCallSavestateCallbacks;
extern bool Step_Gens_MainLoop(bool allowSleep, bool allowEmulate);
extern bool frameadvSkipLagForceDisable;
extern "C" void Put_Info_NonImmediate(char *Message, int Duration);
extern int Show_Genesis_Screen();
extern const char* GensPlayMovie(const char* filename, bool silent);
extern const char* GensOpenScript(const char* filename);
extern void GensReplayMovie();

extern "C" {
	#include "lua/src/lua.h"
	#include "lua/src/lauxlib.h"
	#include "lua/src/lualib.h"
	#include "lua/src/lstate.h"
};

enum SpeedMode
{
	SPEEDMODE_NORMAL,
	SPEEDMODE_NOTHROTTLE,
	SPEEDMODE_TURBO,
	SPEEDMODE_MAXIMUM,
};

struct LuaContextInfo {
	lua_State* L; // the Lua state
	bool started; // script has been started and hasn't yet been terminated, although it may not be currently running
	bool running; // script is currently running code (either the main call to the script or the callbacks it registered)
	bool returned; // main call to the script has returned (but it may still be active if it registered callbacks)
	bool crashed; // true if script has errored out
	bool restart; // if true, tells the script-running code to restart the script when the script stops
	bool restartLater; // set to true when a still-running script is stopped so that RestartAllLuaScripts can know which scripts to restart
	unsigned int worryCount; // counts up as the script executes, gets reset when the application is able to process messages, triggers a warning prompt if it gets too high
	bool stopWorrying; // set to true if the user says to let the script run forever despite appearing to be frozen
	bool panic; // if set to true, tells the script to terminate as soon as it can do so safely (used because directly calling lua_close() or luaL_error() is unsafe in some contexts)
	bool ranExit; // used to prevent a registered exit callback from ever getting called more than once
	bool guiFuncsNeedDeferring; // true whenever GUI drawing would be cleared by the next emulation update before it would be visible, and thus needs to be deferred until after the next emulation update
	int numDeferredGUIFuncs; // number of deferred function calls accumulated, used to impose an arbitrary limit to avoid running out of memory
	bool ranFrameAdvance; // false if gens.frameadvance() hasn't been called yet
	int transparencyModifier; // values less than 255 will scale down the opacity of whatever the GUI renders, values greater than 255 will increase the opacity of anything transparent the GUI renders
	SpeedMode speedMode; // determines how gens.frameadvance() acts
	char panicMessage [64]; // a message to print if the script terminates due to panic being set
	std::string lastFilename; // path to where the script last ran from so that restart can work (note: storing the script in memory instead would not be useful because we always want the most up-to-date script from file)
	std::string nextFilename; // path to where the script should run from next, mainly used in case the restart flag is true
	unsigned int dataSaveKey; // crc32 of the save data key, used to decide which script should get which data... by default (if no key is specified) it's calculated from the script filename
	unsigned int dataLoadKey; // same as dataSaveKey but set through registerload instead of registersave if the two differ
	bool dataSaveLoadKeySet; // false if the data save keys are unset or set to their default value
	// callbacks into the lua window... these don't need to exist per context the way I'm using them, but whatever
	void(*print)(int uid, const char* str);
	void(*onstart)(int uid);
	void(*onstop)(int uid, bool statusOK);
};
std::map<int, LuaContextInfo*> luaContextInfo;
std::map<lua_State*, int> luaStateToUIDMap;
int g_numScriptsStarted = 0;
bool g_anyScriptsHighSpeed = false;
bool g_stopAllScriptsEnabled = true;

#define USE_INFO_STACK
#ifdef USE_INFO_STACK
	std::vector<LuaContextInfo*> infoStack;
	#define GetCurrentInfo() *infoStack.front() // should be faster but relies on infoStack correctly being updated to always have the current info in the first element
#else
	std::map<lua_State*, LuaContextInfo*> luaStateToContextMap;
	#define GetCurrentInfo() *luaStateToContextMap[L] // should always work but might be slower
#endif


static const char* luaCallIDStrings [] =
{
	"CALL_BEFOREEMULATION",
	"CALL_AFTEREMULATION",
	"CALL_AFTEREMULATIONGUI",
	"CALL_BEFOREEXIT",
	"CALL_BEFORESAVE",
	"CALL_AFTERLOAD",

	"CALL_HOTKEY_1",
	"CALL_HOTKEY_2",
	"CALL_HOTKEY_3",
	"CALL_HOTKEY_4",
	"CALL_HOTKEY_5",
	"CALL_HOTKEY_6",
	"CALL_HOTKEY_7",
	"CALL_HOTKEY_8",
	"CALL_HOTKEY_9",
	"CALL_HOTKEY_10",
	"CALL_HOTKEY_11",
	"CALL_HOTKEY_12",
	"CALL_HOTKEY_13",
	"CALL_HOTKEY_14",
	"CALL_HOTKEY_15",
	"CALL_HOTKEY_16",
};

static const int _makeSureWeHaveTheRightNumberOfStrings [sizeof(luaCallIDStrings)/sizeof(*luaCallIDStrings) == LUACALL_COUNT ? 1 : 0];

void StopScriptIfFinished(int uid, bool justReturned = false);
void SetSaveKey(LuaContextInfo& info, const char* key);
void SetLoadKey(LuaContextInfo& info, const char* key);
void RefreshScriptStartedStatus();
void RefreshScriptSpeedStatus();

int add_memory_proc (int *list, int addr, int &numprocs)
{
	if (numprocs >= 16) return 1;
	int i = 0;
	while ((i < numprocs) && (list[i] < addr))
		i++;
	for (int j = numprocs; j >= i; j--)
		list[j] = list[j-i];
	list[i] = addr;
	numprocs++;
	return 0;
}
int del_memory_proc (int *list, int addr, int &numprocs)
{
	if (numprocs <= 0) return 1;
	int i = 0;
//	bool found = false;
	while ((i < numprocs) && (list[i] != addr))
		i++;
	if (list[i] == addr)
	{
		while (i < numprocs)
		{
			list[i] = list[i+1];
			i++;
		}
		list[i]=0;
		return 0;
	}
	else return 1;
}
#define memreg(proc)\
int proc##_writelist[16];\
int proc##_readlist[16];\
int proc##_execlist[16];\
int proc##_numwritefuncs = 0;\
int proc##_numreadfuncs = 0;\
int proc##_numexecfuncs = 0;\
int memory_register##proc##write (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_W%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_writelist, addr, proc##_numwritefuncs);\
	else \
		if (add_memory_proc(proc##_writelist, addr, proc##_numwritefuncs)) \
			luaL_error(L, #proc "write hook registry is full.");\
	return 0;\
}\
int memory_register##proc##read (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_R%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_readlist, addr, proc##_numreadfuncs);\
	else \
		if (add_memory_proc(proc##_readlist, addr, proc##_numreadfuncs)) \
			luaL_error(L, #proc "read hook registry is full.");\
	return 0;\
}\
int memory_register##proc##exec (lua_State* L)\
{\
	unsigned int addr = luaL_checkinteger(L, 1);\
	char Name[16];\
	sprintf(Name,#proc"_E%08X",addr);\
	if (lua_type(L,2) != LUA_TNIL && lua_type(L,2) != LUA_TFUNCTION)\
		luaL_error(L, "function or nil expected in arg 2 to memory.register" #proc "write");\
	lua_setfield(L, LUA_REGISTRYINDEX, Name);\
	if (lua_type(L,2) == LUA_TNIL) \
		del_memory_proc(proc##_execlist, addr, proc##_numexecfuncs);\
	else \
		if (add_memory_proc(proc##_execlist, addr, proc##_numexecfuncs)) \
			luaL_error(L, #proc "exec hook registry is full.");\
	return 0;\
}
memreg(M68K)
memreg(S68K)
//memreg(SH2)
//memreg(Z80)
int gens_registerbefore(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gens_registerafter(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATION]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gens_registerexit(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int gui_register(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTEREMULATIONGUI]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
static const char* toCString(lua_State* L, int i);
int state_registersave(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	if (!lua_isnoneornil(L,2))
		SetSaveKey(GetCurrentInfo(), toCString(L,2));
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFORESAVE]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}
int state_registerload(lua_State* L)
{
	if (!lua_isnil(L,1))
		luaL_checktype(L, 1, LUA_TFUNCTION);
	if (!lua_isnoneornil(L,2))
		SetLoadKey(GetCurrentInfo(), toCString(L,2));
	lua_settop(L,1);
	lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_AFTERLOAD]);
	StopScriptIfFinished(luaStateToUIDMap[L]);
	return 0;
}

int input_registerhotkey(lua_State* L)
{
	int hotkeyNumber = luaL_checkinteger(L,1);
	if(hotkeyNumber < 1 || hotkeyNumber > 16)
		luaL_error(L, "input.registerhotkey(n,func) requires 1 <= n <= 16, but got n = %d.", hotkeyNumber);
	else
	{
		if (!lua_isnil(L,2))
			luaL_checktype(L, 2, LUA_TFUNCTION);
		lua_settop(L,2);
		lua_setfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_SCRIPT_HOTKEY_1 + hotkeyNumber-1]);
		StopScriptIfFinished(luaStateToUIDMap[L]);
	}
	return 0;
}

static const char* deferredGUIIDString = "lazygui";

// store the most recent C function call from Lua (and all its arguments)
// for later evaluation
void DeferFunctionCall(lua_State* L, const char* idstring)
{
	// there's probably a cleaner way of doing this using lua_pushcclosure or lua_getfenv

	int num = lua_gettop(L);

	// get the C function pointer
	//lua_CFunction cf = lua_tocfunction(L, -(num+1));
	lua_CFunction cf = (L->ci->func)->value.gc->cl.c.f;
	assert(cf);
	lua_pushcfunction(L,cf);

	// make a list of the function and its arguments (and also pop those arguments from the stack)
	lua_createtable(L, num+1, 0);
	lua_insert(L, 1);
	for(int n = num+1; n > 0; n--)
		lua_rawseti(L, 1, n);

	// put the list into a global array
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	lua_insert(L, 1);
	int curSize = luaL_getn(L, 1);
	lua_rawseti(L, 1, curSize+1);

	// clean the stack
	lua_settop(L, 0);
}
void CallDeferredFunctions(lua_State* L, const char* idstring)
{
	lua_settop(L, 0);
	lua_getfield(L, LUA_REGISTRYINDEX, idstring);
	int numCalls = luaL_getn(L, 1);
	for(int i = 1; i <= numCalls; i++)
	{
        lua_rawgeti(L, 1, i);  // get the function+arguments list
		int listSize = luaL_getn(L, 2);

		// push the arguments and the function
		for(int j = 1; j <= listSize; j++)
			lua_rawgeti(L, 2, j);

		// get and pop the function
		lua_CFunction cf = lua_tocfunction(L, -1);
		lua_pop(L, 1);

		// swap the arguments on the stack with the list we're iterating through
		// before calling the function, because C functions assume argument 1 is at 1 on the stack
		lua_pushvalue(L, 1);
		lua_remove(L, 2);
		lua_remove(L, 1);

		// call the function
		cf(L);
 
		// put the list back where it was
		lua_replace(L, 1);
		lua_settop(L, 1);
	}

	// clear the list of deferred functions
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, idstring);
	LuaContextInfo& info = GetCurrentInfo();
	info.numDeferredGUIFuncs = 0;

	// clean the stack
	lua_settop(L, 0);
}

#define MAX_DEFERRED_COUNT 16384

bool DeferGUIFuncIfNeeded(lua_State* L)
{
	LuaContextInfo& info = GetCurrentInfo();
	if(info.speedMode == SPEEDMODE_MAXIMUM)
	{
		// if the mode is "maximum" then discard all GUI function calls
		// and pretend it was because we deferred them
		return true;
	}
	if(info.guiFuncsNeedDeferring)
	{
		if(info.numDeferredGUIFuncs < MAX_DEFERRED_COUNT)
		{
			// defer whatever function called this one until later
			DeferFunctionCall(L, deferredGUIIDString);
			info.numDeferredGUIFuncs++;
		}
		else
		{
			// too many deferred functions on the same frame
			// silently discard the rest
		}
		return true;
	}

	// ok to run the function right now
	return false;
}

void worry(lua_State* L, int intensity)
{
	LuaContextInfo& info = GetCurrentInfo();
	info.worryCount += intensity;
}

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

static std::vector<const void*> s_tableAddressStack;

#define APPENDPRINT { int n = snprintf(ptr, remaining,
#define END ); if(n >= 0) { ptr += n; remaining -= n; } else { remaining = 0; } }
static void toCStringConverter(lua_State* L, int i, char*& ptr, int& remaining)
{
	if(remaining <= 0)
		return;

	const char* str = ptr; // for debugging

	switch(lua_type(L, i))
	{
		case LUA_TNONE: APPENDPRINT "no value" END break;
		case LUA_TNIL: APPENDPRINT "nil" END break;
		case LUA_TBOOLEAN: APPENDPRINT lua_toboolean(L,i) ? "true" : "false" END break;
		case LUA_TSTRING: APPENDPRINT "%s",lua_tostring(L,i) END break;
		case LUA_TNUMBER: APPENDPRINT "%.12Lg",lua_tonumber(L,i) END break;
defcase:default: APPENDPRINT "%s:%p",luaL_typename(L,i),lua_topointer(L,i) END break;
		case LUA_TTABLE:
		{
			// first make sure there's enough stack space
			if(!lua_checkstack(L, 4))
			{
				// note that even if lua_checkstack never returns false,
				// that doesn't mean we didn't need to call it,
				// because calling it retrieves stack space past LUA_MINSTACK
				goto defcase;
			}

			std::vector<const void*>::const_iterator foundCycleIter = std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L,i));
			if(foundCycleIter != s_tableAddressStack.end())
			{
				int parentNum = s_tableAddressStack.end() - foundCycleIter;
				if(parentNum > 1)
					APPENDPRINT "%s:parent^%d",luaL_typename(L,i),parentNum END
				else
					APPENDPRINT "%s:parent",luaL_typename(L,i) END
			}
			else
			{
				s_tableAddressStack.push_back(lua_topointer(L,i));
				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				APPENDPRINT "{" END

				lua_pushnil(L); // first key
				int keyIndex = lua_gettop(L);
				int valueIndex = keyIndex + 1;
				bool first = true;
				while(lua_next(L, i))
				{
					bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
					bool invalidLuaIdentifier = (!keyIsString || !isalpha(*lua_tostring(L, keyIndex)));
					if(first)
						first = false;
					else
						APPENDPRINT ", " END
					if(invalidLuaIdentifier)
						if(keyIsString)
							APPENDPRINT "['" END
						else
							APPENDPRINT "[" END

					toCStringConverter(L, keyIndex, ptr, remaining); // key

					if(invalidLuaIdentifier)
						if(keyIsString)
							APPENDPRINT "']=" END
						else
							APPENDPRINT "]=" END
					else
						APPENDPRINT "=" END

					bool valueIsString = (lua_type(L, valueIndex) == LUA_TSTRING);
					if(valueIsString)
						APPENDPRINT "'" END

					toCStringConverter(L, valueIndex, ptr, remaining); // value

					if(valueIsString)
						APPENDPRINT "'" END

					lua_pop(L, 1);

					if(remaining <= 0)
					{
						lua_settop(L, keyIndex-1); // stack might not be clean yet if we're breaking early
						break;
					}
				}
				APPENDPRINT "}" END
			}
		}	break;
	}
}
static const char* toCString(lua_State* L)
{
	static const int maxLen = 8192;
	static char str[maxLen];

	str[0] = 0;
	char* ptr = str;

	int remaining = maxLen;
	int n=lua_gettop(L);
	for(int i = 1; i <= n; i++)
	{
		toCStringConverter(L, i, ptr, remaining);
		if(i != n)
			APPENDPRINT " " END
	}

	if(remaining < 3)
	{
		while(remaining < 6)
		{
			remaining++;
			ptr--;
		}
		APPENDPRINT "..." END
	}

	APPENDPRINT "\r\n" END

	return str;
}
static const char* toCString(lua_State* L, int i)
{
	luaL_checkany(L,i);

	static const int maxLen = 8192;
	static char str[maxLen];
	str[0] = 0;
	char* ptr = str;

	int remaining = maxLen;
	toCStringConverter(L, i, ptr, remaining);
	return str;
}
#undef APPENDPRINT
#undef END

static int gens_message(lua_State* L)
{
	const char* str = toCString(L);
	Put_Info_NonImmediate((char*)str, 500);
	return 0;
}

static int print(lua_State* L)
{
	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = *luaContextInfo[uid];

	const char* str = toCString(L);

	if(info.print)
		info.print(uid, str);
	else
		puts(str);

	worry(L, 100);
	return 0;
}

// provides a way to get (from Lua) the string
// that print() or gens.message() would print
static int tostring(lua_State* L)
{
	const char* str = toCString(L);
	lua_pushstring(L, str);
	return 1;
}

// provides an easy way to copy a table from Lua
// (simple assignment only makes an alias, but sometimes an independent table is desired)
// currently this function only performs a shallow copy,
// but I think it should be changed to do a deep copy (possibly of configurable depth?)
// that maintains the internal table reference structure
static int copytable(lua_State *L)
{
	int origIndex = 1; // we only care about the first argument
	int origType = lua_type(L, origIndex);
	if(origType == LUA_TNIL)
	{
		lua_pushnil(L);
		return 1;
	}
	if(origType != LUA_TTABLE)
	{
		luaL_typerror(L, 1, lua_typename(L, LUA_TTABLE));
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	int copyIndex = lua_gettop(L);

	lua_pushnil(L); // first key
	int keyIndex = lua_gettop(L);
	int valueIndex = keyIndex + 1;

	while(lua_next(L, origIndex))
	{
		lua_pushvalue(L, keyIndex);
		lua_pushvalue(L, valueIndex);
		lua_rawset(L, copyIndex); // copytable[key] = value
		lua_pop(L, 1);
	}

	return 1; // return the new table
}

// because print traditionally shows the address of tables,
// and the print function I provide instead shows the contents of tables,
// I also provide this function
// (otherwise there would be no way to see a table's address, AFAICT)
static int addressof(lua_State *L)
{
	const void* ptr = lua_topointer(L,-1);
	lua_pushinteger(L, (lua_Integer)ptr);
	return 1;
}

static int bitand(lua_State *L)
{
	int rv = ~0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv &= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitor(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv |= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitxor(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv ^= luaL_checkinteger(L,i);
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}
static int bitshift(lua_State *L)
{
	int num = luaL_checkinteger(L,1);
	int shift = luaL_checkinteger(L,2);
	if(shift < 0)
		num <<= -shift;
	else
		num >>= shift;
	lua_settop(L,0);
	lua_pushinteger(L,num);
	return 1;
}
static int bitbit(lua_State *L)
{
	int rv = 0;
	int numArgs = lua_gettop(L);
	for(int i = 1; i <= numArgs; i++)
		rv |= (1 << luaL_checkinteger(L,i));
	lua_settop(L,0);
	lua_pushinteger(L,rv);
	return 1;
}

int gens_wait(lua_State* L);


#define HOOKCOUNT 4096
#define MAX_WORRY_COUNT 6000
void LuaRescueHook(lua_State* L, lua_Debug *dbg)
{
	LuaContextInfo& info = GetCurrentInfo();

	info.worryCount++;

	if(info.stopWorrying && !info.panic)
	{
		if(info.worryCount > (MAX_WORRY_COUNT >> 2))
		{
			// the user already said they're OK with the script being frozen,
			// but we don't trust their judgement completely,
			// so periodically update the main loop so they have a chance to manually stop it
			gens_wait(L);
			info.worryCount = 0;
		}
		return;
	}

	if(info.worryCount > MAX_WORRY_COUNT || info.panic)
	{
		info.worryCount = 0;
		info.stopWorrying = false;

		int answer = IDYES;
		if(!info.panic)
		{
#ifdef _WIN32
			DialogsOpen++;
			answer = MessageBox(HWnd, "A Lua script has been running for quite a while. Maybe it is in an infinite loop.\n\nWould you like to stop the script?\n\n(Yes to stop it now,\n No to keep running and not ask again,\n Cancel to keep running but ask again later)", "Lua Alert", MB_YESNOCANCEL | MB_DEFBUTTON3 | MB_ICONASTERISK);
			DialogsOpen--;
#endif
		}

		if(answer == IDNO)
			info.stopWorrying = true; // don't remove the hook because we need it still running for RequestAbortLuaScript to work

		if(answer == IDYES)
		{
			//lua_sethook(L, NULL, 0, 0);
			assert(L->errfunc || L->errorJmp);
			luaL_error(L, info.panic ? info.panicMessage : "terminated by user");
		}

		info.panic = false;
	}
}

// acts similar to normal emulation update
// except without the user being able to activate emulator commands
int gens_emulateframe(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	Update_Emulation_One(HWnd);
	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately after emulating many frames by this method

	worry(L,300);
	return 0;
}

// acts as a fast-forward emulation update that still renders every frame
// and the user is unable to activate emulator commands during it
int gens_emulateframefastnoskipping(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	Update_Emulation_One_Before(HWnd);
	Update_Frame_Hook();
	Update_Emulation_After_Controlled(HWnd, true);
	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately after a bout of fast-forward frames

	worry(L,200);
	return 0;
}

// acts as a (very) fast-forward emulation update
// where the user is unable to activate emulator commands
int gens_emulateframefast(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	Update_Emulation_One_Before(HWnd);

	if(FrameCount%16 == 0) // skip rendering 15 out of 16 frames
	{
		// update once and render
		Update_Frame_Hook();
		Update_Emulation_After_Controlled(HWnd, true);
	}
	else
	{
		// update once but skip rendering
		Update_Frame_Fast_Hook();
		Update_Emulation_After_Controlled(HWnd, false);
	}

	Prevent_Next_Frame_Skipping(); // so we don't skip a whole bunch of frames immediately AFTER a bout of fast-forward frames

	worry(L,150);
	return 0;
}

// acts as an extremely-fast-forward emulation update
// that also doesn't render any graphics or generate any sounds,
// and the user is unable to activate emulator commands during it.
// if you load a savestate after calling this function,
// it should leave no trace of having been called,
// so you can do things like generate future emulation states every frame
// while the user continues to see and hear normal emulation
int gens_emulateframeinvisible(lua_State* L)
{
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		return 0;

	int oldDisableSound2 = disableSound2;
	int oldDisableRamSearchUpdate = disableRamSearchUpdate;
	disableSound2 = true;
	disableRamSearchUpdate = true;

	Update_Emulation_One_Before_Minimal();
	Update_Frame_Fast();
	UpdateLagCount();

	disableSound2 = oldDisableSound2;
	disableRamSearchUpdate = oldDisableRamSearchUpdate;

	// disable video latency compensation for a few frames
	// because it can get pretty slow if that's doing prediction updates every frame
	// when the lua script is also doing prediction updates
	disableVideoLatencyCompensationCount = VideoLatencyCompensation + 1;

	worry(L,100);
	return 0;
}

#ifndef _WIN32
	#define stricmp strcasecmp
	#define strnicmp strncasecmp
#endif

int gens_speedmode(lua_State* L)
{
	SpeedMode newSpeedMode = SPEEDMODE_NORMAL;
	if(lua_isnumber(L,1))
		newSpeedMode = (SpeedMode)luaL_checkinteger(L,1);
	else
	{
		const char* str = luaL_checkstring(L,1);
		if(!stricmp(str, "normal"))
			newSpeedMode = SPEEDMODE_NORMAL;
		else if(!stricmp(str, "nothrottle"))
			newSpeedMode = SPEEDMODE_NOTHROTTLE;
		else if(!stricmp(str, "turbo"))
			newSpeedMode = SPEEDMODE_TURBO;
		else if(!stricmp(str, "maximum"))
			newSpeedMode = SPEEDMODE_MAXIMUM;
	}

	LuaContextInfo& info = GetCurrentInfo();
	info.speedMode = newSpeedMode;
	RefreshScriptSpeedStatus();
	return 0;
}

// tells Gens to wait while the script is doing calculations
// can call this periodically instead of gens.frameadvance
// note that the user can use hotkeys at this time
// (e.g. a savestate could possibly get loaded before gens.wait() returns)
int gens_wait(lua_State* L)
{
	LuaContextInfo& info = GetCurrentInfo();

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			Step_Gens_MainLoop(true, false);
			break;
		case SPEEDMODE_NOTHROTTLE:
		case SPEEDMODE_TURBO:
		case SPEEDMODE_MAXIMUM:
			Step_Gens_MainLoop(Paused!=0, false);
			break;
	}

	return 0;
}

int gens_frameadvance(lua_State* L)
{
	int uid = luaStateToUIDMap[L];
	LuaContextInfo& info = GetCurrentInfo();

	if(!info.ranFrameAdvance)
	{
		// otherwise we'll never see the first frame of GUI drawing
		if(info.speedMode != SPEEDMODE_MAXIMUM)
			Show_Genesis_Screen();
		info.ranFrameAdvance = true;
	}

	switch(info.speedMode)
	{
		default:
		case SPEEDMODE_NORMAL:
			while(!Step_Gens_MainLoop(true, true) && !info.panic);
			break;
		case SPEEDMODE_NOTHROTTLE:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			if(!(FastForwardKeyDown && (GetActiveWindow()==HWnd || BackgroundInput)))
				gens_emulateframefastnoskipping(L);
			else
				gens_emulateframefast(L);
			break;
		case SPEEDMODE_TURBO:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			gens_emulateframefast(L);
			break;
		case SPEEDMODE_MAXIMUM:
			while(!Step_Gens_MainLoop(Paused!=0, false) && !info.panic);
			gens_emulateframeinvisible(L);
			break;
	}
	return 0;
}

int gens_pause(lua_State* L)
{
	Paused = 1;
	gens_frameadvance(L);

	// allow the user to not have to manually unpause
	// after restarting a script that used gens.pause()
	LuaContextInfo& info = GetCurrentInfo();
	if(info.panic)
		Paused = 0;

	return 0;
}

int gens_redraw(lua_State* L)
{
	Show_Genesis_Screen();
	worry(L,250);
	return 0;
}



int memory_readbyte(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned char value = (unsigned char)(ReadValueAtHardwareAddress(address, 1) & 0xFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1; // we return the number of return values
}
int memory_readbytesigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed char value = (signed char)(ReadValueAtHardwareAddress(address, 1) & 0xFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned short value = (unsigned short)(ReadValueAtHardwareAddress(address, 2) & 0xFFFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readwordsigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed short value = (signed short)(ReadValueAtHardwareAddress(address, 2) & 0xFFFF);
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readdword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned long value = (unsigned long)(ReadValueAtHardwareAddress(address, 4));
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}
int memory_readdwordsigned(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	signed long value = (signed long)(ReadValueAtHardwareAddress(address, 4));
	lua_settop(L,0);
	lua_pushinteger(L, value);
	return 1;
}

int memory_writebyte(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned char value = (unsigned char)(luaL_checkinteger(L,2) & 0xFF);
	WriteValueAtHardwareRAMAddress(address, value, 1);
	return 0;
}
int memory_writeword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned short value = (unsigned short)(luaL_checkinteger(L,2) & 0xFFFF);
	WriteValueAtHardwareRAMAddress(address, value, 2);
	return 0;
}
int memory_writedword(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	unsigned long value = (unsigned long)(luaL_checkinteger(L,2));
	WriteValueAtHardwareRAMAddress(address, value, 4);
	return 0;
}

int memory_readbyterange(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	int length = luaL_checkinteger(L,2);

	if(length < 0)
	{
		address += length;
		length = -length;
	}

	// push the array
	lua_createtable(L, abs(length), 0);

	// put all the values into the (1-based) array
	for(int a = address, n = 1; n <= length; a++, n++)
	{
		if(IsHardwareAddressValid(a))
		{
			unsigned char value = (unsigned char)(ReadValueAtHardwareAddress(a, 1) & 0xFF);
			lua_pushinteger(L, value);
			lua_rawseti(L, -2, n);
		}
		// else leave the value nil
	}

	return 1;
}

int memory_isvalid(lua_State* L)
{
	int address = luaL_checkinteger(L,1);
	lua_settop(L,0);
	lua_pushboolean(L, IsHardwareAddressValid(address));
	return 1;
}

struct registerPointerMap
{
	const char* registerName;
	unsigned int* pointer;
	int dataSize;
};

#define RPM_ENTRY(name,var) {name, (unsigned int*)&var, sizeof(var)},

registerPointerMap m68kPointerMap [] = {
	RPM_ENTRY("a0", main68k_context.areg[0])
	RPM_ENTRY("a1", main68k_context.areg[1])
	RPM_ENTRY("a2", main68k_context.areg[2])
	RPM_ENTRY("a3", main68k_context.areg[3])
	RPM_ENTRY("a4", main68k_context.areg[4])
	RPM_ENTRY("a5", main68k_context.areg[5])
	RPM_ENTRY("a6", main68k_context.areg[6])
	RPM_ENTRY("a7", main68k_context.areg[7])
	RPM_ENTRY("d0", main68k_context.dreg[0])
	RPM_ENTRY("d1", main68k_context.dreg[1])
	RPM_ENTRY("d2", main68k_context.dreg[2])
	RPM_ENTRY("d3", main68k_context.dreg[3])
	RPM_ENTRY("d4", main68k_context.dreg[4])
	RPM_ENTRY("d5", main68k_context.dreg[5])
	RPM_ENTRY("d6", main68k_context.dreg[6])
	RPM_ENTRY("d7", main68k_context.dreg[7])
	RPM_ENTRY("pc", main68k_context.pc)
	RPM_ENTRY("sr", main68k_context.sr)
	{}
};
registerPointerMap s68kPointerMap [] = {
	RPM_ENTRY("a0", sub68k_context.areg[0])
	RPM_ENTRY("a1", sub68k_context.areg[1])
	RPM_ENTRY("a2", sub68k_context.areg[2])
	RPM_ENTRY("a3", sub68k_context.areg[3])
	RPM_ENTRY("a4", sub68k_context.areg[4])
	RPM_ENTRY("a5", sub68k_context.areg[5])
	RPM_ENTRY("a6", sub68k_context.areg[6])
	RPM_ENTRY("a7", sub68k_context.areg[7])
	RPM_ENTRY("d0", sub68k_context.dreg[0])
	RPM_ENTRY("d1", sub68k_context.dreg[1])
	RPM_ENTRY("d2", sub68k_context.dreg[2])
	RPM_ENTRY("d3", sub68k_context.dreg[3])
	RPM_ENTRY("d4", sub68k_context.dreg[4])
	RPM_ENTRY("d5", sub68k_context.dreg[5])
	RPM_ENTRY("d6", sub68k_context.dreg[6])
	RPM_ENTRY("d7", sub68k_context.dreg[7])
	RPM_ENTRY("pc", sub68k_context.pc)
	RPM_ENTRY("sr", sub68k_context.sr)
	{}
};

struct cpuToRegisterMap
{
	const char* cpuName;
	registerPointerMap* rpmap;
}
cpuToRegisterMaps [] =
{
	{"m68k.", m68kPointerMap},
	{"s68k.", s68kPointerMap},
};


int memory_getregister(lua_State* L)
{
	const char* qualifiedRegisterName = luaL_checkstring(L,1);
	lua_settop(L,0);
	for(int cpu = 0; cpu < sizeof(cpuToRegisterMaps)/sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		int cpuNameLen = strlen(ctrm.cpuName);
		if(!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for(int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if(!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch(rpm.dataSize)
					{ default:
					case 1: lua_pushinteger(L, *(unsigned char*)rpm.pointer); break;
					case 2: lua_pushinteger(L, *(unsigned short*)rpm.pointer); break;
					case 4: lua_pushinteger(L, *(unsigned long*)rpm.pointer); break;
					}
					return 1;
				}
			}
			lua_pushnil(L);
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}
int memory_setregister(lua_State* L)
{
	const char* qualifiedRegisterName = luaL_checkstring(L,1);
	unsigned long value = (unsigned long)(luaL_checkinteger(L,2));
	lua_settop(L,0);
	for(int cpu = 0; cpu < sizeof(cpuToRegisterMaps)/sizeof(*cpuToRegisterMaps); cpu++)
	{
		cpuToRegisterMap ctrm = cpuToRegisterMaps[cpu];
		int cpuNameLen = strlen(ctrm.cpuName);
		if(!strnicmp(qualifiedRegisterName, ctrm.cpuName, cpuNameLen))
		{
			qualifiedRegisterName += cpuNameLen;
			for(int reg = 0; ctrm.rpmap[reg].dataSize; reg++)
			{
				registerPointerMap rpm = ctrm.rpmap[reg];
				if(!stricmp(qualifiedRegisterName, rpm.registerName))
				{
					switch(rpm.dataSize)
					{ default:
					case 1: *(unsigned char*)rpm.pointer = (unsigned char)(value & 0xFF); break;
					case 2: *(unsigned short*)rpm.pointer = (unsigned short)(value & 0xFFFF); break;
					case 4: *(unsigned long*)rpm.pointer = value; break;
					}
					return 0;
				}
			}
			return 0;
		}
	}
	return 0;
}


int state_create(lua_State* L)
{
	if(lua_isnumber(L,1))
	{
		// simply return the integer that got passed in
		// (that's as good a savestate object as any for a numbered savestate slot)
		lua_settop(L,1);
		return 1;
	}

	int len = GENESIS_STATE_LENGTH;
	if (SegaCD_Started) len += SEGACD_LENGTH_EX;
	if (_32X_Started) len += G32X_LENGTH_EX;
	if (!((Genesis_Started)||(SegaCD_Started)||(_32X_Started)))
		len += max(SEGACD_LENGTH_EX, G32X_LENGTH_EX);

	// allocate the in-memory/anonymous savestate
	unsigned char* stateBuffer = (unsigned char*)lua_newuserdata(L, len);
	stateBuffer[0] = 0;

	return 1;
}

// savestate.save(location [, option])
// saves the current emulation state to the given location
// you can pass in either a savestate file number (an integer),
// OR you can pass in a savestate object that was returned by savestate.create()
// if option is "quiet" then any warning messages will be suppressed
// if option is "userdataonly" then the state will not actually be loaded, but load callbacks will still get called and supplied with the data saved by save callbacks (see savestate.registerload()/savestate.registersave())
int state_save(lua_State* L)
{
	const char* option = (lua_type(L,2) == LUA_TSTRING) ? lua_tostring(L,2) : NULL;
	if(option)
	{
		if(!stricmp(option, "quiet")) // I'm not sure if saving can generate warning messages, but we might as well support suppressing them should they turn out to exist
			g_disableStatestateWarnings = true;
		else if(!stricmp(option, "userdataonly"))
			g_onlyCallSavestateCallbacks = true;
	}
	struct Scope { ~Scope(){ g_disableStatestateWarnings = false; g_onlyCallSavestateCallbacks = false; } } scope; // needs to run even if the following code throws an exception... maybe I should have put this in a "finally" block instead, but this project seems to have something against using the "try" statement

	int type = lua_type(L,1);
	switch(type)
	{
		case LUA_TNUMBER: // numbered save file
		default:
		{
			int stateNumber = luaL_checkinteger(L,1);
			Set_Current_State(stateNumber, false,false);
			char Name [1024] = {0};
			Get_State_File_Name(Name);
			Save_State(Name);
		}	return 0;
		case LUA_TUSERDATA: // in-memory save slot
		{
			unsigned char* stateBuffer = (unsigned char*)lua_touserdata(L,1);
			if(stateBuffer)
				Save_State_To_Buffer(stateBuffer);
		}	return 0;
	}
}

// savestate.load(location [, option])
// loads the current emulation state from the given location
// you can pass in either a savestate file number (an integer),
// OR you can pass in a savestate object that was returned by savestate.create() and has already saved to with savestate.save()
// if option is "quiet" then any warning messages will be suppressed
// if option is "userdataonly" then the state will not actually be loaded, but load callbacks will still get called and supplied with the data saved by save callbacks (see savestate.registerload()/savestate.registersave())
int state_load(lua_State* L)
{
	const char* option = (lua_type(L,2) == LUA_TSTRING) ? lua_tostring(L,2) : NULL;
	if(option)
	{
		if(!stricmp(option, "quiet"))
			g_disableStatestateWarnings = true;
		else if(!stricmp(option, "userdataonly"))
			g_onlyCallSavestateCallbacks = true;
	}
	struct Scope { ~Scope(){ g_disableStatestateWarnings = false; g_onlyCallSavestateCallbacks = false; } } scope; // needs to run even if the following code throws an exception... maybe I should have put this in a "finally" block instead, but this project seems to have something against using the "try" statement

	g_disableStatestateWarnings = lua_toboolean(L,2) != 0;

	int type = lua_type(L,1);
	switch(type)
	{
		case LUA_TNUMBER: // numbered save file
		default:
		{
			int stateNumber = luaL_checkinteger(L,1);
			Set_Current_State(stateNumber, false,!g_disableStatestateWarnings);
			char Name [1024] = {0};
			Get_State_File_Name(Name);
			Load_State(Name);
		}	return 0;
		case LUA_TUSERDATA: // in-memory save slot
		{
			unsigned char* stateBuffer = (unsigned char*)lua_touserdata(L,1);
			if(stateBuffer)
			{
				if(stateBuffer[0])
					Load_State_From_Buffer(stateBuffer);
				else // the first byte of a valid savestate is never 0
					luaL_error(L, "attempted to load an anonymous savestate before saving it");
			}
		}	return 0;
	}
}

static const struct ButtonDesc
{
	unsigned short controllerNum;
	unsigned short bit;
	const char* name;
}
s_buttonDescs [] =
{
	{1, 0, "up"},
	{1, 1, "down"},
	{1, 2, "left"},
	{1, 3, "right"},
	{1, 4, "A"},
	{1, 5, "B"},
	{1, 6, "C"},
	{1, 7, "start"},
	{1, 32, "X"},
	{1, 33, "Y"},
	{1, 34, "Z"},
	{1, 35, "mode"},
	{2, 24, "up"},
	{2, 25, "down"},
	{2, 26, "left"},
	{2, 27, "right"},
	{2, 28, "A"},
	{2, 29, "B"},
	{2, 30, "C"},
	{2, 31, "start"},
	{2, 36, "X"},
	{2, 37, "Y"},
	{2, 38, "Z"},
	{2, 39, "mode"},
	{0x1B, 8, "up"},
	{0x1B, 9, "down"},
	{0x1B, 10, "left"},
	{0x1B, 11, "right"},
	{0x1B, 12, "A"},
	{0x1B, 13, "B"},
	{0x1B, 14, "C"},
	{0x1B, 15, "start"},
	{0x1C, 16, "up"},
	{0x1C, 17, "down"},
	{0x1C, 18, "left"},
	{0x1C, 19, "right"},
	{0x1C, 20, "A"},
	{0x1C, 21, "B"},
	{0x1C, 22, "C"},
	{0x1C, 23, "start"},
};

int joy_getArgControllerNum(lua_State* L, int& index)
{
	int controllerNumber;
	int type = lua_type(L,index);
	if(type == LUA_TSTRING || type == LUA_TNUMBER)
	{
		controllerNumber = 0;
		if(type == LUA_TSTRING)
		{
			const char* str = lua_tostring(L,index);
			if(!stricmp(str, "1C"))
				controllerNumber = 0x1C;
			else if(!stricmp(str, "1B"))
				controllerNumber = 0x1B;
			else if(!stricmp(str, "1A"))
				controllerNumber = 0x1A;
		}
		if(!controllerNumber)
			controllerNumber = luaL_checkinteger(L,index);
		index++;
	}
	else
	{
		// argument omitted; default to controller 1
		controllerNumber = 1;
	}

	if(controllerNumber == 0x1A)
		controllerNumber = 1;
	if(controllerNumber != 1 && controllerNumber != 2 && controllerNumber != 0x1B && controllerNumber != 0x1C)
		luaL_error(L, "controller number must be 1, 2, '1B', or '1C'");

	return controllerNumber;
}


// joypad.set(controllerNum = 1, inputTable)
// controllerNum can be 1, 2, '1B', or '1C'
int joy_set(lua_State* L)
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	luaL_checktype(L, index, LUA_TTABLE);

	int input = ~0;
	int mask = 0;

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			lua_getfield(L, index, bd.name);
			if (!lua_isnil(L,-1))
			{
				bool pressed = lua_toboolean(L,-1) != 0;
				int bitmask = ((long long)1 << bd.bit);
				if(pressed)
					input &= ~bitmask;
				else
					input |= bitmask;
				mask |= bitmask;
			}
			lua_pop(L,1);
		}
	}

	SetNextInputCondensed(input, mask);

	return 0;
}

// joypad.get(controllerNum = 1)
// controllerNum can be 1, 2, '1B', or '1C'
int joy_get_internal(lua_State* L, bool reportUp, bool reportDown)
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	lua_newtable(L);

	long long input = GetCurrentInputCondensed();

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			bool pressed = (input & ((long long)1<<bd.bit)) == 0;
			if((pressed && reportDown) || (!pressed && reportUp))
			{
				lua_pushboolean(L, pressed);
				lua_setfield(L, -2, bd.name);
			}
		}
	}

	return 1;
}
// joypad.get(int controllerNumber = 1)
// returns a table of every game button,
// true meaning currently-held and false meaning not-currently-held
// this WILL read input from a currently-playing movie
int joy_get(lua_State* L)
{
	return joy_get_internal(L, true, true);
}
// joypad.getdown(int controllerNumber = 1)
// returns a table of every game button that is currently held
int joy_getdown(lua_State* L)
{
	return joy_get_internal(L, false, true);
}
// joypad.getup(int controllerNumber = 1)
// returns a table of every game button that is not currently held
int joy_getup(lua_State* L)
{
	return joy_get_internal(L, true, false);
}

// joypad.peek(controllerNum = 1)
// controllerNum can be 1, 2, '1B', or '1C'
int joy_peek_internal(lua_State* L, bool reportUp, bool reportDown)
{
	int index = 1;
	int controllerNumber = joy_getArgControllerNum(L, index);

	lua_newtable(L);

	long long input = PeekInputCondensed();

	for(int i = 0; i < sizeof(s_buttonDescs)/sizeof(*s_buttonDescs); i++)
	{
		const ButtonDesc& bd = s_buttonDescs[i];
		if(bd.controllerNum == controllerNumber)
		{
			bool pressed = (input & ((long long)1<<bd.bit)) == 0;
			if((pressed && reportDown) || (!pressed && reportUp))
			{
				lua_pushboolean(L, pressed);
				lua_setfield(L, -2, bd.name);
			}
		}
	}

	return 1;
}

// joypad.peek(int controllerNumber = 1)
// returns a table of every game button,
// true meaning currently-held and false meaning not-currently-held
// peek checks which joypad buttons are physically pressed, so it will NOT read input from a playing movie, it CAN read mid-frame input, and it will NOT pay attention to stuff like autofire or autohold or disabled L+R/U+D
int joy_peek(lua_State* L)
{
	return joy_peek_internal(L, true, true);
}
// joypad.peekdown(int controllerNumber = 1)
// returns a table of every game button that is currently held (according to what joypad.peek() would return)
int joy_peekdown(lua_State* L)
{
	return joy_peek_internal(L, false, true);
}
// joypad.peekup(int controllerNumber = 1)
// returns a table of every game button that is not currently held (according to what joypad.peek() would return)
int joy_peekup(lua_State* L)
{
	return joy_peek_internal(L, true, false);
}


static const struct ColorMapping
{
	const char* name;
	int value;
}
s_colorMapping [] =
{
	{"white",     0xFFFFFFFF},
	{"black",     0x000000FF},
	{"clear",     0x00000000},
	{"gray",      0x7F7F7FFF},
	{"grey",      0x7F7F7FFF},
	{"red",       0xFF0000FF},
	{"orange",    0xFF7F00FF},
	{"yellow",    0xFFFF00FF},
	{"chartreuse",0x7FFF00FF},
	{"green",     0x00FF00FF},
	{"teal",      0x00FF7FFF},
	{"cyan" ,     0x00FFFFFF},
	{"blue",      0x0000FFFF},
	{"purple",    0x7F00FFFF},
	{"magenta",   0xFF00FFFF},
};

inline int getcolor_unmodified(lua_State *L, int idx, int defaultColor)
{
	int type = lua_type(L,idx);
	switch(type)
	{
		case LUA_TNUMBER:
		{
			return lua_tointeger(L,idx);
		}	break;
		case LUA_TSTRING:
		{
			const char* str = lua_tostring(L,idx);
			if(*str == '#')
			{
				int color;
				sscanf(str+1, "%X", &color);
				int len = strlen(str+1);
				int missing = max(0, 8-len);
				color <<= missing << 2;
				if(missing >= 2) color |= 0xFF;
				return color;
			}
			else for(int i = 0; i<sizeof(s_colorMapping)/sizeof(*s_colorMapping); i++)
			{
				if(!stricmp(str,s_colorMapping[i].name))
					return s_colorMapping[i].value;
			}
			if(!strnicmp(str, "rand", 4))
				return ((rand()*255/RAND_MAX) << 8) | ((rand()*255/RAND_MAX) << 16) | ((rand()*255/RAND_MAX) << 24) | 0xFF;
		}	break;
		case LUA_TTABLE:
		{
			int color = 0xFF;
			lua_pushnil(L); // first key
			int keyIndex = lua_gettop(L);
			int valueIndex = keyIndex + 1;
			bool first = true;
			while(lua_next(L, idx))
			{
				bool keyIsString = (lua_type(L, keyIndex) == LUA_TSTRING);
				bool valIsNumber = (lua_type(L, valueIndex) == LUA_TNUMBER);
				if(keyIsString)
				{
					const char* key = lua_tostring(L, keyIndex);
					int value = lua_tointeger(L, valueIndex);
					if(value < 0) value = 0;
					if(value > 255) value = 255;
					switch(tolower(*key))
					{
					case 'r':
						color |= value << 24;
						break;
					case 'g':
						color |= value << 16;
						break;
					case 'b':
						color |= value << 8;
						break;
					case 'a':
						color = (color & ~0xFF) | value;
						break;
					}
				}
				lua_pop(L, 1);
			}
			return color;
		}	break;
	}
	return defaultColor;
}
int getcolor(lua_State *L, int idx, int defaultColor)
{
	int color = getcolor_unmodified(L, idx, defaultColor);
	LuaContextInfo& info = GetCurrentInfo();
	if(info.transparencyModifier != 255)
	{
		int alpha = (((color & 0xFF) * info.transparencyModifier) / 255);
		if(alpha > 255) alpha = 255;
		color = (color & ~0xFF) | alpha;
	}
	return color;
}

int gui_text(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0; // we have to wait until later to call this function because gens hasn't emulated the next frame yet
		          // (the only way to avoid this deferring is to be in a gui.register or gens.registerafter callback)

	int x = luaL_checkinteger(L,1) & 0xFFFF;
	int y = luaL_checkinteger(L,2) & 0xFFFF;
	const char* str = toCString(L,3); // better than using luaL_checkstring here (more permissive)
	
	if(str && *str)
	{
		int foreColor = getcolor(L,4,0xFFFFFFFF);
		int backColor = getcolor(L,5,0x000000FF);
		PutText2(str, x, y, foreColor, backColor);
	}

	return 0;
}
int gui_box(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x1 = luaL_checkinteger(L,1) & 0xFFFF;
	int y1 = luaL_checkinteger(L,2) & 0xFFFF;
	int x2 = luaL_checkinteger(L,3) & 0xFFFF;
	int y2 = luaL_checkinteger(L,4) & 0xFFFF;
	int fillcolor = getcolor(L,5,0xFFFFFF3F);
	int outlinecolor = getcolor(L,6,fillcolor&0xFF);

	DrawBoxPP2 (x1, y1, x2, y2, fillcolor, outlinecolor);

	return 0;
}
// gui.setpixel(x,y,color)
// color can be a RGB web color like '#ff7030', or with alpha RGBA like '#ff703060'
//   or it can be an RGBA hex number like 0xFF703060
//   or it can be a preset color like 'red', 'orange', 'blue', 'white', etc.
int gui_pixel(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x = luaL_checkinteger(L,1) & 0xFFFF;
	int y = luaL_checkinteger(L,2) & 0xFFFF;
	int color = getcolor(L,3,0xFFFFFFFF);
	int color32 = color>>8;
	int color16 = DrawUtil::Pix32To16(color32);
	int Opac = color & 0xFF;

	if(Opac)
		Pixel(x, y, color32, color16, 0, Opac);

	return 0;
}
// r,g,b = gui.getpixel(x,y)
int gui_getpixel(lua_State* L)
{
	int x = luaL_checkinteger(L,1);
	int y = luaL_checkinteger(L,2);

	int xres = ((VDP_Reg.Set4 & 0x1) || Debug || !Game || !FrameCount) ? 320 : 256;
	int yres = ((VDP_Reg.Set2 & 0x8) || Debug || !Game || !FrameCount) ? 240 : 224;

	x = max(0,min(xres,x));
	y = max(0,min(yres,y));

	int off = (y * 336) + x + 8;

	int color;
	if (Bits32)
		color = MD_Screen32[off];
	else
		color = DrawUtil::Pix16To32(MD_Screen[off]);

	int b = (color & 0x000000FF);
	int g = (color & 0x0000FF00) >> 8;
	int r = (color & 0x00FF0000) >> 16;

	lua_pushinteger(L, r);
	lua_pushinteger(L, g);
	lua_pushinteger(L, b);

	return 3;
}
int gui_line(lua_State* L)
{
	if(DeferGUIFuncIfNeeded(L))
		return 0;

	int x1 = luaL_checkinteger(L,1) & 0xFFFF;
	int y1 = luaL_checkinteger(L,2) & 0xFFFF;
	int x2 = luaL_checkinteger(L,3) & 0xFFFF;
	int y2 = luaL_checkinteger(L,4) & 0xFFFF;
	int color = getcolor(L,5,0xFFFFFFFF);
	int color32 = color>>8;
	int color16 = DrawUtil::Pix32To16(color32);
	int Opac = color & 0xFF;

	if(Opac)
	{
		int skipFirst = lua_toboolean(L,6);
		DrawLine(x1, y1, x2, y2, color32, color16, 0, Opac, skipFirst);
	}

	return 0;
}

// gui.opacity(number alphaValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely transparent, 1.0 is completely opaque
// non-integer values are supported and meaningful, as are values greater than 1.0
// it is not necessary to use this function to get transparency (or the less-recommended gui.transparency() either),
// because you can provide an alpha value in the color argument of each draw call.
// however, it can be convenient to be able to globally modify the drawing transparency
int gui_setopacity(lua_State* L)
{
	lua_Number opacF = luaL_checknumber(L,1);
	opacF *= 255.0;
	if(opacF < 0) opacF = 0;
	int opac;
	lua_number2int(opac, opacF);
	LuaContextInfo& info = GetCurrentInfo();
	info.transparencyModifier = opac;
	return 0;
}

// gui.transparency(number transparencyValue)
// sets the transparency of subsequent draw calls
// 0.0 is completely opaque, 4.0 is completely transparent
// non-integer values are supported and meaningful, as are values less than 0.0
// this is a legacy function, and the range is from 0 to 4 solely for this reason
// it does the exact same thing as gui.opacity() but with a different argument range
int gui_settransparency(lua_State* L)
{
	lua_Number transp = luaL_checknumber(L,1);
	lua_Number opacF = 4 - transp;
	opacF *= 255.0 / 4.0;
	if(opacF < 0) opacF = 0;
	int opac;
	lua_number2int(opac, opacF);
	LuaContextInfo& info = GetCurrentInfo();
	info.transparencyModifier = opac;
	return 0;
}

int gens_openscript(lua_State* L)
{
	const char* filename = lua_isstring(L,1) ? lua_tostring(L,1) : NULL;
	const char* errorMsg = GensOpenScript(filename);
	if(errorMsg)
		luaL_error(L, errorMsg);
    return 0;
}

int gens_getframecount(lua_State* L)
{
	lua_pushinteger(L, FrameCount);
	return 1;
}
int gens_getlagcount(lua_State* L)
{
	lua_pushinteger(L, LagCountPersistent);
	return 1;
}
int movie_getlength(lua_State* L)
{
	lua_pushinteger(L, MainMovie.LastFrame);
	return 1;
}
int movie_isactive(lua_State* L)
{
	lua_pushboolean(L, MainMovie.File != NULL);
	return 1;
}
int movie_rerecordcount(lua_State* L)
{
	lua_pushinteger(L, MainMovie.NbRerecords);
	return 1;
}
int movie_getreadonly(lua_State* L)
{
	lua_pushboolean(L, MainMovie.ReadOnly);
	return 1;
}
int movie_setreadonly(lua_State* L)
{
	bool readonly = lua_toboolean(L,1) != 0;
	MainMovie.ReadOnly = readonly;
	return 0;
}
int movie_isrecording(lua_State* L)
{
	lua_pushboolean(L, MainMovie.Status == MOVIE_RECORDING);
	return 1;
}
int movie_isplaying(lua_State* L)
{
	lua_pushboolean(L, MainMovie.Status == MOVIE_PLAYING);
	return 1;
}
int movie_getmode(lua_State* L)
{
	switch(MainMovie.Status)
	{
	case MOVIE_PLAYING:
		lua_pushstring(L, "playback");
		break;
	case MOVIE_RECORDING:
		lua_pushstring(L, "record");
		break;
	case MOVIE_FINISHED:
		lua_pushstring(L, "finished");
		break;
	default:
		lua_pushnil(L);
		break;
	}
	return 1;
}
int movie_getname(lua_State* L)
{
	lua_pushstring(L, MainMovie.FileName);
	return 1;
}
// movie.play() -- plays a movie of the user's choice
// movie.play(filename) -- starts playing a particular movie
// throws an error (with a description) if for whatever reason the movie couldn't be played
int movie_play(lua_State *L)
{
	const char* filename = lua_isstring(L,1) ? lua_tostring(L,1) : NULL;
	const char* errorMsg = GensPlayMovie(filename, true);
	if(errorMsg)
		luaL_error(L, errorMsg);
    return 0;
} 
int movie_replay(lua_State *L)
{
	if(MainMovie.File)
		GensReplayMovie();
	else
		luaL_error(L, "it is invalid to call movie_replay when no movie open.");
    return 0;
} 
int movie_close(lua_State* L)
{
	CloseMovieFile(&MainMovie);
	return 0;
}

int sound_clear(lua_State* L)
{
	Clear_Sound_Buffer();
	return 0;
}

#ifdef _WIN32
const char* s_keyToName[256] =
{
	NULL,
	"leftclick",
	"rightclick",
	NULL,
	"middleclick",
	NULL,
	NULL,
	NULL,
	"backspace",
	"tab",
	NULL,
	NULL,
	NULL,
	"enter",
	NULL,
	NULL,
	"shift", // 0x10
	"control",
	"alt",
	"pause",
	"capslock",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"escape",
	NULL,
	NULL,
	NULL,
	NULL,
	"space", // 0x20
	"pageup",
	"pagedown",
	"end",
	"home",
	"left",
	"up",
	"right",
	"down",
	NULL,
	NULL,
	NULL,
	NULL,
	"insert",
	"delete",
	NULL,
	"0","1","2","3","4","5","6","7","8","9",
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	"A","B","C","D","E","F","G","H","I","J",
	"K","L","M","N","O","P","Q","R","S","T",
	"U","V","W","X","Y","Z",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numpad0","numpad1","numpad2","numpad3","numpad4","numpad5","numpad6","numpad7","numpad8","numpad9",
	"numpad*","numpad+",
	NULL,
	"numpad-","numpad.","numpad/",
	"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",
	"F13","F14","F15","F16","F17","F18","F19","F20","F21","F22","F23","F24",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"numlock",
	"scrolllock",
	NULL, // 0x92
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xB9
	"semicolon",
	"plus",
	"comma",
	"minus",
	"period",
	"slash",
	"tilde",
	NULL, // 0xC1
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	NULL, // 0xDA
	"leftbracket",
	"backslash",
	"rightbracket",
	"quote",
};
#endif


// input.get()
// takes no input, returns a lua table of entries representing the current input state,
// independent of the joypad buttons the emulated game thinks are pressed
// for example:
//   if the user is holding the W key and the left mouse button
//   and has the mouse at the bottom-right corner of the game screen,
//   then this would return {W=true, leftclick=true, xmouse=319, ymouse=223}
int input_getcurrentinputstatus(lua_State* L)
{
	lua_newtable(L);

#ifdef _WIN32
	// keyboard and mouse button status
	{
		unsigned char keys [256];
		if(!BackgroundInput)
		{
			if(GetKeyboardState(keys))
			{
				for(int i = 1; i < 255; i++)
				{
					int mask = (i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL) ? 0x01 : 0x80;
					if(keys[i] & mask)
					{
						const char* name = s_keyToName[i];
						if(name)
						{
							lua_pushboolean(L, true);
							lua_setfield(L, -2, name);
						}
					}
				}
			}
		}
		else // use a slightly different method that will detect background input:
		{
			for(int i = 1; i < 255; i++)
			{
				const char* name = s_keyToName[i];
				if(name)
				{
					int active;
					if(i == VK_CAPITAL || i == VK_NUMLOCK || i == VK_SCROLL)
						active = GetKeyState(i) & 0x01;
					else
						active = GetAsyncKeyState(i) & 0x8000;
					if(active)
					{
						lua_pushboolean(L, true);
						lua_setfield(L, -2, name);
					}
				}
			}
		}
	}
	// mouse position in game screen pixel coordinates
	{
		POINT point;
		RECT rect, srcRectUnused;
		float xRatioUnused, yRatioUnused;
		int depUnused;
		GetCursorPos(&point);
		ScreenToClient(HWnd, &point);
		GetClientRect(HWnd, &rect);
		void CalculateDrawArea(int Render_Mode, RECT& RectDest, RECT& RectSrc, float& Ratio_X, float& Ratio_Y, int& Dep);
		CalculateDrawArea(Full_Screen ? Render_FS : Render_W, rect, srcRectUnused, xRatioUnused, yRatioUnused, depUnused);
		int xres = ((VDP_Reg.Set4 & 0x1) || Debug || !Game || !FrameCount) ? 320 : 256;
		int yres = ((VDP_Reg.Set2 & 0x8) || Debug || !Game || !FrameCount) ? 240 : 224;
		int x = ((point.x - rect.left) * xres) / max(1, rect.right - rect.left);
		int y = ((point.y - rect.top) * yres) / max(1, rect.bottom - rect.top);
		lua_pushinteger(L, x);
		lua_setfield(L, -2, "xmouse");
		lua_pushinteger(L, y);
		lua_setfield(L, -2, "ymouse");
	}
#else
	// NYI (well, return an empty table)
#endif

	return 1;
}


// resets our "worry" counter of the Lua state
int dontworry(LuaContextInfo& info)
{
	info.worryCount = 0;
	return 0;
}

static const struct luaL_reg genslib [] =
{
	{"frameadvance", gens_frameadvance},
	{"speedmode", gens_speedmode},
	{"wait", gens_wait},
	{"pause", gens_pause},
	{"emulateframe", gens_emulateframe},
	{"emulateframefastnoskipping", gens_emulateframefastnoskipping},
	{"emulateframefast", gens_emulateframefast},
	{"emulateframeinvisible", gens_emulateframeinvisible},
	{"redraw", gens_redraw},
	{"framecount", gens_getframecount},
	{"lagcount", gens_getlagcount},
	{"registerbefore", gens_registerbefore},
	{"registerafter", gens_registerafter},
	{"registerexit", gens_registerexit},
	{"message", gens_message},
	{"openscript", gens_openscript},
	{"print", print},
	{NULL, NULL}
};
static const struct luaL_reg guilib [] =
{
	{"register", gui_register},
	{"text", gui_text},
	{"box", gui_box},
	{"line", gui_line},
	{"pixel", gui_pixel},
	{"getpixel", gui_getpixel},
	{"opacity", gui_setopacity},
	{"transparency", gui_settransparency},
	// alternative names
	{"drawtext", gui_text},
	{"drawbox", gui_box},
	{"drawline", gui_line},
	{"drawpixel", gui_pixel},
	{"setpixel", gui_pixel},
	{"writepixel", gui_pixel},
	{"readpixel", gui_getpixel},
	{"rect", gui_box},
	{"drawrect", gui_box},
	{NULL, NULL}
};
static const struct luaL_reg statelib [] =
{
	{"create", state_create},
	{"save", state_save},
	{"load", state_load},
	{"registersave", state_registersave},
	{"registerload", state_registerload},
	{NULL, NULL}
};
static const struct luaL_reg memorylib [] =
{
	{"readbyte", memory_readbyte},
	{"readbyteunsigned", memory_readbyte},
	{"readbytesigned", memory_readbytesigned},
	{"readword", memory_readword},
	{"readwordunsigned", memory_readword},
	{"readwordsigned", memory_readwordsigned},
	{"readdword", memory_readdword},
	{"readdwordunsigned", memory_readdword},
	{"readdwordsigned", memory_readdwordsigned},
	{"readbyterange", memory_readbyterange},
	{"writebyte", memory_writebyte},
	{"writeword", memory_writeword},
	{"writedword", memory_writedword},
	{"isvalid", memory_isvalid},
	{"getregister", memory_getregister},
	{"setregister", memory_setregister},
	// alternate naming scheme for word and double-word
	{"readshort", memory_readword},
	{"readshortunsigned", memory_readword},
	{"readshortsigned", memory_readwordsigned},
	{"readlong", memory_readdword},
	{"readlongunsigned", memory_readdword},
	{"readlongsigned", memory_readdwordsigned},
	{"writeshort", memory_writeword},
	{"writelong", memory_writedword},

	//main 68000 memory hooks
	{"register", memory_registerM68Kwrite},
	{"registerwrite", memory_registerM68Kwrite},
	{"registerread", memory_registerM68Kread},
	{"registerexec", memory_registerM68Kexec},

	//full names for main 68000 memory hooks
	{"registerM68K", memory_registerM68Kwrite},
	{"registerM68Kwrite", memory_registerM68Kwrite},
	{"registerM68Kread", memory_registerM68Kread},
	{"registerM68Kexec", memory_registerM68Kexec},

	//alternate names for main 68000 memory hooks
	{"registergen", memory_registerM68Kwrite},
	{"registergenwrite", memory_registerM68Kwrite},
	{"registergenread", memory_registerM68Kread},
	{"registergenexec", memory_registerM68Kexec},

	//sub 68000 (segaCD) memory hooks
	{"registerS68K", memory_registerS68Kwrite},
	{"registerS68Kwrite", memory_registerS68Kwrite},
	{"registerS68Kread", memory_registerS68Kread},
	{"registerS68Kexec", memory_registerS68Kexec},

	//alternate names for sub 68000 (segaCD) memory hooks
	{"registerCD", memory_registerS68Kwrite},
	{"registerCDwrite", memory_registerS68Kwrite},
	{"registerCDread", memory_registerS68Kread},
	{"registerCDexec", memory_registerS68Kexec},

//	//Super-H 2 (32X) memory hooks
//	{"registerSH2", memory_registerSH2write},
//	{"registerSH2write", memory_registerSH2write},
//	{"registerSH2read", memory_registerSH2read},
//	{"registerSH2exec", memory_registerSH2PC},
//	{"registerSH2PC", memory_registerSH2PC},

//	//alternate names for Super-H 2 (32X) memory hooks
//	{"register32X", memory_registerSH2write},
//	{"register32Xwrite", memory_registerSH2write},
//	{"register32Xread", memory_registerSH2read},
//	{"register32Xexec", memory_registerSH2PC},
//	{"register32XPC", memory_registerSH2PC},

//	//Z80 (sound controller) memory hooks
//	{"registerZ80", memory_registerZ80write},
//	{"registerZ80write", memory_registerZ80write},
//	{"registerZ80read", memory_registerZ80read},
//	{"registerZ80PC", memory_registerZ80PC},

	{NULL, NULL}
};
static const struct luaL_reg joylib [] =
{
	{"get", joy_get},
	{"getdown", joy_getdown},
	{"getup", joy_getup},
	{"peek", joy_peek},
	{"peekdown", joy_peekdown},
	{"peekup", joy_peekup},
	{"set", joy_set},
	// alternative names
	{"read", joy_get},
	{"write", joy_set},
	{"readdown", joy_getdown},
	{"readup", joy_getup},
	{NULL, NULL}
};
static const struct luaL_reg inputlib [] =
{
	{"get", input_getcurrentinputstatus},
	{"registerhotkey", input_registerhotkey},
	// alternative names
	{"read", input_getcurrentinputstatus},
	{NULL, NULL}
};
static const struct luaL_reg movielib [] =
{
	{"length", movie_getlength},
	{"active", movie_isactive},
	{"recording", movie_isrecording},
	{"playing", movie_isplaying},
	{"mode", movie_getmode},
	{"name", movie_getname},
	{"getname", movie_getname},
	{"rerecordcount", movie_rerecordcount},
	{"readonly", movie_getreadonly},
	{"getreadonly", movie_getreadonly},
	{"setreadonly", movie_setreadonly},
	{"playback", movie_play},
	{"play", movie_play},
	{"replay", movie_replay},
	{"stop", movie_close},
	{"framecount", gens_getframecount}, // for those familiar with other emulators that have movie.framecount() instead of emulatorname.framecount()
	// alternative names
	{"open", movie_play},
	{"close", movie_close},
	{NULL, NULL}
};
static const struct luaL_reg soundlib [] =
{
	{"clear", sound_clear},
	{NULL, NULL}
};

void ResetInfo(LuaContextInfo& info)
{
	info.L = NULL;
	info.started = false;
	info.running = false;
	info.returned = false;
	info.crashed = false;
	info.restart = false;
	info.restartLater = false;
	info.worryCount = 0;
	info.stopWorrying = false;
	info.panic = false;
	info.ranExit = false;
	info.numDeferredGUIFuncs = 0;
	info.ranFrameAdvance = false;
	info.transparencyModifier = 255;
	info.speedMode = SPEEDMODE_NORMAL;
	info.guiFuncsNeedDeferring = false;
	info.dataSaveKey = 0;
	info.dataLoadKey = 0;
	info.dataSaveLoadKeySet = false;
}

void OpenLuaContext(int uid, void(*print)(int uid, const char* str), void(*onstart)(int uid), void(*onstop)(int uid, bool statusOK))
{
	LuaContextInfo* newInfo = new LuaContextInfo();
	ResetInfo(*newInfo);
	newInfo->print = print;
	newInfo->onstart = onstart;
	newInfo->onstop = onstop;
	luaContextInfo[uid] = newInfo;
}

static const char* PathToFilename(const char* path)
{
	const char* slash1 = strrchr(path, '\\');
	const char* slash2 = strrchr(path, '/');
	if(slash1) slash1++;
	if(slash2) slash2++;
	const char* rv = path;
	rv = max(rv, slash1);
	rv = max(rv, slash2);
	if(!rv) rv = "";
	return rv;
}


void RunLuaScriptFile(int uid, const char* filenameCStr)
{
	if(luaContextInfo.find(uid) == luaContextInfo.end())
		return;
	StopLuaScript(uid);

	LuaContextInfo& info = *luaContextInfo[uid];

#ifdef USE_INFO_STACK
	infoStack.insert(infoStack.begin(), &info);
	struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope; // doing it like this makes sure that the info stack gets cleaned up even if an exception is thrown
#endif

	info.nextFilename = filenameCStr;

	if(info.running)
	{
		// it's a little complicated, but... the call to luaL_dofile below
		// could call a C function that calls this very function again
		// additionally, if that happened then the above call to StopLuaScript
		// probably couldn't stop the script yet, so instead of continuing,
		// we'll set a flag that tells the first call of this function to loop again
		// when the script is able to stop safely
		info.restart = true;
		return;
	}

	do
	{
		std::string filename = info.nextFilename;

		lua_State* L = lua_open();
#ifndef USE_INFO_STACK
		luaStateToContextMap[L] = &info;
#endif
		luaStateToUIDMap[L] = uid;
		ResetInfo(info);
		info.L = L;
		info.guiFuncsNeedDeferring = true;
		info.lastFilename = filename;

		SetSaveKey(info, PathToFilename(filename.c_str()));
		info.dataSaveLoadKeySet = false;

		luaL_openlibs(L);
		luaL_register(L, "gens", genslib);
		luaL_register(L, "gui", guilib);
		luaL_register(L, "savestate", statelib);
		luaL_register(L, "memory", memorylib);
		luaL_register(L, "joypad", joylib); // for game input
		luaL_register(L, "input", inputlib); // for user input
		luaL_register(L, "movie", movielib);
		luaL_register(L, "sound", soundlib);
		
		// register a few utility functions outside of libraries (in the global namespace)
		lua_register(L, "print", print);
		lua_register(L, "tostring", tostring);
		lua_register(L, "addressof", addressof);
		lua_register(L, "copytable", copytable);
		lua_register(L, "AND", bitand);
		lua_register(L, "OR", bitor);
		lua_register(L, "XOR", bitxor);
		lua_register(L, "SHIFT", bitshift);
		lua_register(L, "BIT", bitbit);

		// register a function to periodically check for inactivity
		lua_sethook(L, LuaRescueHook, LUA_MASKCOUNT, HOOKCOUNT);

		// deferred evaluation table
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, deferredGUIIDString);

		info.started = true;
		RefreshScriptStartedStatus();
		if(info.onstart)
			info.onstart(uid);
		info.running = true;
		RefreshScriptSpeedStatus();
		info.returned = false;
		int errorcode = luaL_dofile(L,filename.c_str());
		info.running = false;
		RefreshScriptSpeedStatus();
		info.returned = true;

		if (errorcode)
		{
			info.crashed = true;
			if(info.print)
			{
				info.print(uid, lua_tostring(L,-1));
				info.print(uid, "\r\n");
			}
			else
			{
				fprintf(stderr, "%s\n", lua_tostring(L,-1));
			}
			StopLuaScript(uid);
		}
		else
		{
			Show_Genesis_Screen();
			StopScriptIfFinished(uid, true);
		}
	} while(info.restart);
}

void StopScriptIfFinished(int uid, bool justReturned)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	if(!info.returned)
		return;

	// the script has returned, but it is not necessarily done running
	// because it may have registered a function that it expects to keep getting called
	// so check if it has any registered functions and stop the script only if it doesn't

	bool keepAlive = false;
	for(int calltype = 0; calltype < LUACALL_COUNT; calltype++)
	{
		lua_State* L = info.L;
		if(L)
		{
			const char* idstring = luaCallIDStrings[calltype];
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			bool isFunction = lua_isfunction(L, -1);
			lua_pop(L, 1);

			if(isFunction)
			{
				keepAlive = true;
				break;
			}
		}
	}

	if(keepAlive)
	{
		if(justReturned)
		{
			if(info.print)
				info.print(uid, "script returned but is still running registered functions\r\n");
			else
				fprintf(stderr, "%s\n", "script returned but is still running registered functions");
		}
	}
	else
	{
		if(info.print)
			info.print(uid, "script finished running\r\n");
		else
			fprintf(stderr, "%s\n", "script finished running");

		StopLuaScript(uid);
	}
}

void RequestAbortLuaScript(int uid, const char* message)
{
	if(luaContextInfo.find(uid) == luaContextInfo.end())
		return;
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(L)
	{
		// this probably isn't the right way to do it
		// but calling luaL_error here is positively unsafe
		// (it seemingly works fine but sometimes corrupts the emulation state in colorful ways)
		// and this works pretty well and is definitely safe, so screw it
		info.L->hookcount = 1; // run hook function as soon as possible
		info.panic = true; // and call luaL_error once we're inside the hook function
		if(message)
		{
			strncpy(info.panicMessage, message, sizeof(info.panicMessage));
			info.panicMessage[sizeof(info.panicMessage)-1] = 0;
		}
		else
		{
			strcpy(info.panicMessage, "script terminated");
		}
	}
}

void SetSaveKey(LuaContextInfo& info, const char* key)
{
	info.dataSaveKey = crc32(0, (const unsigned char*)key, strlen(key));

	if(!info.dataSaveLoadKeySet)
	{
		info.dataLoadKey = info.dataSaveKey;
		info.dataSaveLoadKeySet = true;
	}
}
void SetLoadKey(LuaContextInfo& info, const char* key)
{
	info.dataLoadKey = crc32(0, (const unsigned char*)key, strlen(key));

	if(!info.dataSaveLoadKeySet)
	{
		info.dataSaveKey = info.dataLoadKey;
		info.dataSaveLoadKeySet = true;
	}
}

void CallExitFunction(int uid)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;

	if(!L)
		return;

	dontworry(info);

	// first call the registered exit function if there is one
	if(!info.ranExit)
	{
		info.ranExit = true;

#ifdef USE_INFO_STACK
		infoStack.insert(infoStack.begin(), &info);
		struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif
		lua_settop(L, 0);
		lua_getfield(L, LUA_REGISTRYINDEX, luaCallIDStrings[LUACALL_BEFOREEXIT]);
		
		if (lua_isfunction(L, -1))
		{
			bool wasRunning = info.running;
			info.running = true;
			RefreshScriptSpeedStatus();

			bool wasPanic = info.panic;
			info.panic = false; // otherwise we could barely do anything in the exit function

			int errorcode = lua_pcall(L, 0, 0, 0);

			info.panic |= wasPanic; // restore panic

			info.running = wasRunning;
			RefreshScriptSpeedStatus();
			if (errorcode)
			{
				info.crashed = true;
				if(L->errfunc || L->errorJmp)
					luaL_error(L, lua_tostring(L,-1));
				else if(info.print)
				{
					info.print(uid, lua_tostring(L,-1));
					info.print(uid, "\r\n");
				}
				else
				{
					fprintf(stderr, "%s\n", lua_tostring(L,-1));
				}
			}
		}
	}
}

void StopLuaScript(int uid)
{
	LuaContextInfo& info = *luaContextInfo[uid];

	if(info.running)
	{
		// if it's currently running then we can't stop it now without crashing
		// so the best we can do is politely request for it to go kill itself
		RequestAbortLuaScript(uid);
		return;
	}

	lua_State* L = info.L;
	if(L)
	{
		CallExitFunction(uid);

		if(info.onstop)
			info.onstop(uid, !info.crashed); // must happen before closing L and after the exit function, otherwise the final GUI state of the script won't be shown properly or at all

		if(info.started) // this check is necessary
		{
			lua_close(L);
#ifndef USE_INFO_STACK
			luaStateToContextMap.erase(L);
#endif
			luaStateToUIDMap.erase(L);
			info.L = NULL;
			info.started = false;
		}
		RefreshScriptStartedStatus();
	}
}

void CloseLuaContext(int uid)
{
	StopLuaScript(uid);
	delete luaContextInfo[uid];
	luaContextInfo.erase(uid);
}


void CallRegisteredLuaFunctions(LuaCallID calltype)
{
	assert((unsigned int)calltype < (unsigned int)LUACALL_COUNT);
	const char* idstring = luaCallIDStrings[calltype];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L && (!info.panic || calltype == LUACALL_BEFOREEXIT))
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif
			// handle deferred GUI function calls and disabling deferring when unnecessary
			if(calltype == LUACALL_AFTEREMULATIONGUI || calltype == LUACALL_AFTEREMULATION)
				info.guiFuncsNeedDeferring = false;
			if(calltype == LUACALL_AFTEREMULATIONGUI)
				CallDeferredFunctions(L, deferredGUIIDString);

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();
				int errorcode = lua_pcall(L, 0, 0, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
				{
					info.crashed = true;
					if(L->errfunc || L->errorJmp)
						luaL_error(L, lua_tostring(L,-1));
					else
					{
						if(info.print)
						{
							info.print(uid, lua_tostring(L,-1));
							info.print(uid, "\r\n");
						}
						else
						{
							fprintf(stderr, "%s\n", lua_tostring(L,-1));
						}
						StopLuaScript(uid);
					}
				}
			}
			else
			{
				lua_pop(L, 1);
			}

			info.guiFuncsNeedDeferring = true;
		}

		++iter;
	}
}

void CallRegisteredLuaSaveFunctions(int savestateNumber, LuaSaveData& saveData)
{
	const char* idstring = luaCallIDStrings[LUACALL_BEFORESAVE];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L)
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();
				lua_pushinteger(L, savestateNumber);
				int errorcode = lua_pcall(L, 1, LUA_MULTRET, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
				{
					info.crashed = true;
					if(L->errfunc || L->errorJmp)
						luaL_error(L, lua_tostring(L,-1));
					else
					{
						if(info.print)
						{
							info.print(uid, lua_tostring(L,-1));
							info.print(uid, "\r\n");
						}
						else
						{
							fprintf(stderr, "%s\n", lua_tostring(L,-1));
						}
						StopLuaScript(uid);
					}
				}
				saveData.SaveRecord(uid, info.dataSaveKey);
			}
			else
			{
				lua_pop(L, 1);
			}
		}

		++iter;
	}
}


void CallRegisteredLuaLoadFunctions(int savestateNumber, const LuaSaveData& saveData)
{
	const char* idstring = luaCallIDStrings[LUACALL_AFTERLOAD];

	std::map<int, LuaContextInfo*>::iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		lua_State* L = info.L;
		if(L)
		{
#ifdef USE_INFO_STACK
			infoStack.insert(infoStack.begin(), &info);
			struct Scope { ~Scope(){ infoStack.erase(infoStack.begin()); } } scope;
#endif

			lua_settop(L, 0);
			lua_getfield(L, LUA_REGISTRYINDEX, idstring);
			
			if (lua_isfunction(L, -1))
			{
				bool wasRunning = info.running;
				info.running = true;
				RefreshScriptSpeedStatus();

				lua_pushinteger(L, savestateNumber);
				saveData.LoadRecord(uid, info.dataLoadKey);
				int n = lua_gettop(L) - 1;

				int errorcode = lua_pcall(L, n, 0, 0);
				info.running = wasRunning;
				RefreshScriptSpeedStatus();
				if (errorcode)
				{
					info.crashed = true;
					if(L->errfunc || L->errorJmp)
						luaL_error(L, lua_tostring(L,-1));
					else
					{
						if(info.print)
						{
							info.print(uid, lua_tostring(L,-1));
							info.print(uid, "\r\n");
						}
						else
						{
							fprintf(stderr, "%s\n", lua_tostring(L,-1));
						}
						StopLuaScript(uid);
					}
				}
			}
			else
			{
				lua_pop(L, 1);
			}
		}

		++iter;
	}
}
/*
template<typename T>
void PushIntegerItem(T item, std::vector<unsigned char>& output)
{
	unsigned int value = (unsigned int)T;
	for(int i = sizeof(T); i; i--)
	{
		output.push_back(value & 0xFF);
		value >>= 8;
	}
}
*/
template<typename T>
void PushBinaryItem(T item, std::vector<unsigned char>& output)
{
	unsigned char* buf = (unsigned char*)&item;
	for(int i = sizeof(T); i; i--)
		output.push_back(*buf++);
}

static void AdvanceByteStream(const unsigned char*& data, unsigned int& remaining, int amount)
{
	data += amount;
	remaining -= amount;
}

static void LuaStackToBinaryConverter(lua_State* L, int i, std::vector<unsigned char>& output)
{
	int type = lua_type(L, i);

	// the first byte of every serialized item says what Lua type it is
	output.push_back(type & 0xFF);

	switch(type)
	{
		default:
			{
				LuaContextInfo& info = GetCurrentInfo();
				if(info.print)
				{
					char errmsg [1024];
					sprintf(errmsg, "values of type \"%s\" are not allowed to be returned from registered save functions.\r\n", luaL_typename(L,i));
					info.print(luaStateToUIDMap[L], errmsg);
				}
				else
				{
					fprintf(stderr, "values of type \"%s\" are not allowed to be returned from registered save functions.\n", luaL_typename(L,i));
				}
			}
			break;
		case LUA_TNIL:
			// no information necessary beyond the type
			break;
		case LUA_TBOOLEAN:
			// serialize as 0 or 1
			output.push_back(lua_toboolean(L,i));
			break;
		case LUA_TSTRING:
			// serialize as a 0-terminated string of characters
			{
				const char* str = lua_tostring(L,i);
				while(*str)
					output.push_back(*str++);
				output.push_back('\0');
			}
			break;
		case LUA_TNUMBER:
			// serialize as the binary data of the number as a double
			// (which should be using the IEEE double precision floating point standard)
			{
				double num = (double)lua_tonumber(L,i);
				PushBinaryItem(num, output);
			}
			break;
		case LUA_TTABLE:
			// serialize as a "NONE-terminated" sequence of (key,value) Lua values
			// (should work because "none" and "nil" are not valid table keys in Lua)
			// note that the structure of table references are not faithfully serialized (yet)
		{
			if(lua_checkstack(L, 4) && std::find(s_tableAddressStack.begin(), s_tableAddressStack.end(), lua_topointer(L,i)) == s_tableAddressStack.end())
			{
				s_tableAddressStack.push_back(lua_topointer(L,i));
				struct Scope { ~Scope(){ s_tableAddressStack.pop_back(); } } scope;

				lua_pushnil(L); // first key
				int keyIndex = lua_gettop(L);
				int valueIndex = keyIndex + 1;
				while(lua_next(L, i))
				{
					LuaStackToBinaryConverter(L, keyIndex, output);
					LuaStackToBinaryConverter(L, valueIndex, output);
					lua_pop(L, 1);
				}
			}
			output.push_back(LUA_TNONE); // terminator (not a valid key)
		}	break;
	}
}

// complements LuaStackToBinaryConverter
void BinaryToLuaStackConverter(lua_State* L, const unsigned char*& data, unsigned int& remaining)
{
	unsigned char type = *data;
	AdvanceByteStream(data, remaining, 1);

	switch(type)
	{
		default:
			{
				LuaContextInfo& info = GetCurrentInfo();
				if(info.print)
				{
					char errmsg [1024];
					if(type < 10)
						sprintf(errmsg, "values of type \"%s\" are not allowed to be loaded into registered load functions. The save state's Lua save data file might be corrupted.\r\n", lua_typename(L,type));
					else
						sprintf(errmsg, "The save state's Lua save data file seems to be corrupted.\r\n");
					info.print(luaStateToUIDMap[L], errmsg);
				}
				else
				{
					if(type < 10)
						fprintf(stderr, "values of type \"%s\" are not allowed to be loaded into registered load functions. The save state's Lua save data file might be corrupted.\n", lua_typename(L,type));
					else
						fprintf(stderr, "The save state's Lua save data file seems to be corrupted.\n");
				}
			}
			break;
		case LUA_TNIL:
			lua_pushnil(L);
			break;
		case LUA_TBOOLEAN:
			lua_pushboolean(L, *data);
			AdvanceByteStream(data, remaining, 1);
			break;
		case LUA_TSTRING:
			lua_pushstring(L, (const char*)data);
			AdvanceByteStream(data, remaining, strlen((const char*)data) + 1);
			break;
		case LUA_TNUMBER:
			lua_pushnumber(L, *(double*)data);
			AdvanceByteStream(data, remaining, sizeof(double));
			break;
		case LUA_TTABLE:
			lua_newtable(L);
			while((unsigned char)*data != (unsigned char)LUA_TNONE)
			{
				BinaryToLuaStackConverter(L, data, remaining); // push key
				BinaryToLuaStackConverter(L, data, remaining); // push value
				lua_rawset(L, -3); // table[key] = value
			}
			AdvanceByteStream(data, remaining, 1);
			break;
	}
}

unsigned char* LuaStackToBinary(lua_State* L, unsigned int& size)
{
	std::vector<unsigned char> output;

	int n = lua_gettop(L);
	for(int i = 1; i <= n; i++)
		LuaStackToBinaryConverter(L, i, output);

	if(output.empty())
		return NULL;

	unsigned char* rv = new unsigned char [output.size()];
	memcpy(rv, &output.front(), output.size());
	size = output.size();
	return rv;
}

void BinaryToLuaStack(lua_State* L, const unsigned char* data, unsigned int size)
{
	while(size > 0)
		BinaryToLuaStackConverter(L, data, size);
}


// saves Lua stack into a record and pops it
void LuaSaveData::SaveRecord(int uid, unsigned int key)
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(!L)
		return;

	Record* cur = new Record();
	cur->key = key;
	cur->data = LuaStackToBinary(L, cur->size);
	cur->next = NULL;

	lua_settop(L,0);

	if(cur->size <= 0)
	{
		delete cur;
		return;
	}

	Record* last = recordList;
	while(last && last->next)
		last = last->next;
	if(last)
		last->next = cur;
	else
		recordList = cur;
}

// pushes a record's data onto the Lua stack
void LuaSaveData::LoadRecord(int uid, unsigned int key) const
{
	LuaContextInfo& info = *luaContextInfo[uid];
	lua_State* L = info.L;
	if(!L)
		return;

	Record* cur = recordList;
	while(cur)
	{
		if(cur->key == key)
		{
			BinaryToLuaStack(L, cur->data, cur->size);
			return;
		}
		cur = cur->next;
	}

}

void fwriteint(unsigned int value, FILE* file)
{
	for(int i=0;i<4;i++)
	{
		int w = value & 0xFF;
		fwrite(&w, 1, 1, file);
		value >>= 8;
	}
}
void freadint(unsigned int& value, FILE* file)
{
	int rv = 0;
	for(int i=0;i<4;i++)
	{
		int r = 0;
		fread(&r, 1, 1, file);
		rv |= r << (i*8);
	}
	value = rv;
}

// writes all records to an already-open file
void LuaSaveData::ExportRecords(void* fileV) const
{
	FILE* file = (FILE*)fileV;
	if(!file)
		return;

	Record* cur = recordList;
	while(cur)
	{
		fwriteint(cur->key, file);
		fwriteint(cur->size, file);
		fwrite(cur->data, cur->size, 1, file);
		cur = cur->next;
	}
}

// reads records from an already-open file
void LuaSaveData::ImportRecords(void* fileV)
{
	FILE* file = (FILE*)fileV;
	if(!file)
		return;

	ClearRecords();

	Record rec;
	Record* cur = &rec;
	Record* last = NULL;
	while(1)
	{
		freadint(cur->key, file);
		freadint(cur->size, file);

		if(feof(file) || ferror(file))
			break;

		cur->data = new unsigned char [cur->size];
		fread(cur->data, cur->size, 1, file);

		Record* next = new Record();
		memcpy(next, cur, sizeof(Record));
		next->next = NULL;

		if(last)
			last->next = next;
		else
			recordList = next;
		last = next;
	}
}

void LuaSaveData::ClearRecords()
{
	Record* cur = recordList;
	while(cur)
	{
		Record* del = cur;
		cur = cur->next;

		delete[] del->data;
		delete del;
	}

	recordList = NULL;
}



void DontWorryLua() // everything's going to be OK
{
	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		dontworry(*iter->second);
		++iter;
	}
}

void EnableStopAllLuaScripts(bool enable)
{
	g_stopAllScriptsEnabled = enable;
}

void StopAllLuaScripts()
{
	if(!g_stopAllScriptsEnabled)
		return;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		bool wasStarted = info.started;
		StopLuaScript(uid);
		info.restartLater = wasStarted;
		++iter;
	}
}

void RestartAllLuaScripts()
{
	if(!g_stopAllScriptsEnabled)
		return;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		int uid = iter->first;
		LuaContextInfo& info = *iter->second;
		if(info.restartLater || info.started)
		{
			info.restartLater = false;
			RunLuaScriptFile(uid, info.lastFilename.c_str());
		}
		++iter;
	}
}

// sets anything that needs to depend on the total number of scripts running
void RefreshScriptStartedStatus()
{
	int numScriptsStarted = 0;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.started)
			numScriptsStarted++;
		++iter;
	}

	frameadvSkipLagForceDisable = (numScriptsStarted != 0); // disable while scripts are running because currently lag skipping makes lua callbacks get called twice per frame advance
	g_numScriptsStarted = numScriptsStarted;
}

// sets anything that needs to depend on speed mode or running status of scripts
void RefreshScriptSpeedStatus()
{
	g_anyScriptsHighSpeed = false;

	std::map<int, LuaContextInfo*>::const_iterator iter = luaContextInfo.begin();
	std::map<int, LuaContextInfo*>::const_iterator end = luaContextInfo.end();
	while(iter != end)
	{
		LuaContextInfo& info = *iter->second;
		if(info.running)
			if(info.speedMode == SPEEDMODE_TURBO || info.speedMode == SPEEDMODE_MAXIMUM)
				g_anyScriptsHighSpeed = true;
		++iter;
	}
}
