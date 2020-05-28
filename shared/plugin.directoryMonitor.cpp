#include "CoronaAssert.h"
#include "CoronaEvent.h"
#include "CoronaLua.h"
#include "CoronaLibrary.h"
#include <stdio.h>
#include <string.h>
#define DMON_IMPL
#define _DMON_LOG_ERRORF
#include "dmon.h"
#include "readerwriterqueue.h"
#include <thread>

// ----------------------------------------------------------------------------
using namespace std;

namespace Corona
{
	// ----------------------------------------------------------------------------

	struct EventData
	{
		int watchID;
		const char* action;
		const char* filePath;
		const char* previousFilePath;
		const char* rootDirectory;
	};

	class DirectoryMonitorLibrary
	{
	public:
		typedef DirectoryMonitorLibrary Self;

	public:
		static const char kName[];
		static const char kEventName[];
		static EventData eventData;
		static moodycamel::ReaderWriterQueue<EventData> data;

	public:
		static int Open(lua_State* L);
		static int Finalizer(lua_State* L);
		static Self* ToLibrary(lua_State* L);

	protected:
		DirectoryMonitorLibrary();
		bool Initialize(void* platformContext);

	public:
		static int watch(lua_State* L);
		static int unwatch(lua_State* L);
		static int processFrame(lua_State* L);
	};

	// ----------------------------------------------------------------------------

	// This corresponds to the name of the library, e.g. [Lua] require "plugin.library"
	const char DirectoryMonitorLibrary::kName[] = "plugin.directoryMonitor";
	const char DirectoryMonitorLibrary::kEventName[] = "directoryMonitor";
	EventData DirectoryMonitorLibrary::eventData = {};
	moodycamel::ReaderWriterQueue<EventData> DirectoryMonitorLibrary::data(1);
	static int callbackRef = 0;

	int DirectoryMonitorLibrary::Open(lua_State* L)
	{
		// Register __gc callback
		const char kMetatableName[] = __FILE__; // Globally unique string to prevent collision
		CoronaLuaInitializeGCMetatable(L, kMetatableName, Finalizer);

		//CoronaLuaInitializeGCMetatable( L, kMetatableName, Finalizer );
		void* platformContext = CoronaLuaGetContext(L);

		// Set library as upvalue for each library function
		Self* library = new Self;

		if (library->Initialize(platformContext))
		{
			// Functions in library
			static const luaL_Reg kFunctions[] =
			{
				{ "watch", watch },
				{ "unwatch", unwatch },
				{ NULL, NULL }
			};

			// Register functions as closures, giving each access to the
			// 'library' instance via ToLibrary()
			{
				CoronaLuaPushUserdata(L, library, kMetatableName);
				luaL_openlib(L, kName, kFunctions, 1); // leave "library" on top of stack
			}

			// Now invoke above from C:
			// Lua stack order (from lowest index to highest):
			//   f
			//   Runtime
			//   "enterFrame"
			//   ProcessFrame (closure)
			{
				CoronaLuaPushRuntime(L);				 // push 'Runtime'
				lua_getfield(L, -1, "addEventListener"); // push 'f', i.e. Runtime.addEventListener
				lua_insert(L, -2);						 // swap so 'f' is below 'Runtime'
				lua_pushstring(L, "enterFrame");

				// Push ProcessFrame as closure so it has access
				lua_pushlightuserdata(L, library);
				lua_pushcclosure(L, &processFrame, 1);

				lua_pushvalue(L, -1);
				callbackRef = luaL_ref(L, LUA_REGISTRYINDEX); // r = clo
			}
			CoronaLuaDoCall(L, 3, 0);
		}

		return 1;
	}

	int DirectoryMonitorLibrary::Finalizer(lua_State* L)
	{
		Self* library = (Self*)CoronaLuaToUserdata(L, 1);
		dmon_deinit();

		CoronaLuaPushRuntime(L); // push 'Runtime'

		if (lua_type(L, -1) == LUA_TTABLE)
		{
			lua_getfield(L, -1, "removeEventListener"); // push 'f', i.e. Runtime.addEventListener
			lua_insert(L, -2);							// swap so 'f' is below 'Runtime'
			lua_pushstring(L, "enterFrame");
			lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef); // pushes closure
			CoronaLuaDoCall(L, 3, 0);
			luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
		}
		else
		{
			lua_pop(L, 1); // pop nil
		}

		if (library)
		{
			delete library;
		}

