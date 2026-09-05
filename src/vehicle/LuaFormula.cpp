#include "vehicle/LuaFormula.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>

#ifdef WIZ_HAVE_LUAJIT
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <luajit.h>
}
#endif

namespace wizengine {
namespace vehicle {

#ifdef WIZ_HAVE_LUAJIT

namespace {

// FFI 配列を 3 本作り、レジストリに繋ぐためのテーブルと、各配列のアドレスを
// 返す。アドレスは intptr_t → double で運ぶ（x64 のポインタは 2^48 未満
// なので double で正確に表せる）。
const char* kNewBuffers =
    "local ffi = require('ffi')\n"
    "return function(nI, nO, nS)\n"
    "  local I = ffi.new('double[?]', math.max(nI, 1))\n"
    "  local O = ffi.new('double[?]', math.max(nO, 1))\n"
    "  local S = ffi.new('double[?]', math.max(nS, 1))\n"
    "  return I, O, S, tonumber(ffi.cast('intptr_t', I)),\n"
    "         tonumber(ffi.cast('intptr_t', O)), tonumber(ffi.cast('intptr_t', S))\n"
    "end\n";

// コンパイル済み関数（プログラム 1 個につき 1 回）。実体はこれを共有する。
struct CompiledChunk {
    int fnRef = LUA_NOREF;
};

class LuaInstance : public FormulaInstance {
public:
    LuaInstance(lua_State* L, std::shared_ptr<const FormulaProgram> program,
                std::shared_ptr<CompiledChunk> chunk)
        : L_(L), program_(std::move(program)), chunk_(std::move(chunk)) {}

    ~LuaInstance() override {
        if (!L_) return;
        luaL_unref(L_, LUA_REGISTRYINDEX, refI_);
        luaL_unref(L_, LUA_REGISTRYINDEX, refO_);
        luaL_unref(L_, LUA_REGISTRYINDEX, refS_);
    }

    bool init(int newBuffersRef, std::string& error) {
        lua_rawgeti(L_, LUA_REGISTRYINDEX, newBuffersRef);
        lua_pushinteger(L_, program_->inputCount());
        lua_pushinteger(L_, program_->outputCount());
        lua_pushinteger(L_, program_->stateCount());
        if (lua_pcall(L_, 3, 6, 0) != 0) {
            error = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "buffer allocation failed";
            lua_pop(L_, 1);
            return false;
        }
        in_ = reinterpret_cast<double*>(std::uintptr_t(lua_tonumber(L_, -3)));
        out_ = reinterpret_cast<double*>(std::uintptr_t(lua_tonumber(L_, -2)));
        state_ = reinterpret_cast<double*>(std::uintptr_t(lua_tonumber(L_, -1)));
        lua_pop(L_, 3);
        refS_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        refO_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        refI_ = luaL_ref(L_, LUA_REGISTRYINDEX);
        reset();
        return true;
    }

    void reset() override {
        for (int i = 0; i < program_->stateCount(); ++i) state_[i] = 0.0;
    }
    const char* backend() const override { return "luajit"; }

    bool eval(const double* in, double* out, double dt) override {
        const int ni = program_->inputCount();
        const int no = program_->outputCount();
        for (int i = 0; i < ni; ++i) in_[i] = in[i];
        for (int i = 0; i < no; ++i) out_[i] = 0.0;
        lua_rawgeti(L_, LUA_REGISTRYINDEX, chunk_->fnRef);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, refI_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, refO_);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, refS_);
        lua_pushnumber(L_, dt);
        if (lua_pcall(L_, 4, 0, 0) != 0) {
            lastError_ = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "lua error";
            lua_pop(L_, 1);
            return false;
        }
        bool finite = true;
        for (int i = 0; i < no; ++i) {
            out[i] = out_[i];
            if (!std::isfinite(out[i])) finite = false;
        }
        return finite;
    }

private:
    lua_State* L_;
    std::shared_ptr<const FormulaProgram> program_;
    std::shared_ptr<CompiledChunk> chunk_;
    int refI_ = LUA_NOREF, refO_ = LUA_NOREF, refS_ = LUA_NOREF;
    double* in_ = nullptr;
    double* out_ = nullptr;
    double* state_ = nullptr;
    std::string lastError_;
};

// プログラム → コンパイル済み関数のキャッシュ（ステートごと）。
std::map<const FormulaProgram*, std::weak_ptr<CompiledChunk>>& chunkCache(lua_State* L) {
    static std::map<lua_State*, std::map<const FormulaProgram*, std::weak_ptr<CompiledChunk>>> all;
    return all[L];
}

}  // namespace

