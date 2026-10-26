// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "libaegisub/lua/script_reader.h"

#include "libaegisub/file_mapping.h"
#include "libaegisub/log.h"
#include "libaegisub/lua/utils.h"
#include "libaegisub/split.h"

#include <boost/algorithm/string/replace.hpp>
#include <lauxlib.h>

namespace agi::lua {
	namespace {
		// Wraps moonscript.loadstring with an on-disk compile cache. Called with
		// (moonscript module, cache dir, compiler stamp) and returns the
		// loadstring function to use.
		const char moonscript_cache_glue[] = R"LUA(
-- Arguments: moonscript module, cache directory ("" disables caching), compiler stamp
local moon, cache_dir, stamp = ...
if cache_dir == "" then return moon.loadstring end

local bit = require("bit")
local line_tables = require("moonscript.line_tables")
local to_lua = moon.to_lua
local lua_load = loadstring or load
local MAGIC = "AEGICACHE1"
local TRAILER = "END\n"

local function cache_path(chunkname)
	local h = 5381
	for i = 1, #chunkname do
		h = bit.band(h * 33 + chunkname:byte(i), 0xffffffff)
	end
	local base = (chunkname:match("([^/\\]+)$") or "chunk"):gsub("[^%w%._%-]", "_"):sub(-40)
	return cache_dir .. "/" .. bit.tohex(h) .. "-" .. base .. ".cache"
end

local function serialize_ltable(lt)
	local parts, n = {"return {"}, 1
	for k, v in pairs(lt) do
		if type(k) ~= "number" or type(v) ~= "number" then return nil end
		n = n + 1
		parts[n] = ("[%d]=%d,"):format(k, v)
	end
	parts[n + 1] = "}"
	return table.concat(parts)
end

-- Returns code, line_table on a valid hit for exactly this source text
local function read_entry(path, text)
	local f = io.open(path, "rb")
	if not f then return nil end
	local data = f:read("*a")
	f:close()
	if not data then return nil end

	local magic, st, a, b, c, body = data:match("^([^\n]*)\n([^\n]*)\n(%d+) (%d+) (%d+)\n()")
	if magic ~= MAGIC or st ~= stamp then return nil end
	a, b, c = tonumber(a), tonumber(b), tonumber(c)
	if #data ~= body - 1 + a + b + c + #TRAILER then return nil end
	if a ~= #text or data:sub(body, body + a - 1) ~= text then return nil end
	if data:sub(-#TRAILER) ~= TRAILER then return nil end

	local code = data:sub(body + a, body + a + b - 1)
	local lt_src = data:sub(body + a + b, body + a + b + c - 1)
	local fn = lua_load(lt_src, "=moonscript_cache")
	if not fn then return nil end
	local lt = fn()
	if type(lt) ~= "table" then return nil end
	return code, lt
end

local function write_entry(path, text, code, lt)
	local lt_src = serialize_ltable(lt)
	if not lt_src then return end

	local tmp = ("%s.%s%x.tmp"):format(path, tostring({}):match("0x(%x+)") or "0", math.floor(os.clock() * 1e6))
	local f = io.open(tmp, "wb")
	if not f then return end
	local ok = f:write(MAGIC, "\n", stamp, "\n", #text, " ", #code, " ", #lt_src, "\n",
		text, code, lt_src, TRAILER)
	f:close()
	if not ok then os.remove(tmp) return end
	if not os.rename(tmp, path) then
		-- Windows will not rename over an existing file
		os.remove(path)
		if not os.rename(tmp, path) then os.remove(tmp) end
	end
end

return function(text, chunkname)
	chunkname = chunkname or "=(moonscript.loadstring)"

	local path = cache_path(chunkname)
	local ok, code, lt = pcall(read_entry, path, text)
	if ok and code then
		local fn = lua_load(code, chunkname)
		if fn then
			line_tables[chunkname] = lt
			return fn
		end
	end

	local code, lt_or_err = to_lua(text)
	if not code then return nil, lt_or_err end
	line_tables[chunkname] = lt_or_err
	pcall(write_entry, path, text, code, lt_or_err)
	return lua_load(code, chunkname)
end
)LUA";

		bool load_support_file(lua_State *L, fs::path const& filename) {
			try {
				return LoadFile(L, filename);
			}
			catch (agi::Exception const& e) {
				lua_pushstring(L, e.GetMessage().c_str());
				return false;
			}
		}
	}

	bool LoadFile(lua_State *L, agi::fs::path const& raw_filename) {
		auto filename = raw_filename;
		try {
			filename = agi::fs::Canonicalize(raw_filename);
		}
		catch (agi::fs::FileSystemUnknownError const& e) {
			LOG_E("auto4/lua") << "Error canonicalizing path: " << e.GetMessage();
		}

		agi::read_file_mapping file(filename);
		auto buff = file.read();
		auto size = static_cast<size_t>(file.size());

		// Discard the BOM if present
		if (size >= 3 && buff[0] == '\xef' && buff[1] == '\xbb' && buff[2] == '\xbf') {
			buff += 3;
			size -= 3;
		}

		if (!agi::fs::HasExtension(filename, "moon"))
			return luaL_loadbuffer(L, buff, size, filename.string().c_str()) == 0;

		// We have a MoonScript file, so we need to load it with that
		// It might be nice to have a dedicated lua state for compiling
		// MoonScript to Lua
		lua_getfield(L, LUA_REGISTRYINDEX, "moonscript");

		// Save the text we'll be loading for the line number rewriting in the
		// error handling
		lua_pushlstring(L, buff, size);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, ("raw moonscript: " + filename.string()).c_str());