		return 0;
	}

	DirectoryMonitorLibrary* DirectoryMonitorLibrary::ToLibrary(lua_State* L)
	{
		// library is pushed as part of the closure
		Self* library = (Self*)CoronaLuaToUserdata(L, lua_upvalueindex(1));
		return library;
	}

	DirectoryMonitorLibrary::DirectoryMonitorLibrary()
	{
	}

	bool DirectoryMonitorLibrary::Initialize(void* platformContext)
	{
		dmon_init();
		return 1;
	}

	// ----------------------------------------------------------------------------

	static void watchCallback(dmon_watch_id watchID, dmon_action action, const char* rootDir,
		const char* filePath, const char* previousFilePath, void* user)
	{
		thread writer([&]()
			{
				DirectoryMonitorLibrary::eventData.watchID = watchID.id;
				DirectoryMonitorLibrary::eventData.filePath = filePath;
				DirectoryMonitorLibrary::eventData.rootDirectory = rootDir;
				DirectoryMonitorLibrary::eventData.previousFilePath = previousFilePath;

				switch (action)
				{
				case DMON_ACTION_CREATE:
					DirectoryMonitorLibrary::eventData.action = "create";
					break;

				case DMON_ACTION_DELETE:
					DirectoryMonitorLibrary::eventData.action = "delete";
					break;

				case DMON_ACTION_MODIFY:
					DirectoryMonitorLibrary::eventData.action = "modify";
					break;

				case DMON_ACTION_MOVE:
					DirectoryMonitorLibrary::eventData.action = "move";
					break;

				default:
					DirectoryMonitorLibrary::eventData.action = "unknown";
					break;
				}

				DirectoryMonitorLibrary::data.enqueue(DirectoryMonitorLibrary::eventData);
				this_thread::sleep_for(chrono::milliseconds(10));
			});

		writer.join();
	}

	int DirectoryMonitorLibrary::processFrame(lua_State* L)
	{
		thread reader([&]()
			{
				EventData eData;
				bool canDequeue = DirectoryMonitorLibrary::data.try_dequeue(eData);

				if (canDequeue)
				{
					CoronaLuaPushRuntime(L);			  // push 'Runtime'
					lua_getfield(L, -1, "dispatchEvent"); // push 'f', i.e. Runtime.dispatchEvent
					lua_insert(L, -2);					  // swap so 'f' is below 'Runtime'

					CoronaLuaNewEvent(L, DirectoryMonitorLibrary::kEventName);

					lua_pushnumber(L, eData.watchID);
					lua_setfield(L, -2, "watchID");

					lua_pushstring(L, eData.action);
					lua_setfield(L, -2, "action");

					lua_pushstring(L, eData.rootDirectory);
					lua_setfield(L, -2, "rootDirectory");

					lua_pushstring(L, eData.filePath);
					lua_setfield(L, -2, "filePath");

					lua_pushstring(L, eData.previousFilePath);
					lua_setfield(L, -2, "previousFilePath");

					lua_pushvalue(L, -3);
					lua_call(L, 3, 0); // Call Runtime.dispatchEvent() with 3 arguments (runtime, eventName, event table)
				}
			});

		reader.join();

		return 0;
	}

	//
	int DirectoryMonitorLibrary::watch(lua_State* L)
	{
		const char* filePath;

		if (lua_isstring(L, 1))
		{
			filePath = lua_tostring(L, 1);

			if (filePath != NULL)
			{
				dmon_watch_id handle = dmon_watch(filePath, watchCallback, DMON_WATCHFLAGS_RECURSIVE, NULL);
				lua_pushnumber(L, handle.id);
				return 1;
			}
		}
		else
		{
			CoronaLuaError(L, "directoryMonitor.watch() filePath (string) expected, got: %s", lua_typename(L, 1));
			lua_pushnil(L);
			return 0;
		}

		lua_pushnil(L);
		return 0;
	}

	int DirectoryMonitorLibrary::unwatch(lua_State* L)
	{
		dmon_watch_id handle;

		if (lua_isnumber(L, 1))
		{
			handle.id = (uint32_t)lua_tonumber(L, 1);
			dmon_unwatch(handle);
		}
		else
		{
			CoronaLuaError(L, "directoryMonitor.unwatch() handle (number) expected, got: %s", lua_typename(L, 1));
			lua_pushnil(L);
			return 0;
		}

		return 0;
	}

	// ----------------------------------------------------------------------------

} // namespace Corona

// ----------------------------------------------------------------------------

CORONA_EXPORT
int luaopen_plugin_directoryMonitor(lua_State* L)
{
	return Corona::DirectoryMonitorLibrary::Open(L);
}
