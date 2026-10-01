#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

static int PackF32(lua_State* state) {
    const float value = static_cast<float>(luaL_checknumber(state, 1));
    char bytes[sizeof(value)];
    static_assert(sizeof(value) == 4, "binary32 float required");
    std::memcpy(bytes, &value, sizeof(value));
    lua_pushlstring(state, bytes, sizeof(bytes));
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 4) return 64;
    lua_State* state = luaL_newstate();
    if (state == nullptr) return 65;
    luaL_openlibs(state);
    std::ifstream input(argv[1], std::ios::binary);
    const std::string request((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) { lua_close(state); return 67; }
    lua_pushlstring(state, request.data(), request.size());
    lua_setglobal(state, "TEST_REQUEST");
    const float negativeZero = -0.0f;
    lua_pushnumber(state, negativeZero);
    lua_setglobal(state, "TEST_NEGATIVE_ZERO");
    lua_newtable(state);
    lua_pushcfunction(state, PackF32);
    lua_setfield(state, -2, "PackF32");
    lua_setglobal(state, "VFS");
    lua_newtable(state);
    for (int index = 1; index <= 2; ++index) {
        lua_pushstring(state, argv[index]);
        lua_rawseti(state, -2, index);
    }
    lua_setglobal(state, "arg");
    const int status = luaL_dofile(state, argv[3]);
    if (status != 0) {
        const char* error = lua_tostring(state, -1);
        std::fprintf(stderr, "%s\n", error == nullptr ? "lua-error" : error);
    }
    lua_close(state);
    return status == 0 ? 0 : 66;
}