		push_value(L, filename);
		if (lua_pcall(L, 2, 2, 0))
			return false; // Leaves error message on stack

		// loadstring returns nil, error on error or a function on success
		if (lua_isnil(L, -2)) {
			lua_remove(L, -2);
			return false;
		}

		lua_pop(L, 1); // Remove the extra nil for the stackchecker
		return true;
	}

	static int module_loader(lua_State *L) {
		int pretop = lua_gettop(L);
		std::string module(check_string(L, -1));
		boost::replace_all(module, ".", LUA_DIRSEP);

		// Get the lua package include path (which the user may have modified)
		lua_getglobal(L, "package");
		lua_getfield(L, -1, "path");
		std::string package_paths(check_string(L, -1));
		lua_pop(L, 2);

		for (auto tok : agi::Split(package_paths, ';')) {
			std::string filename;
			boost::replace_all_copy(std::back_inserter(filename), tok, "?", module);

			// If there's a .moon file at that path, load it instead of the
			// .lua file
			agi::fs::path path = filename;
			if (agi::fs::HasExtension(path, "lua")) {
				agi::fs::path moonpath = path;
				moonpath.replace_extension("moon");
				if (agi::fs::FileExists(moonpath))
					path = moonpath;
			}

			if (!agi::fs::FileExists(path))
				continue;

			try {
				if (!LoadFile(L, path))
					return error(L, "Error loading Lua module \"%s\":\n%s", path.string().c_str(), check_string(L, -1).c_str());
				break;
			}
			catch (agi::fs::FileNotFound const&) {
				// Not an error so swallow and continue on
			}
			catch (agi::fs::NotAFile const&) {
				// Not an error so swallow and continue on
			}
			catch (agi::Exception const& e) {
				return error(L, "Error loading Lua module \"%s\":\n%s", path.string().c_str(), e.GetMessage().c_str());
			}
		}

		return lua_gettop(L) - pretop;
	}

	bool Install(lua_State *L, std::vector<fs::path> const& include_path,
		fs::path const& support_path, fs::path const& cache_dir) {
		// set the module load path to include_path
		lua_getglobal(L, "package");
		push_value(L, "path");

		push_value(L, "");
		for (auto const& path : include_path) {
			lua_pushfstring(L, "%s/?.lua;%s/?/init.lua;", path.string().c_str(), path.string().c_str());
			lua_concat(L, 2);
		}

#ifndef _WIN32
		// No point in checking any of the default locations on Windows since
		// there won't be anything there
		push_value(L, "path");
		lua_gettable(L, -4);
		lua_concat(L, 2);
#endif

		lua_settable(L, -3);

		// Replace the default lua module loader with our unicode compatible one
		lua_getfield(L, -1, "loaders");
		push_value(L, exception_wrapper<module_loader>);
		lua_rawseti(L, -2, 2);
		lua_pop(L, 2); // loaders, package

#ifdef _WIN32
		// Replace the default lua IO functions with our unicode compatible ones
		if (!load_support_file(L, support_path / "unicode-monkeypatch.lua"))
			return false;
		if (lua_pcall(L, 0, 0, 0)) {
			return false; // leave error message
		}
#endif

		// Load mandatory support by exact path. In particular, do not allow a
		// moonscript.lua beside an approved subtitle-local script to run first.
		if (!load_support_file(L, support_path / "moonscript.lua"))
			return false;
		if (lua_pcall(L, 0, 1, 0)) {
			return false; // leave error message
		}
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			lua_pushliteral(L, "Bundled moonscript.lua did not return a module table");
			return false;
		}

		lua_getfield(L, -1, "loadstring");
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 2);
			lua_pushliteral(L, "Bundled moonscript.lua has no loadstring function");
			return false;
		}
		lua_pop(L, 1);

		// The cache is invalidated whenever the bundled compiler changes
		std::string stamp = "v1";
		try {
			auto compiler = support_path / "moonscript.lua";
			stamp += "-" + std::to_string(agi::fs::Size(compiler));
			stamp += "-" + std::to_string(agi::fs::ModifiedTime(compiler).time_since_epoch().count());
		}
		catch (agi::Exception const&) {
			// Without a reliable stamp, don't risk serving stale output
			stamp += "-nostamp";
		}

		// Build the loadstring function (cached when cache_dir is set)
		if (luaL_loadbuffer(L, moonscript_cache_glue, sizeof(moonscript_cache_glue) - 1, "=moonscript_cache") != 0) {
			lua_remove(L, -2); // module, leaving only the error message
			return false;
		}
		lua_pushvalue(L, -2); // moonscript module
		if (cache_dir.empty())
			push_value(L, std::string());
		else
			push_value(L, cache_dir);
		push_value(L, stamp);
		if (lua_pcall(L, 3, 1, 0)) {
			lua_remove(L, -2); // module, leaving only the error message
			return false;
		}
		if (!lua_isfunction(L, -1)) {
			lua_pop(L, 2);
			lua_pushliteral(L, "MoonScript cache setup did not return a loadstring function");
			return false;
		}
		lua_setfield(L, LUA_REGISTRYINDEX, "moonscript");

		// Direct loading bypasses require(), so record the trusted module as
		// loaded before user code gets control.
		lua_getglobal(L, "package");
		lua_getfield(L, -1, "loaded");
		lua_pushvalue(L, -3);
		lua_setfield(L, -2, "moonscript");
		lua_pop(L, 3); // loaded, package, module

		return true;
	}
}