bool LuaRuntime::available() { return true; }

LuaRuntime::LuaRuntime() {
    L_ = luaL_newstate();
    if (!L_) return;
    luaL_openlibs(L_);
    if (luaL_loadstring(L_, kNewBuffers) != 0 || lua_pcall(L_, 0, 1, 0) != 0) {
        lua_close(L_);
        L_ = nullptr;
        return;
    }
    newBuffersRef_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

LuaRuntime::~LuaRuntime() {
    if (L_) {
        chunkCache(L_).clear();
        lua_close(L_);
    }
}

std::string LuaRuntime::version() const { return LUAJIT_VERSION; }

bool LuaRuntime::jitEnabled() const {
    if (!L_) return false;
    lua_getglobal(L_, "jit");
    if (!lua_istable(L_, -1)) {
        lua_pop(L_, 1);
        return false;
    }
    lua_getfield(L_, -1, "status");
    if (lua_pcall(L_, 0, 1, 0) != 0) {
        lua_pop(L_, 2);
        return false;
    }
    const bool on = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 2);
    return on;
}

std::unique_ptr<FormulaInstance> LuaRuntime::instantiate(
    const std::shared_ptr<const FormulaProgram>& program, std::string& error) {
    if (!L_ || !program) {
        error = "lua runtime is not available";
        return nullptr;
    }
    auto& cache = chunkCache(L_);
    std::shared_ptr<CompiledChunk> chunk = cache[program.get()].lock();
    if (!chunk) {
        const std::string src = program->luaSource();
        const std::string name = "=formula:" + program->name();
        if (luaL_loadbuffer(L_, src.data(), src.size(), name.c_str()) != 0 ||
            lua_pcall(L_, 0, 1, 0) != 0) {
            error = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "compile failed";
            lua_pop(L_, 1);
            return nullptr;
        }
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 1);
            error = "generated chunk did not return a function";
            return nullptr;
        }
        chunk = std::make_shared<CompiledChunk>();
        chunk->fnRef = luaL_ref(L_, LUA_REGISTRYINDEX);
        cache[program.get()] = chunk;
        // 参照が切れたら関数も手放す（プログラムごと消えたとき）。
    }
    auto inst = std::make_unique<LuaInstance>(L_, program, chunk);
    if (!inst->init(newBuffersRef_, error)) return nullptr;
    return inst;
}

bool LuaRuntime::evalNumber(const std::string& chunk, double& out, std::string& error) {
    if (!L_) {
        error = "no state";
        return false;
    }
    if (luaL_loadstring(L_, chunk.c_str()) != 0 || lua_pcall(L_, 0, 1, 0) != 0) {
        error = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "error";
        lua_pop(L_, 1);
        return false;
    }
    out = lua_tonumber(L_, -1);
    lua_pop(L_, 1);
    return true;
}

#else  // ---- LuaJIT 無しのビルド ---------------------------------------------

bool LuaRuntime::available() { return false; }
LuaRuntime::LuaRuntime() {}
LuaRuntime::~LuaRuntime() {}
std::string LuaRuntime::version() const { return "(no luajit)"; }
bool LuaRuntime::jitEnabled() const { return false; }
std::unique_ptr<FormulaInstance> LuaRuntime::instantiate(
    const std::shared_ptr<const FormulaProgram>&, std::string& error) {
    error = "this build has no LuaJIT (WIZ_HAVE_LUAJIT)";
    return nullptr;
}
bool LuaRuntime::evalNumber(const std::string&, double&, std::string& error) {
    error = "this build has no LuaJIT";
    return false;
}

#endif

}  // namespace vehicle
}  // namespace wizengine
